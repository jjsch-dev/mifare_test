/**
 * @file mifare.c
 * @brief Driver core: lifecycle, SPI transport, ISO14443-3, OEM parity helpers.
 */
#include "mifare.h"
#include "mifare_regs.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_rom_sys.h"

static const char *TAG = "mifare";

/* Hard power-down recovery: NRSTPD HIGH -> oscillator stable (datasheet >1 ms) */
#define MIFARE_POWER_UP_DELAY_MS        2
/* SoftReset self-clear timeout */
#define MIFARE_RESET_TIMEOUT_MS         50
/* Transceive deadline: pure software window (see transceive wait note) */
#define MIFARE_TRX_TIMEOUT_MS           50
/* OEM idle burst ModemState watch window */
#define MIFARE_BURST_WATCH_MS           15

typedef struct {
    mifare_config_t cfg;
    spi_device_handle_t dev;
    bool initialized;
    bool bus_ready;
} mifare_state_t;

static mifare_state_t s;

/* ========================================================================= */
/*                              SPI bus management                           */
/* ========================================================================= */

static esp_err_t bus_acquire(void)
{
    if (s.bus_ready) {
        return ESP_OK;
    }

    const spi_bus_config_t buscfg = {
        .mosi_io_num   = s.cfg.mosi_pin,
        .miso_io_num   = s.cfg.miso_pin,
        .sclk_io_num   = s.cfg.sclk_pin,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 64,
    };
    esp_err_t err = spi_bus_initialize(s.cfg.spi_host, &buscfg, SPI_DMA_DISABLED);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(err));
        return err;
    }

    const spi_device_interface_config_t devcfg = {
        .clock_speed_hz = s.cfg.spi_clock_hz,
        .mode           = 0,             /* MFRC522: CPOL=0 CPHA=0, MSB first */
        .spics_io_num   = s.cfg.cs_pin,
        .queue_size     = 1,
    };
    err = spi_bus_add_device(s.cfg.spi_host, &devcfg, &s.dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_add_device failed: %s", esp_err_to_name(err));
        spi_bus_free(s.cfg.spi_host);
        return err;
    }

    s.bus_ready = true;
    return ESP_OK;
}

/**
 * @brief Remove the SPI device and park every bus pin LOW.
 *
 * The reader shares the bus with the ZB25VQ64C flash. With NRSTPD LOW the
 * OEM drives all SPI pins LOW to avoid parasitically feeding the reader
 * through its I/O clamps and to keep flash traffic clean. OEM parity.
 */
static void bus_release_pins_low(void)
{
    if (s.bus_ready) {
        spi_bus_remove_device(s.dev);
        spi_bus_free(s.cfg.spi_host);
        s.bus_ready = false;
    }

    const gpio_num_t pins[] = { s.cfg.mosi_pin, s.cfg.miso_pin,
                                s.cfg.sclk_pin, s.cfg.cs_pin };
    for (size_t i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
        gpio_reset_pin(pins[i]);
        gpio_set_direction(pins[i], GPIO_MODE_OUTPUT);
        gpio_set_level(pins[i], 0);
    }
}

/* ========================================================================= */
/*                              Register access                              */
/* ========================================================================= */

/* MFRC522 SPI framing: byte0 = (addr << 1) | read_flag, MSB first. */

