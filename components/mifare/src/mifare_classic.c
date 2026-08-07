/**
 * @file mifare_classic.c
 * @brief Mifare Classic operations: MFAuthent, block read/write.
 *
 * Used for OUR credential scheme (sector 1, derived keys, structured token).
 * The OEM's fixed-key auth is vestigial (always fails, result ignored) — see
 * PROJECT_NOTES.md Stage 2 findings.
 */
#include "mifare.h"
#include "mifare_regs.h"

#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "mifare_classic";

#define MFAUTHENT_TIMEOUT_MS   25
#define MIFARE_BLOCK_SIZE      16

/* mifare.c internals shared within the component (not part of the public API) */
esp_err_t mifare_priv_transceive(const uint8_t *tx, uint8_t tx_len,
                                 uint8_t *rx, uint8_t rx_cap, uint8_t *rx_len,
                                 uint8_t tx_last_bits);
esp_err_t mifare_priv_set_crc(bool enable);
esp_err_t mifare_priv_fifo_write(const uint8_t *data, uint8_t len);

/**
 * Wait for the MFAuthent command to complete.
 *
 * OEM parity: the lock firmware polls CommandReg until the chip returns to
 * Idle (sniffer proof: CommandReg read = 0x20 right after MFAuthent), never
 * ComIrqReg — bench evidence showed IdleIRq/TimerIRq are unreliable on this
 * clone silicon. On timeout the command is aborted with Idle.
 */
static esp_err_t wait_command_done(uint32_t timeout_ms)
{
    const int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    uint8_t cmd = 0;
    do {
        esp_err_t err = mifare_read_reg(MFRC522_REG_COMMAND, &cmd);
        if (err != ESP_OK) {
            return err;
        }
        if ((cmd & 0x0F) == MFRC522_CMD_IDLE) {
            return ESP_OK;
        }
    } while (esp_timer_get_time() < deadline);

    mifare_write_reg(MFRC522_REG_COMMAND, MFRC522_CMD_IDLE);
    mifare_write_reg(MFRC522_REG_FIFO_LEVEL, MFRC522_FIFO_FLUSH);
    return ESP_ERR_TIMEOUT;
}

esp_err_t mifare_auth(uint8_t block, mifare_key_type_t key_type,
                      const uint8_t *key, const uint8_t *uid)
{
    if (key == NULL || uid == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err;

    /* Clear Crypto1On before authenticating (deliberate OEM step — parity). */
    err = mifare_write_reg(MFRC522_REG_STATUS2, 0x00);
    if (err != ESP_OK) return err;
    err = mifare_priv_set_crc(true);
    if (err != ESP_OK) return err;

    /* FIFO: auth cmd | block | key[6] | uid[4] */
    uint8_t buf[12];
    buf[0] = (uint8_t)key_type;
    buf[1] = block;
    memcpy(&buf[2], key, 6);
    memcpy(&buf[8], uid, 4);

    err = mifare_write_reg(MFRC522_REG_COMMAND, MFRC522_CMD_IDLE);
    if (err != ESP_OK) return err;
    err = mifare_write_reg(MFRC522_REG_FIFO_LEVEL, MFRC522_FIFO_FLUSH);
    if (err != ESP_OK) return err;
    err = mifare_priv_fifo_write(buf, sizeof(buf));
    if (err != ESP_OK) return err;
    err = mifare_write_reg(MFRC522_REG_COM_IRQ, MFRC522_IRQ_CLEAR_ALL);
    if (err != ESP_OK) return err;
    err = mifare_write_reg(MFRC522_REG_COMMAND, MFRC522_CMD_MFAUTHENT);
    if (err != ESP_OK) return err;
    /* OEM writes BitFraming=0x80 right after MFAuthent — replicated for parity. */
    err = mifare_write_reg(MFRC522_REG_BIT_FRAMING, MFRC522_BITFRAMING_START_SEND);
    if (err != ESP_OK) return err;

    err = wait_command_done(MFAUTHENT_TIMEOUT_MS);
    if (err != ESP_OK) {
        return err;
    }

    uint8_t status2 = 0;
    err = mifare_read_reg(MFRC522_REG_STATUS2, &status2);
    if (err != ESP_OK) return err;
    if ((status2 & MFRC522_STATUS2_CRYPTO1_ON) == 0) {
        ESP_LOGW(TAG, "auth failed (block %u, key %s) — Crypto1On not set",
                 block, key_type == MIFARE_KEY_A ? "A" : "B");
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t mifare_read_block(uint8_t block, uint8_t data[16])
{
    if (data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = mifare_priv_set_crc(true);
    if (err != ESP_OK) return err;

    const uint8_t cmd[2] = { MIFARE_CMD_READ, block };
    uint8_t n = 0;
    err = mifare_priv_transceive(cmd, sizeof(cmd), data, MIFARE_BLOCK_SIZE, &n, 0);
    if (err != ESP_OK) return err;
    if (n != MIFARE_BLOCK_SIZE) {
        ESP_LOGW(TAG, "read block %u: got %u bytes, expected 16", block, n);
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

/** Wait for the 4-bit ACK (0xA) after a write-phase command. */
static esp_err_t expect_ack(const char *phase)
{
    uint8_t ack = 0, n = 0;
    esp_err_t err = mifare_read_reg(MFRC522_REG_FIFO_LEVEL, &n);
    if (err != ESP_OK) return err;
    if (n != 1) {
        ESP_LOGW(TAG, "write %s: no ACK (FIFOLevel=%u)", phase, n);
        return ESP_FAIL;
    }
    err = mifare_read_reg(MFRC522_REG_FIFO_DATA, &ack);
    if (err != ESP_OK) return err;
    if ((ack & 0x0F) != MIFARE_ACK) {
        ESP_LOGW(TAG, "write %s: NACK 0x%02X", phase, ack);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t mifare_write_block(uint8_t block, const uint8_t data[16])
{
    if (data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    /* Block 0 is manufacturer data (read-only on genuine cards) and block
       trailers hold keys/access bits — guard the obvious foot-guns. */
    if (block == 0 || (block % 4) == 3) {
        ESP_LOGE(TAG, "refusing to write block %u (manufacturer/trailer)", block);
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = mifare_priv_set_crc(true);
    if (err != ESP_OK) return err;

    /* Phase 1: WRITE command + block address -> 4-bit ACK */
    const uint8_t cmd[2] = { MIFARE_CMD_WRITE, block };
    err = mifare_priv_transceive(cmd, sizeof(cmd), NULL, 0, NULL, 0);
    if (err != ESP_OK) return err;
    err = expect_ack("cmd");
    if (err != ESP_OK) return err;

    /* Phase 2: 16 data bytes -> 4-bit ACK */
    err = mifare_priv_transceive(data, MIFARE_BLOCK_SIZE, NULL, 0, NULL, 0);
    if (err != ESP_OK) return err;
    return expect_ack("data");
}
