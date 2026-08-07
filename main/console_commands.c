/**
 * @file console_commands.c
 * @brief ESP Console CLI commands (argtable3) for the mifare driver.
 *
 * Command layers:
 *   1. Lifecycle / self-test : init, deinit, power, version
 *   2. ISO14443-3 step by step: wupa, reqa, select, halt
 *   3. Mifare Classic        : auth, read, write
 *   4. OEM parity / debug    : burst, poll, reg, dump
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_console.h"
#include "argtable3/argtable3.h"
#include "esp_log.h"
#include "mifare.h"
#include "console_commands.h"

static const char *TAG = "CMD";

/* ========================================================================= */
/*                              STATE                                        */
/* ========================================================================= */

static bool s_driver_ready = false;
static mifare_card_t s_last_card;   /* cached by `select`, consumed by auth/read/write */
static bool s_card_selected = false;

/* OEM vestigial fixed key — used by `poll` to replicate the observed probe. */
static const uint8_t k_oem_probe_key[6] = { 0x04, 0x01, 0x07, 0x00, 0x05, 0x09 };

/* ========================================================================= */
/*                              ARGTABLES                                    */
/* ========================================================================= */

static struct {
    struct arg_int *mosi, *miso, *clk, *cs, *rst, *irq, *freq, *ver;
    struct arg_end *end;
} init_args;

static struct { struct arg_int *state; struct arg_end *end; } power_args;

static struct {
    struct arg_int *block;
    struct arg_str *key_type;
    struct arg_str *key;
    struct arg_end *end;
} auth_args;

static struct { struct arg_int *block; struct arg_end *end; } read_args;

static struct {
    struct arg_int *block;
    struct arg_str *data;
    struct arg_end *end;
} write_args;

static struct {
    struct arg_int *count, *period;
    struct arg_end *end;
} poll_args;

static struct {
    struct arg_str *op;
    struct arg_int *addr, *value;
    struct arg_end *end;
} reg_args;

/* ========================================================================= */
/*                              HELPERS                                      */
/* ========================================================================= */

static bool require_ready(const char *cmd)
{
    if (!s_driver_ready) {
        ESP_LOGE(TAG, "%s: driver not initialized — run 'init' first", cmd);
        return false;
    }
    return true;
}

/* Card/register commands need the reader awake. poll/burst power-cycle it
   (OEM parity: NRSTPD LOW + SPI bus released between bursts), so the first
   command after a poll used to die with ESP_ERR_INVALID_STATE. Auto-recover:
   mifare_power_up() is idempotent and doubles as the OEM recovery path.
   burst/poll deliberately do NOT use this — they manage the wake themselves
   so their waveform preamble stays byte-faithful for the scope A/B. */
static bool require_awake(const char *cmd)
{
    if (!require_ready(cmd)) {
        return false;
    }
    if (!mifare_is_awake()) {
        esp_err_t ret = mifare_power_up();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "%s: reader asleep and power-up failed: %s",
                     cmd, esp_err_to_name(ret));
            return false;
        }
        ESP_LOGI(TAG, "%s: reader was asleep (power-cycled by poll/burst) — powered up", cmd);
    }
    return true;
}

static bool require_card(const char *cmd)
{
    if (!require_awake(cmd)) {
        return false;
    }
    if (!s_card_selected) {
        ESP_LOGE(TAG, "%s: no card selected — run 'select' first", cmd);
        return false;
    }
    return true;
}

static int hex_to_bytes(const char *hex, uint8_t *out, size_t out_len)
{
    if (strlen(hex) != out_len * 2) {
        return -1;
    }
    for (size_t i = 0; i < out_len; i++) {
        unsigned v;
        if (sscanf(hex + 2 * i, "%2x", &v) != 1) {
            return -1;
        }
        out[i] = (uint8_t)v;
    }
    return 0;
}