esp_err_t mifare_write_reg(uint8_t reg, uint8_t value)
{
    if (!s.bus_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    spi_transaction_t t = {
        .length = 16,
        .flags  = SPI_TRANS_USE_TXDATA,
        .tx_data = { (uint8_t)((reg << 1) & 0x7E), value },
    };
    return spi_device_transmit(s.dev, &t);
}

esp_err_t mifare_read_reg(uint8_t reg, uint8_t *value)
{
    if (!s.bus_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    spi_transaction_t t = {
        .length   = 16,
        .rxlength = 16,
        .flags    = SPI_TRANS_USE_TXDATA | SPI_TRANS_USE_RXDATA,
        .tx_data  = { (uint8_t)(((reg << 1) & 0x7E) | 0x80), 0x00 },
    };
    esp_err_t err = spi_device_transmit(s.dev, &t);
    if (err == ESP_OK) {
        *value = t.rx_data[1];
    }
    return err;
}

/** Push bytes into the FIFO (single SPI frame, address byte + payload). */
esp_err_t mifare_priv_fifo_write(const uint8_t *data, uint8_t len)
{
    uint8_t buf[20];    /* largest payload here: 16B block write + cmd/addr */
    if ((size_t)len + 1 > sizeof(buf)) {
        return ESP_ERR_INVALID_SIZE;
    }
    buf[0] = (MFRC522_REG_FIFO_DATA << 1) & 0x7E;
    memcpy(&buf[1], data, len);

    spi_transaction_t t = {
        .length    = 8 * ((size_t)len + 1),
        .tx_buffer = buf,
    };
    return spi_device_transmit(s.dev, &t);
}

/**
 * Pop bytes from the FIFO. MFRC522 expects the read address repeated for
 * every byte except the last, which is a 0x00 dummy.
 */
static esp_err_t fifo_read(uint8_t *data, uint8_t len)
{
    uint8_t tx[20], rx[20];
    if ((size_t)len + 1 > sizeof(tx)) {
        return ESP_ERR_INVALID_SIZE;
    }
    const uint8_t addr_rd = ((MFRC522_REG_FIFO_DATA << 1) & 0x7E) | 0x80;
    memset(tx, addr_rd, len);
    tx[len] = 0x00;

    spi_transaction_t t = {
        .length    = 8 * ((size_t)len + 1),
        .rxlength  = 8 * ((size_t)len + 1),
        .tx_buffer = tx,
        .rx_buffer = rx,
    };
    esp_err_t err = spi_device_transmit(s.dev, &t);
    if (err == ESP_OK) {
        memcpy(data, &rx[1], len);
    }
    return err;
}

/* ========================================================================= */
/*                              Internal helpers                             */
/* ========================================================================= */

/** Enable/disable hardware CRC on TX and RX (Type A CRC-A).
 *  Shared within the component (mifare_classic.c). */
static bool s_crc_on;   /* tracked for the air-time estimate in transceive */

esp_err_t mifare_priv_set_crc(bool enable)
{
    const uint8_t v = enable ? 0x80 : 0x00;
    esp_err_t err = mifare_write_reg(MFRC522_REG_TX_MODE, v);
    if (err == ESP_OK) {
        err = mifare_write_reg(MFRC522_REG_RX_MODE, v);
    }
    if (err == ESP_OK) {
        s_crc_on = enable;
    }
    return err;
}

/**
 * @brief Transceive primitive (classic MFRC522 ordering, OEM-parity wait).
 *
 * Flush -> FIFO load -> Transceive -> StartSend -> poll FIFOLevelReg ->
 * ErrorReg check -> drain FIFO.
 *
 * Wait strategy: poll FIFOLevelReg until a response byte lands, with a pure
 * software deadline. This is exactly what the OEM firmware does on the real
 * lock (sniffer proof: WUPA/anticoll/select are all followed by FIFOLevelReg
 * reads, never by ComIrqReg waits or timer setup). Bench evidence on the
 * NZ3801-class clone showed why the OEM chose this: ComIrqReg IRQs and the
 * TAuto hardware timer are unreliable on this silicon (RxIRq asserted but
 * the command never terminated; TimerIRq never fired in a 50 ms window).
 *
 * On timeout the Transceive is aborted with Idle + FIFO flush so the chip
 * never lingers in a half-active modem state.
 *
 * @param tx_last_bits  0 = full bytes; 7 for REQA/WUPA short frames.
 * @return ESP_ERR_TIMEOUT when no card answered within the deadline.
 */
esp_err_t mifare_priv_transceive(const uint8_t *tx, uint8_t tx_len,
                            uint8_t *rx, uint8_t rx_cap, uint8_t *rx_len,
                            uint8_t tx_last_bits)
{
    esp_err_t err;

    err = mifare_write_reg(MFRC522_REG_COMMAND, MFRC522_CMD_IDLE);
    if (err != ESP_OK) return err;
    err = mifare_write_reg(MFRC522_REG_FIFO_LEVEL, MFRC522_FIFO_FLUSH);
    if (err != ESP_OK) return err;
    err = mifare_priv_fifo_write(tx, tx_len);
    if (err != ESP_OK) return err;
    err = mifare_write_reg(MFRC522_REG_COM_IRQ, MFRC522_IRQ_CLEAR_ALL);
    if (err != ESP_OK) return err;
    err = mifare_write_reg(MFRC522_REG_COMMAND, MFRC522_CMD_TRANSCEIVE);
    if (err != ESP_OK) return err;
    err = mifare_write_reg(MFRC522_REG_BIT_FRAMING,
                           MFRC522_BITFRAMING_START_SEND | (tx_last_bits & 0x07));
    if (err != ESP_OK) return err;

    /* OEM-parity wait: response bytes landing in the FIFO are the only
       trustworthy "card answered" signal on this silicon. Wait for the full
       expected frame (rx_cap), not just the first byte — the second ATQA byte
       lands ~85 us after the first and an early cut reads a partial frame
       (bench evidence: FIFOLevel=1, single byte 0x44, ESP_ERR_INVALID_SIZE).
       A short pause between polls keeps 6 MHz SPI bursts from flooding the
       RF receive window (the OEM polls with gaps too). */
    uint8_t level = 0;
    const uint8_t target = (rx && rx_cap > 0) ? rx_cap : 1;
    const int64_t deadline = esp_timer_get_time() + (int64_t)MIFARE_TRX_TIMEOUT_MS * 1000;

    /* Self-reception wipe (bench evidence, NZ3801-class clone): the receiver
       comes up during our own transmission and the FIFO fills with loopback
       bytes BEFORE the card could possibly answer (SELECT: 6 bytes present
       ~150 us into a ~770 us frame; HLTA: 1 phantom byte). One single flush,
       timed right after our frame leaves the air, wipes the loopback safely:
       loopback has settled by TX-end + ~94 us, and the card's first byte
       cannot land before TX-end + FDT (~86 us) + one byte time (~94 us).
       NEVER flush later than this: doing it mid-reception kills in-flight
       card bytes and stalls the clone's receiver (bench evidence). */
    const uint32_t tx_bits = ((tx_last_bits & 0x07) != 0)
                                 ? ((uint32_t)(tx_len - 1) * 9 + (tx_last_bits & 0x07) + 1)
                                 : ((uint32_t)tx_len * 9)
                               + (s_crc_on ? 18 : 0);   /* CRC-A on air, with parity */
    esp_rom_delay_us(tx_bits * 944 / 100 + 100);   /* 9.44 us/ETU + margin */
    err = mifare_write_reg(MFRC522_REG_FIFO_LEVEL, MFRC522_FIFO_FLUSH);
    if (err != ESP_OK) return err;

    do {
        err = mifare_read_reg(MFRC522_REG_FIFO_LEVEL, &level);
        if (err != ESP_OK) return err;
        level &= 0x7F;
        if (level >= target) {
            break;
        }
        if (level > 0) {
            /* Partial frame and the modem already finished -> short frame. */
            uint8_t cmd = 0;
            err = mifare_read_reg(MFRC522_REG_COMMAND, &cmd);
            if (err != ESP_OK) return err;
            if ((cmd & 0x0F) == MFRC522_CMD_IDLE) {
                break;
            }
        }
        esp_rom_delay_us(100);
    } while (esp_timer_get_time() < deadline);

    /* Leave the modem in a clean state whether it self-completed or not. */
    mifare_write_reg(MFRC522_REG_COMMAND, MFRC522_CMD_IDLE);

    if (level == 0) {
        mifare_write_reg(MFRC522_REG_FIFO_LEVEL, MFRC522_FIFO_FLUSH);
        return ESP_ERR_TIMEOUT;
    }

    /* ErrorReg is diagnostic-only here (the OEM firmware never reads it; on
       the clone these flags can be unreliable). Collision stays fatal because
       anticollision semantics depend on it. */
    uint8_t err_reg = 0;
    err = mifare_read_reg(MFRC522_REG_ERROR, &err_reg);
    if (err != ESP_OK) return err;
    if (err_reg & MFRC522_ERR_COLL) {
        ESP_LOGW(TAG, "collision detected (single-card bench expected)");
        return ESP_FAIL;
    }
    if (err_reg & (MFRC522_ERR_PROTOCOL | MFRC522_ERR_PARITY | MFRC522_ERR_BUFFER_OVFL)) {
        ESP_LOGW(TAG, "ErrorReg=0x%02X (diagnostic only, proceeding)", err_reg);
    }

    if (rx && rx_cap > 0) {
        if (level > rx_cap) {
            /* More bytes than expected: don't fail blind. Drain the whole
               FIFO and dump it — on the NZ3801-class clone extra bytes appear
               on CRC-enabled transactions (bench evidence: 6 bytes after
               SELECT CL1, 1 phantom byte after HLTA) and their content is
               diagnostic gold. The caller still gets its rx_cap bytes. */
            uint8_t dump[64];
            uint8_t extra = level;
            err = fifo_read(dump, extra);
            if (err != ESP_OK) return err;
            ESP_LOGW(TAG, "FIFO holds %u bytes (expected %u):", level, rx_cap);
            ESP_LOG_BUFFER_HEX(TAG, dump, extra);
            memcpy(rx, dump, rx_cap);
            level = rx_cap;
        } else {
            err = fifo_read(rx, level);
            if (err != ESP_OK) return err;
        }
    }
    if (rx_len) {
        *rx_len = level;
    }
    return ESP_OK;
}

/** Full OEM init table (post-SoftReset), captured order and values. */
static esp_err_t oem_full_init(void)
{
    static const struct { uint8_t reg; uint8_t val; } k_init[] = {
        { MFRC522_REG_MOD_WIDTH,    OEM_MOD_WIDTH },
        { MFRC522_REG_TX_ASK,       OEM_TX_ASK },
        { MFRC522_REG_GS_N,         OEM_GS_N },
        { MFRC522_REG_CW_GS_P,      OEM_CW_GS_P },
        { MFRC522_REG_CONTROL,      OEM_CONTROL },
        { MFRC522_REG_RF_CFG,       OEM_RF_CFG },
        { MFRC522_REG_RX_THRESHOLD, OEM_RX_THRESHOLD },
        { MFRC522_REG_TX_CONTROL,   OEM_TX_CONTROL_FULL },   /* antenna ON */
        { MFRC522_REG_TX_MODE,      0x00 },                  /* CRC off */
        { MFRC522_REG_RX_MODE,      0x00 },
        { MFRC522_REG_RESERVED_23,  OEM_RESERVED_23 },       /* clone trim */
        { MFRC522_REG_RESERVED_25,  OEM_RESERVED_25 },
    };
    for (size_t i = 0; i < sizeof(k_init) / sizeof(k_init[0]); i++) {
        esp_err_t err = mifare_write_reg(k_init[i].reg, k_init[i].val);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}

/* ========================================================================= */
/*                              Lifecycle                                    */
/* ========================================================================= */

esp_err_t mifare_soft_reset(void)
{
    esp_err_t err = mifare_write_reg(MFRC522_REG_COMMAND, MFRC522_CMD_SOFT_RESET);
    if (err != ESP_OK) {
        return err;
    }
    /* Command field self-clears when the reset completes. */
    const int64_t deadline = esp_timer_get_time() + (int64_t)MIFARE_RESET_TIMEOUT_MS * 1000;
    uint8_t cmd;
    do {
        err = mifare_read_reg(MFRC522_REG_COMMAND, &cmd);
        if (err != ESP_OK) {
            return err;
        }
        if ((cmd & 0x0F) != MFRC522_CMD_SOFT_RESET) {
            return ESP_OK;
        }
    } while (esp_timer_get_time() < deadline);
    return ESP_ERR_TIMEOUT;
}

esp_err_t mifare_wake(void)
{
    if (!s.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = bus_acquire();
    if (err != ESP_OK) {
        return err;
    }
    gpio_set_level(s.cfg.rst_pin, 1);
    vTaskDelay(pdMS_TO_TICKS(MIFARE_POWER_UP_DELAY_MS));
    return ESP_OK;
}

esp_err_t mifare_power_up(void)
{
    esp_err_t err = mifare_wake();
    if (err != ESP_OK) {
        return err;
    }
    err = mifare_soft_reset();
    if (err != ESP_OK) {
        return err;
    }
    return oem_full_init();
}

esp_err_t mifare_power_down(void)
{
    if (!s.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    gpio_set_level(s.cfg.rst_pin, 0);   /* NRSTPD LOW: hard power-down */
    bus_release_pins_low();
    return ESP_OK;
}

esp_err_t mifare_version(uint8_t *version)
{
    return mifare_read_reg(MFRC522_REG_VERSION, version);
}

esp_err_t mifare_init(const mifare_config_t *cfg)
{
    if (cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(&s, 0, sizeof(s));
    s.cfg = *cfg;
    if (s.cfg.spi_clock_hz <= 0) {
        s.cfg.spi_clock_hz = MIFARE_DEFAULT_SPI_CLOCK_HZ;
    }

    /* NRSTPD starts LOW (reader powered down until power_up). */
    gpio_reset_pin(s.cfg.rst_pin);
    gpio_set_direction(s.cfg.rst_pin, GPIO_MODE_OUTPUT);
    gpio_set_level(s.cfg.rst_pin, 0);

    s.initialized = true;

    esp_err_t err = mifare_power_up();
    if (err != ESP_OK) {
        return err;
    }

    /* Self-test: VersionReg. 0x00/0xFF = dead SPI; mismatch = warn-worthy. */
    uint8_t ver = 0;
    err = mifare_version(&ver);
    if (err != ESP_OK) {
        return err;
    }
    if (ver == 0x00 || ver == 0xFF) {
        ESP_LOGE(TAG, "VersionReg=0x%02X — SPI link dead?", ver);
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (s.cfg.expected_version != MIFARE_VERSION_SKIP_CHECK &&
        ver != s.cfg.expected_version) {
        ESP_LOGE(TAG, "VersionReg=0x%02X, expected 0x%02X", ver, s.cfg.expected_version);
        return ESP_ERR_INVALID_RESPONSE;
    }
    ESP_LOGI(TAG, "reader online: VersionReg=0x%02X (%s) @ %d Hz SPI",
             ver,
             ver == MIFARE_VERSION_NZ3801_AB ? "NZ3801-AB" :
             ver == MIFARE_VERSION_MFRC522_V1 ? "MFRC522 v1" :
             ver == MIFARE_VERSION_MFRC522_V2 ? "MFRC522 v2" : "unknown clone",
             s.cfg.spi_clock_hz);
    return ESP_OK;
}

esp_err_t mifare_deinit(void)
{
    if (!s.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    mifare_power_down();
    gpio_reset_pin(s.cfg.rst_pin);
    memset(&s, 0, sizeof(s));
    return ESP_OK;
}

/* ========================================================================= */
/*                    OEM idle burst (1 Hz poll cycle body)                  */
/* ========================================================================= */

esp_err_t mifare_idle_burst(bool *card_detected, uint8_t *final_state)
{
    if (!s.bus_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err;
    uint8_t dummy;

    /* OEM config subset (no SoftReset after hard power-down: regs are at
       defaults; the burst rewrites exactly what it needs). Captured order. */
    err = mifare_read_reg(MFRC522_REG_COMMAND, &dummy);            /* expect 0x20 */
    if (err != ESP_OK) return err;

    static const struct { uint8_t reg; uint8_t val; } k_burst[] = {
        { MFRC522_REG_CONTROL,      OEM_CONTROL },
        { MFRC522_REG_MODE,         OEM_MODE },
        { MFRC522_REG_RESERVED_23,  OEM_RESERVED_23 },
        { MFRC522_REG_RESERVED_25,  OEM_RESERVED_25 },
        { MFRC522_REG_AUTO_TEST,    OEM_AUTO_TEST },
        { MFRC522_REG_TX_ASK,       OEM_TX_ASK },
        { MFRC522_REG_COMMAND,      MFRC522_CMD_IDLE },
        { MFRC522_REG_TX_CONTROL,   OEM_TX_CONTROL_BURST },  /* antenna ON, no InvTx2 */
        { MFRC522_REG_BIT_FRAMING,  0x07 },
        { MFRC522_REG_COM_IRQ,      MFRC522_IRQ_CLEAR_ALL },
        { MFRC522_REG_DIV_IRQ,      MFRC522_IRQ_CLEAR_ALL },
        { MFRC522_REG_FIFO_LEVEL,   MFRC522_FIFO_FLUSH },
    };
    for (size_t i = 0; i < sizeof(k_burst) / sizeof(k_burst[0]); i++) {
        err = mifare_write_reg(k_burst[i].reg, k_burst[i].val);
        if (err != ESP_OK) return err;
    }

    /* OEM ordering: arm Transceive on an empty FIFO, then push WUPA, then StartSend. */
    err = mifare_write_reg(MFRC522_REG_COMMAND, MFRC522_CMD_TRANSCEIVE);
    if (err != ESP_OK) return err;
    const uint8_t wupa = MIFARE_CMD_WUPA;
    err = mifare_priv_fifo_write(&wupa, 1);
    if (err != ESP_OK) return err;
    err = mifare_write_reg(MFRC522_REG_BIT_FRAMING,
                           MFRC522_BITFRAMING_START_SEND | 0x07);
    if (err != ESP_OK) return err;

    /* ModemState watch: OEM-observed 03 -> 04 -> 05 with no card;
       06 = card responding. */
    bool detected = false;
    uint8_t state = 0, prev = 0xFF;
    const int64_t deadline = esp_timer_get_time() + (int64_t)MIFARE_BURST_WATCH_MS * 1000;
    do {
        err = mifare_read_reg(MFRC522_REG_STATUS2, &state);
        if (err != ESP_OK) return err;
        state &= MFRC522_STATUS2_MODEM_MASK;
        if (state != prev) {
            ESP_LOGD(TAG, "ModemState %u -> %u", prev, state);
            prev = state;
        }
        if (state == MFRC522_MODEM_RX_RECEIVING) {
            detected = true;
        }
        if (state == MFRC522_MODEM_IDLE) {
            break;
        }
    } while (esp_timer_get_time() < deadline);

    /* Robustness fallback: ATQA bytes already waiting in the FIFO. */
    uint8_t level = 0;
    err = mifare_read_reg(MFRC522_REG_FIFO_LEVEL, &level);
    if (err != ESP_OK) return err;
    if (level >= 2) {
        detected = true;
    }

    if (card_detected) {
        *card_detected = detected;
    }
    if (final_state) {
        *final_state = state;
    }
    return ESP_OK;
}

/* ========================================================================= */
/*                              ISO14443-3                                   */
/* ========================================================================= */

static esp_err_t reqx(uint8_t cmd, uint16_t *atqa)
{
    esp_err_t err = mifare_priv_set_crc(false);      /* REQA/WUPA: no CRC (OEM parity) */
    if (err != ESP_OK) return err;

    uint8_t resp[2];
    uint8_t n = 0;
    err = mifare_priv_transceive(&cmd, 1, resp, sizeof(resp), &n, 7 /* 7-bit short frame */);
    if (err != ESP_OK) {
        return err;                             /* ESP_ERR_TIMEOUT = no card */
    }
    if (n != 2) {
        ESP_LOGW(TAG, "REQA/WUPA: expected 2 ATQA bytes, got %u (0x%02X%02X)",
                 n, n > 0 ? resp[0] : 0, n > 1 ? resp[1] : 0);
        return ESP_ERR_INVALID_SIZE;
    }
    if (atqa) {
        *atqa = (uint16_t)resp[0] | ((uint16_t)resp[1] << 8);
    }
    return ESP_OK;
}

esp_err_t mifare_wupa(uint16_t *atqa)
{
    return reqx(MIFARE_CMD_WUPA, atqa);
}

esp_err_t mifare_reqa(uint16_t *atqa)
{
    return reqx(MIFARE_CMD_REQA, atqa);
}

esp_err_t mifare_anticoll_select(mifare_card_t *card)
{
    if (card == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(card, 0, sizeof(*card));

    uint8_t cascade_cmd = MIFARE_CMD_ANTICOLL_CL1;

    for (int level = 0; level < 3; level++) {
        /* --- Anticollision (CRC off) --- */
        esp_err_t err = mifare_priv_set_crc(false);
        if (err != ESP_OK) return err;

        const uint8_t ac[2] = { cascade_cmd, 0x20 };
        uint8_t resp[5];
        uint8_t n = 0;
        err = mifare_priv_transceive(ac, sizeof(ac), resp, sizeof(resp), &n, 0);
        if (err != ESP_OK) return err;
        if (n != 5) {
            return ESP_ERR_INVALID_SIZE;
        }
        const uint8_t bcc = resp[0] ^ resp[1] ^ resp[2] ^ resp[3];
        if (bcc != resp[4]) {
            ESP_LOGW(TAG, "BCC mismatch: calc 0x%02X vs recv 0x%02X", bcc, resp[4]);
            return ESP_ERR_INVALID_CRC;
        }

        /* --- Select (CRC on; chip appends/verifies CRC-A) --- */
        err = mifare_priv_set_crc(true);
        if (err != ESP_OK) return err;

        uint8_t sel[7] = { cascade_cmd, MIFARE_CMD_SELECT,
                           resp[0], resp[1], resp[2], resp[3], resp[4] };
        uint8_t sak = 0;
        uint8_t sn = 0;
        err = mifare_priv_transceive(sel, sizeof(sel), &sak, 1, &sn, 0);
        if (err != ESP_OK) return err;
        if (sn != 1) {
            return ESP_ERR_INVALID_SIZE;
        }
        card->sak = sak;

        /* --- UID accumulation (CT 0x88 = 3 UID bytes this level) --- */
        const uint8_t *uid_part = resp;
        uint8_t uid_part_len = 4;
        if (resp[0] == MIFARE_CASCADE_TAG) {
            uid_part = &resp[1];
            uid_part_len = 3;
        }
        if (card->uid_len + uid_part_len > MIFARE_MAX_UID_LEN) {
            return ESP_ERR_INVALID_SIZE;
        }
        memcpy(&card->uid[card->uid_len], uid_part, uid_part_len);
        card->uid_len += uid_part_len;

        if ((sak & 0x04) == 0) {
            return ESP_OK;      /* UID complete */
        }
        cascade_cmd = (cascade_cmd == MIFARE_CMD_ANTICOLL_CL1)
                          ? MIFARE_CMD_ANTICOLL_CL2
                          : MIFARE_CMD_ANTICOLL_CL3;
    }
    return ESP_ERR_INVALID_RESPONSE;    /* cascade bit still set after CL3 */
}

esp_err_t mifare_halt(void)
{
    esp_err_t err = mifare_priv_set_crc(true);
    if (err != ESP_OK) return err;

    const uint8_t hlta[2] = { MIFARE_CMD_HLTA, 0x00 };
    err = mifare_priv_transceive(hlta, sizeof(hlta), NULL, 0, NULL, 0);
    /* HLTA has no response by spec: timeout means the card got it. */
    if (err == ESP_ERR_TIMEOUT) {
        return ESP_OK;
    }
    return err;
}
