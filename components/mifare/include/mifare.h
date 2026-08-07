/**
 * @file mifare.h
 * @brief Mifare / ISO14443-A driver for MFRC522-compatible readers.
 *
 * Targets the NZ3801-AB clone fitted on the ES242F lock main board and
 * genuine MFRC522 (RC522 modules) on the bench. All timing/config values
 * replicate the OEM firmware behavior, proven by SPI sniffer captures and
 * scope measurements (see PROJECT_NOTES.md, Stage 2).
 *
 * Design notes:
 *  - The driver is low-level and stateless regarding *policy*: it never
 *    decides when to poll. The application (lock_app / console harness)
 *    owns the polling loop.
 *  - mifare_power_down() drives NRSTPD low AND parks all SPI pins LOW:
 *    the reader shares the SPI bus with the ZB25VQ64C flash, and parked
 *    pins avoid parasitic feeding of the powered-down reader (OEM parity).
 *  - MFAuthent with the OEM fixed key is vestigial in the original firmware
 *    (always fails, result ignored). It is implemented here as
 *    mifare_auth() for OUR card scheme, not for OEM parity.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "hal/gpio_types.h"
#include "hal/spi_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Constants
 * ------------------------------------------------------------------------- */

/** OEM SPI clock measured on scope (cs_clk_6mhz.png). */
#define MIFARE_DEFAULT_SPI_CLOCK_HZ   6000000

/** Pass as expected_version to skip the VersionReg check. */
#define MIFARE_VERSION_SKIP_CHECK     0x00

/** Known VersionReg values. */
#define MIFARE_VERSION_MFRC522_V1     0x91    /**< Genuine MFRC522 v1.0 (RC522 modules) */
#define MIFARE_VERSION_MFRC522_V2     0x92    /**< Genuine MFRC522 v2.0 */
#define MIFARE_VERSION_NZ3801_AB      0xB2    /**< NZ3801-AB clone (lock main board) */

#define MIFARE_MAX_UID_LEN            10

/* ---------------------------------------------------------------------------
 * Types
 * ------------------------------------------------------------------------- */

typedef struct {
    spi_host_device_t spi_host;     /**< SPI2_HOST recommended on ESP32-S3 */
    gpio_num_t mosi_pin;
    gpio_num_t miso_pin;
    gpio_num_t sclk_pin;
    gpio_num_t cs_pin;
    gpio_num_t rst_pin;             /**< NRSTPD: reader hard power-down/reset */
    gpio_num_t irq_pin;             /**< Optional; pass GPIO_NUM_NC (OEM never uses it) */
    int spi_clock_hz;               /**< 0 -> 6 MHz (OEM parity) */
    uint8_t expected_version;       /**< VersionReg self-test; MIFARE_VERSION_SKIP_CHECK to disable */
} mifare_config_t;

typedef struct {
    uint8_t uid[MIFARE_MAX_UID_LEN];
    uint8_t uid_len;                /**< 4, 7 or 10 bytes */
    uint8_t sak;                    /**< SAK of the last cascade level */
    uint16_t atqa;
} mifare_card_t;

typedef enum {
    MIFARE_KEY_A = 0x60,
    MIFARE_KEY_B = 0x61,
} mifare_key_type_t;

/* ---------------------------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------------------------- */

/**
 * @brief Initialize the driver: GPIOs, SPI bus, power up the reader, run the
 *        full OEM init sequence and verify VersionReg.
 *
 * Leaves the reader powered and ready for mifare_wupa()/mifare_anticoll_select().
 *
 * @return ESP_ERR_INVALID_RESPONSE if VersionReg reads 0x00/0xFF (comms dead)
 *         or does not match cfg->expected_version (when not SKIP_CHECK).
 */
esp_err_t mifare_init(const mifare_config_t *cfg);
esp_err_t mifare_deinit(void);

/**
 * @brief Full power-up: bus + NRSTPD HIGH + SoftReset + complete OEM init
 *        table. Idempotent; safe to call on an already powered reader
 *        (doubles as the OEM "card-detected branch" re-init / recovery).
 */