static void print_card(const mifare_card_t *card)
{
    char uid_str[2 * MIFARE_MAX_UID_LEN + 1] = {0};
    for (int i = 0; i < card->uid_len; i++) {
        sprintf(uid_str + 2 * i, "%02X", card->uid[i]);
    }
    ESP_LOGI(TAG, "UID: %s (%d bytes) | SAK: 0x%02X | ATQA: 0x%04X",
             uid_str, card->uid_len, card->sak, card->atqa);
}

/* ========================================================================= */
/*                         LAYER 1 — LIFECYCLE                               */
/* ========================================================================= */

static int cmd_init(int argc, char **argv)
{
    if (arg_parse(argc, argv, (void **)&init_args) != 0) {
        arg_print_errors(stderr, init_args.end, argv[0]);
        return 1;
    }

    if (s_driver_ready) {
        ESP_LOGW(TAG, "driver already initialized — running deinit first");
        mifare_deinit();
        s_driver_ready = false;
        s_card_selected = false;
    }

    mifare_config_t cfg = {
        .spi_host         = SPI2_HOST,
        .mosi_pin         = init_args.mosi->ival[0],
        .miso_pin         = init_args.miso->ival[0],
        .sclk_pin         = init_args.clk->ival[0],
        .cs_pin           = init_args.cs->ival[0],
        .rst_pin          = init_args.rst->ival[0],
        .irq_pin          = init_args.irq->count > 0 ? init_args.irq->ival[0] : GPIO_NUM_NC,
        .spi_clock_hz     = init_args.freq->count > 0 ? init_args.freq->ival[0] : 0,
        .expected_version = init_args.ver->count > 0 ? (uint8_t)init_args.ver->ival[0]
                                                     : MIFARE_VERSION_SKIP_CHECK,
    };

    esp_err_t ret = mifare_init(&cfg);
    if (ret == ESP_OK) {
        s_driver_ready = true;
        ESP_LOGI(TAG, "mifare driver initialized (MOSI:%d MISO:%d CLK:%d CS:%d RST:%d)",
                 cfg.mosi_pin, cfg.miso_pin, cfg.sclk_pin, cfg.cs_pin, cfg.rst_pin);
    } else {
        ESP_LOGE(TAG, "mifare_init failed: %s", esp_err_to_name(ret));
    }
    return 0;
}

static int cmd_deinit(int argc, char **argv)
{
    if (!require_ready("deinit")) {
        return 1;
    }
    mifare_deinit();
    s_driver_ready = false;
    s_card_selected = false;
    ESP_LOGI(TAG, "driver deinitialized (reader powered down, pins parked)");
    return 0;
}

static int cmd_power(int argc, char **argv)
{
    if (arg_parse(argc, argv, (void **)&power_args) != 0) {
        arg_print_errors(stderr, power_args.end, argv[0]);
        return 1;
    }
    if (!require_ready("power")) {
        return 1;
    }

    esp_err_t ret = power_args.state->ival[0]
                        ? mifare_power_up()
                        : mifare_power_down();
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "reader power: %s", power_args.state->ival[0] ? "UP" : "DOWN");
    } else {
        ESP_LOGE(TAG, "power command failed: %s", esp_err_to_name(ret));
    }
    return 0;
}

static int cmd_version(int argc, char **argv)
{
    if (!require_awake("version")) {
        return 1;
    }
    uint8_t ver = 0;
    esp_err_t ret = mifare_version(&ver);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "VersionReg = 0x%02X (0x91/0x92 = MFRC522, 0xB2 = NZ3801-AB)", ver);
    } else {
        ESP_LOGE(TAG, "version read failed: %s", esp_err_to_name(ret));
    }
    return 0;
}

/* ========================================================================= */
/*                      LAYER 2 — ISO14443-3 STEPS                           */
/* ========================================================================= */

static int do_reqx(const char *name, bool wakeup)
{
    if (!require_awake(name)) {
        return 1;
    }
    uint16_t atqa = 0;
    esp_err_t ret = wakeup ? mifare_wupa(&atqa) : mifare_reqa(&atqa);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "%s OK — ATQA = 0x%04X", name, atqa);
    } else if (ret == ESP_ERR_TIMEOUT) {
        ESP_LOGW(TAG, "%s: no card in the field", name);
    } else {
        ESP_LOGE(TAG, "%s failed: %s", name, esp_err_to_name(ret));
    }
    return 0;
}

static int cmd_wupa(int argc, char **argv) { return do_reqx("wupa", true); }
static int cmd_reqa(int argc, char **argv) { return do_reqx("reqa", false); }

static int cmd_select(int argc, char **argv)
{
    if (!require_awake("select")) {
        return 1;
    }

    /* State-tolerant selection (bench aid, NOT OEM behavior): on the bench
       the RF field stays on between commands, so the card keeps its ISO14443-3
       state (the OEM never sees this — it gates the field between bursts and
       the card always boots into IDLE). Cover every state:
         HLTA  -> normalizes ACTIVE to HALT (ignored elsewhere)
         WUPA  -> works from IDLE/HALT (ignored from READY: not fatal)
         anticoll -> works from READY, which every path above guarantees. */
    mifare_halt();   /* result deliberately ignored */

    uint16_t atqa = 0;
    esp_err_t ret = mifare_wupa(&atqa);
    if (ret == ESP_ERR_TIMEOUT) {
        ESP_LOGI(TAG, "select: WUPA unanswered (card already READY?), trying anticollision");
    } else if (ret != ESP_OK) {
        ESP_LOGE(TAG, "select: wupa failed: %s", esp_err_to_name(ret));
        s_card_selected = false;
        return 0;
    }

    ret = mifare_anticoll_select(&s_last_card);
    if (ret == ESP_ERR_TIMEOUT) {
        ESP_LOGW(TAG, "select: no card in the field");
        s_card_selected = false;
        return 0;
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "anticoll/select failed: %s", esp_err_to_name(ret));
        s_card_selected = false;
        return 0;
    }
    s_last_card.atqa = atqa;
    s_card_selected = true;
    print_card(&s_last_card);
    return 0;
}

static int cmd_halt(int argc, char **argv)
{
    if (!require_card("halt")) {
        return 1;
    }
    esp_err_t ret = mifare_halt();
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "HLTA sent — card should now ignore REQA but answer WUPA");
        s_card_selected = false;
    } else {
        ESP_LOGE(TAG, "halt failed: %s", esp_err_to_name(ret));
    }
    return 0;
}

/* ========================================================================= */
/*                      LAYER 3 — MIFARE CLASSIC                             */
/* ========================================================================= */

static int cmd_auth(int argc, char **argv)
{
    if (arg_parse(argc, argv, (void **)&auth_args) != 0) {
        arg_print_errors(stderr, auth_args.end, argv[0]);
        return 1;
    }
    if (!require_card("auth")) {
        return 1;
    }

    uint8_t key[6];
    if (hex_to_bytes(auth_args.key->sval[0], key, sizeof(key)) != 0) {
        ESP_LOGE(TAG, "auth: key must be exactly 12 hex chars (6 bytes)");
        return 1;
    }
    const char t = (char)tolower(auth_args.key_type->sval[0][0]);
    if (t != 'a' && t != 'b') {
        ESP_LOGE(TAG, "auth: key type must be 'a' or 'b'");
        return 1;
    }

    esp_err_t ret = mifare_auth((uint8_t)auth_args.block->ival[0],
                                t == 'a' ? MIFARE_KEY_A : MIFARE_KEY_B,
                                key, s_last_card.uid);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "auth OK (block %d, key %c) — Crypto1 active",
                 auth_args.block->ival[0], t);
    } else {
        ESP_LOGE(TAG, "auth failed: %s", esp_err_to_name(ret));
    }
    return 0;
}