esp_err_t mifare_power_up(void);

/**
 * @brief Hard power-down: NRSTPD LOW, SPI device removed, all SPI pins
 *        driven LOW (anti parasitic-feeding, frees the shared flash bus).
 */
esp_err_t mifare_power_down(void);

/**
 * @brief True while the reader is powered and the SPI bus is attached.
 *        False after mifare_power_down() — e.g. between 1 Hz poll cycles,
 *        which power-cycle the reader for OEM parity. Card/register
 *        commands must re-power (mifare_power_up()) before touching the
 *        bus, or they fail with ESP_ERR_INVALID_STATE.
 */
bool mifare_is_awake(void);

/**
 * @brief Lightweight wake for 1 Hz poll cycles: bus + NRSTPD HIGH +
 *        stabilization delay, WITHOUT re-init. Follow with
 *        mifare_idle_burst() (which writes the config subset, exactly like
 *        the OEM does after a hard power-down cycle).
 */
esp_err_t mifare_wake(void);

/** @brief SoftReset command + wait until the command field self-clears. */
esp_err_t mifare_soft_reset(void);

/** @brief Read VersionReg. */
esp_err_t mifare_version(uint8_t *version);

/* ---------------------------------------------------------------------------
 * Raw register access (public: needed by the console for bring-up/debug of
 * clone-specific registers like Reserved 0x23/0x25)
 * ------------------------------------------------------------------------- */

esp_err_t mifare_read_reg(uint8_t reg, uint8_t *value);
esp_err_t mifare_write_reg(uint8_t reg, uint8_t value);

/* ---------------------------------------------------------------------------
 * OEM idle poll burst (exact replica for scope A/B and the 1 Hz hot loop)
 * ------------------------------------------------------------------------- */

/**
 * @brief One OEM-style idle burst: config subset (ControlReg/ModeReg/
 *        Reserved 0x23/0x25/AutoTest/TxASK/TxControl=0x03), WUPA issued in
 *        OEM order (Transceive -> FIFO push -> StartSend), then Status2Reg
 *        ModemState polling.
 *
 * @param card_detected  true if a card responded (ModemState reached
 *                       "receiving" or ATQA bytes pending in FIFO).
 * @param final_state    last ModemState value observed (debug/scope aid).
 */
esp_err_t mifare_idle_burst(bool *card_detected, uint8_t *final_state);

/* ---------------------------------------------------------------------------
 * ISO14443-3
 * ------------------------------------------------------------------------- */

esp_err_t mifare_wupa(uint16_t *atqa);  /**< 0x52 — wakes cards from HALT (OEM choice) */
esp_err_t mifare_reqa(uint16_t *atqa);  /**< 0x26 — does NOT wake cards from HALT */

/**
 * @brief Anticollision + Select with cascade support (CL1/CL2/CL3).
 *
 * NOTE (documented deviation): the OEM ignores cascade level 2 and reports
 * 7-byte cards as "88 xx yy zz". This driver resolves the full UID — a strict
 * improvement, harmless for our own 4-byte-UID credential cards.
 */
esp_err_t mifare_anticoll_select(mifare_card_t *card);

/** @brief HLTA. A no-response timeout is treated as success (per spec). */
esp_err_t mifare_halt(void);

/* ---------------------------------------------------------------------------
 * Mifare Classic (for OUR token scheme — sector 1, derived keys)
 * ------------------------------------------------------------------------- */

/**
 * @brief MFAuthent against a block. On success Crypto1On is set and all
 *        further traffic is encrypted until cleared/HALT.
 *
 * @param uid  First 4 UID bytes of the selected card (from mifare_card_t).
 * @return ESP_OK if Status2Reg.Crypto1On is set, ESP_FAIL otherwise.
 */
esp_err_t mifare_auth(uint8_t block, mifare_key_type_t key_type,
                      const uint8_t *key, const uint8_t *uid);

esp_err_t mifare_read_block(uint8_t block, uint8_t data[16]);
esp_err_t mifare_write_block(uint8_t block, const uint8_t data[16]);

#ifdef __cplusplus
}
#endif