static int cmd_read(int argc, char **argv)
{
    if (arg_parse(argc, argv, (void **)&read_args) != 0) {
        arg_print_errors(stderr, read_args.end, argv[0]);
        return 1;
    }
    if (!require_card("read")) {
        return 1;
    }

    uint8_t data[16];
    const uint8_t block = (uint8_t)read_args.block->ival[0];
    esp_err_t ret = mifare_read_block(block, data);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "read block %d failed: %s", block, esp_err_to_name(ret));
        return 0;
    }
    printf("block %2d: ", block);
    for (int i = 0; i < 16; i++) {
        printf("%02X ", data[i]);
    }
    printf("\n");
    return 0;
}

static int cmd_write(int argc, char **argv)
{
    if (arg_parse(argc, argv, (void **)&write_args) != 0) {
        arg_print_errors(stderr, write_args.end, argv[0]);
        return 1;
    }
    if (!require_card("write")) {
        return 1;
    }

    uint8_t data[16];
    if (hex_to_bytes(write_args.data->sval[0], data, sizeof(data)) != 0) {
        ESP_LOGE(TAG, "write: data must be exactly 32 hex chars (16 bytes)");
        return 1;
    }
    const uint8_t block = (uint8_t)write_args.block->ival[0];
    esp_err_t ret = mifare_write_block(block, data);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "block %d written", block);
    } else {
        ESP_LOGE(TAG, "write block %d failed: %s", block, esp_err_to_name(ret));
    }
    return 0;
}

/* ========================================================================= */
/*                      LAYER 4 — OEM PARITY / DEBUG                         */
/* ========================================================================= */

static int cmd_burst(int argc, char **argv)
{
    if (!require_ready("burst")) {
        return 1;
    }
    bool card = false;
    uint8_t state = 0;
    esp_err_t ret = mifare_idle_burst(&card, &state);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "idle burst done: card=%s, final ModemState=0x%02X",
                 card ? "YES" : "no", state);
    } else {
        ESP_LOGE(TAG, "burst failed: %s", esp_err_to_name(ret));
    }
    return 0;
}

/**
 * poll — composite OEM cycle replica (lives in the harness, not the lib):
 *   wake -> idle burst -> [card: power_up + wupa + select + OEM auth probe]
 *   -> power_down -> sleep for the period.
 */
static int cmd_poll(int argc, char **argv)
{
    if (arg_parse(argc, argv, (void **)&poll_args) != 0) {
        arg_print_errors(stderr, poll_args.end, argv[0]);
        return 1;
    }
    if (!require_ready("poll")) {
        return 1;
    }

    const int count  = poll_args.count->count  > 0 ? poll_args.count->ival[0]  : 10;
    const int period = poll_args.period->count > 0 ? poll_args.period->ival[0] : 1000;

    ESP_LOGI(TAG, "poll: %d cycles, %d ms period (scope A/B vs OEM captures)", count, period);

    for (int i = 0; i < count; i++) {
        esp_err_t ret = mifare_wake();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "cycle %d: wake failed: %s", i, esp_err_to_name(ret));
            break;
        }

        bool card = false;
        uint8_t state = 0;
        ret = mifare_idle_burst(&card, &state);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "cycle %d: burst failed: %s", i, esp_err_to_name(ret));
            break;
        }

        if (card) {
            ESP_LOGI(TAG, "cycle %d: CARD DETECTED (ModemState=0x%02X) — full branch", i, state);

            /* OEM card-detected branch: SoftReset + full init + transaction */
            uint16_t atqa = 0;
            ret = mifare_power_up();
            if (ret == ESP_OK) {
                ret = mifare_wupa(&atqa);
            }
            if (ret == ESP_OK) {
                ret = mifare_anticoll_select(&s_last_card);
            }
            if (ret == ESP_OK) {
                s_last_card.atqa = atqa;
                s_card_selected = true;
                print_card(&s_last_card);
                /* Vestigial OEM probe: expected to FAIL (fixed key, result ignored) */
                esp_err_t probe = mifare_auth(0, MIFARE_KEY_A, k_oem_probe_key,
                                              s_last_card.uid);
                ESP_LOGI(TAG, "OEM probe auth: %s (vestigial — FAIL expected)",
                         probe == ESP_OK ? "OK" : "FAIL");
            } else {
                ESP_LOGW(TAG, "cycle %d: transaction failed: %s", i, esp_err_to_name(ret));
            }
        } else {
            ESP_LOGI(TAG, "cycle %d: no card (ModemState=0x%02X)", i, state);
        }

        mifare_power_down();
        if (i + 1 < count) {
            vTaskDelay(pdMS_TO_TICKS(period));
        }
    }
    ESP_LOGI(TAG, "poll finished");
    return 0;
}

static int cmd_reg(int argc, char **argv)
{
    if (arg_parse(argc, argv, (void **)&reg_args) != 0) {
        arg_print_errors(stderr, reg_args.end, argv[0]);
        return 1;
    }
    if (!require_awake("reg")) {
        return 1;
    }

    const char op = (char)tolower(reg_args.op->sval[0][0]);
    const int addr = reg_args.addr->ival[0];
    if (addr < 0 || addr > 0x3F) {
        ESP_LOGE(TAG, "reg: address must be 0x00-0x3F");
        return 1;
    }

    if (op == 'r') {
        uint8_t val = 0;
        esp_err_t ret = mifare_read_reg((uint8_t)addr, &val);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "reg[0x%02X] = 0x%02X", addr, val);
        } else {
            ESP_LOGE(TAG, "read failed: %s", esp_err_to_name(ret));
        }
    } else if (op == 'w') {
        if (reg_args.value->count == 0) {
            ESP_LOGE(TAG, "reg w: missing value");
            return 1;
        }
        esp_err_t ret = mifare_write_reg((uint8_t)addr, (uint8_t)reg_args.value->ival[0]);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "reg[0x%02X] <- 0x%02X", addr, reg_args.value->ival[0]);
        } else {
            ESP_LOGE(TAG, "write failed: %s", esp_err_to_name(ret));
        }
    } else {
        ESP_LOGE(TAG, "reg: op must be 'r' or 'w'");
        return 1;
    }
    return 0;
}

static const char *reg_name(uint8_t addr)
{
    static const char *const names[0x40] = {
        [0x01] = "CommandReg",    [0x02] = "ComIEnReg",     [0x03] = "DivIEnReg",
        [0x04] = "ComIrqReg",     [0x05] = "DivIrqReg",     [0x06] = "ErrorReg",
        [0x07] = "Status1Reg",    [0x08] = "Status2Reg",    [0x09] = "FIFODataReg",
        [0x0A] = "FIFOLevelReg",  [0x0B] = "WaterLevelReg", [0x0C] = "ControlReg",
        [0x0D] = "BitFramingReg", [0x0E] = "CollReg",
        [0x11] = "ModeReg",       [0x12] = "TxModeReg",     [0x13] = "RxModeReg",
        [0x14] = "TxControlReg",  [0x15] = "TxASKReg",      [0x16] = "TxSelReg",
        [0x17] = "RxSelReg",      [0x18] = "RxThresholdReg",[0x19] = "DemodReg",
        [0x1C] = "MfTxReg",       [0x1D] = "MfRxReg",       [0x1F] = "SerialSpeedReg",
        [0x21] = "CRCResultRegH", [0x22] = "CRCResultRegL", [0x23] = "Reserved_0x23",
        [0x24] = "ModWidthReg",   [0x25] = "Reserved_0x25", [0x26] = "RFCfgReg",
        [0x27] = "GsNReg",        [0x28] = "CWGsPReg",      [0x29] = "ModGsPReg",
        [0x2A] = "TModeReg",      [0x2B] = "TPrescalerReg", [0x2C] = "TReloadRegH",
        [0x2D] = "TReloadRegL",   [0x2E] = "TCounterValRegH",[0x2F] = "TCounterValRegL",
        [0x36] = "AutoTestReg",   [0x37] = "VersionReg",
    };
    return names[addr] ? names[addr] : "reserved";
}

static int cmd_dump(int argc, char **argv)
{
    if (!require_awake("dump")) {
        return 1;
    }
    printf(" addr  name             value\n");
    printf(" ----  ---------------  -----\n");
    for (int page = 0; page < 4; page++) {
        for (int lo = 0; lo < 16; lo++) {
            const uint8_t addr = (uint8_t)((page << 4) | lo);
            uint8_t val = 0;
            if (mifare_read_reg(addr, &val) != ESP_OK) {
                ESP_LOGE(TAG, "dump: read failed at 0x%02X", addr);
                return 1;
            }
            printf(" 0x%02X  %-15s  0x%02X\n", addr, reg_name(addr), val);
        }
        printf("\n");
    }
    return 0;
}

/* ========================================================================= */
/*                              REGISTRATION                                 */
/* ========================================================================= */

esp_err_t register_mifare_commands(void)
{
    /* ---- init ---- */
    init_args.mosi = arg_int1("m", "mosi", "<gpio>", "SPI MOSI GPIO");
    init_args.miso = arg_int1("s", "miso", "<gpio>", "SPI MISO GPIO");
    init_args.clk  = arg_int1("c", "clk",  "<gpio>", "SPI CLK GPIO");
    init_args.cs   = arg_int1("C", "cs",   "<gpio>", "SPI CS GPIO (RC522 'SDA' pin)");
    init_args.rst  = arg_int1("r", "rst",  "<gpio>", "NRSTPD GPIO");
    init_args.irq  = arg_int0("q", "irq",  "<gpio>", "optional reader IRQ GPIO (OEM does not use it)");
    init_args.freq = arg_int0("f", "freq", "<hz>",   "SPI clock (default 6000000, OEM parity)");
    init_args.ver  = arg_int0("V", "ver",  "<hex>",  "expected VersionReg: 0x91/0x92 MFRC522, 0xB2 NZ3801-AB (default: skip)");
    init_args.end  = arg_end(8);
    const esp_console_cmd_t init_cmd = {
        .command = "init",
        .help = "Initialize SPI + mifare driver, full OEM init, VersionReg self-test",
        .func = &cmd_init, .argtable = &init_args,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&init_cmd));

    /* ---- deinit ---- */
    const esp_console_cmd_t deinit_cmd = {
        .command = "deinit",
        .help = "Power down reader, park SPI pins LOW, release driver",
        .func = &cmd_deinit,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&deinit_cmd));

    /* ---- power ---- */
    power_args.state = arg_int1("s", "state", "<0|1>", "0 = power down (NRSTPD LOW), 1 = power up + full OEM init");
    power_args.end = arg_end(1);
    const esp_console_cmd_t power_cmd = {
        .command = "power",
        .help = "Reader power control (NRSTPD + pin parking)",
        .func = &cmd_power, .argtable = &power_args,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&power_cmd));

    /* ---- version ---- */
    const esp_console_cmd_t version_cmd = {
        .command = "version",
        .help = "Read VersionReg (0x91/0x92 = MFRC522, 0xB2 = NZ3801-AB)",
        .func = &cmd_version,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&version_cmd));

    /* ---- wupa / reqa ---- */
    const esp_console_cmd_t wupa_cmd = {
        .command = "wupa",
        .help = "Single WUPA (0x52), prints ATQA (wakes cards from HALT — OEM choice)",
        .func = &cmd_wupa,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&wupa_cmd));
    const esp_console_cmd_t reqa_cmd = {
        .command = "reqa",
        .help = "Single REQA (0x26), prints ATQA (does NOT wake HALTed cards)",
        .func = &cmd_reqa,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&reqa_cmd));

    /* ---- select ---- */
    const esp_console_cmd_t select_cmd = {
        .command = "select",
        .help = "WUPA + anticollision + select (cascade-aware); prints UID/SAK/ATQA and caches the card",
        .func = &cmd_select,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&select_cmd));

    /* ---- halt ---- */
    const esp_console_cmd_t halt_cmd = {
        .command = "halt",
        .help = "Send HLTA; then verify REQA misses the card and WUPA finds it",
        .func = &cmd_halt,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&halt_cmd));

    /* ---- auth ---- */
    auth_args.block    = arg_int1("b", "block", "<n>", "block address (auth target sector)");
    auth_args.key_type = arg_str1("t", "type", "<a|b>", "key A or B");
    auth_args.key      = arg_str1("k", "key", "<12hex>", "6-byte key, e.g. FFFFFFFFFFFF");
    auth_args.end      = arg_end(3);
    const esp_console_cmd_t auth_cmd = {
        .command = "auth",
        .help = "MFAuthent against cached card; verifies Crypto1On",
        .func = &cmd_auth, .argtable = &auth_args,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&auth_cmd));

    /* ---- read ---- */
    read_args.block = arg_int1("b", "block", "<n>", "block address");
    read_args.end = arg_end(1);
    const esp_console_cmd_t read_cmd = {
        .command = "read",
        .help = "Read 16-byte block (requires successful auth)",
        .func = &cmd_read, .argtable = &read_args,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&read_cmd));

    /* ---- write ---- */
    write_args.block = arg_int1("b", "block", "<n>", "block address (0 and trailers refused)");
    write_args.data  = arg_str1("d", "data", "<32hex>", "16 bytes to write");
    write_args.end = arg_end(2);
    const esp_console_cmd_t write_cmd = {
        .command = "write",
        .help = "Write 16-byte block (requires successful auth)",
        .func = &cmd_write, .argtable = &write_args,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&write_cmd));

    /* ---- burst ---- */
    const esp_console_cmd_t burst_cmd = {
        .command = "burst",
        .help = "Single OEM idle burst replica (config subset + WUPA + ModemState watch) for scope A/B",
        .func = &cmd_burst,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&burst_cmd));

    /* ---- poll ---- */
    poll_args.count  = arg_int0("n", "count",  "<n>",  "cycles (default 10)");
    poll_args.period = arg_int0("p", "period", "<ms>", "period between cycles (default 1000, OEM parity)");
    poll_args.end = arg_end(2);
    const esp_console_cmd_t poll_cmd = {
        .command = "poll",
        .help = "OEM 1 Hz cycle replica: wake -> burst -> [card: full branch + vestigial auth probe] -> power down",
        .func = &cmd_poll, .argtable = &poll_args,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&poll_cmd));

    /* ---- reg ---- */
    reg_args.op    = arg_str1(NULL, NULL, "<r|w>", "read or write");
    reg_args.addr  = arg_int1(NULL, NULL, "<addr>", "register address 0x00-0x3F (hex ok)");
    reg_args.value = arg_int0(NULL, NULL, "<value>", "value for write (hex ok)");
    reg_args.end = arg_end(3);
    const esp_console_cmd_t reg_cmd = {
        .command = "reg",
        .help = "Raw register access, e.g. 'reg r 0x37' or 'reg w 0x26 0x70' (clone tuning experiments)",
        .func = &cmd_reg, .argtable = &reg_args,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&reg_cmd));

    /* ---- dump ---- */
    const esp_console_cmd_t dump_cmd = {
        .command = "dump",
        .help = "Dump all registers (pages 0-3) with names — compare against OEM table",
        .func = &cmd_dump,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&dump_cmd));

    return ESP_OK;
}
