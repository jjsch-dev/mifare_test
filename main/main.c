/**
 * @file main.c
 * @brief mifare_test — Stage 2 console test harness.
 *
 * Boots an ESP console REPL (UART) with the mifare driver commands.
 * Same pattern as the fpm Stage 1 harness: the library under test lives in
 * components/mifare, this app only wires the console to it.
 *
 * Bench wiring (RC522 module -> ESP32-S3 DevKitC-1), suggested defaults:
 *   RC522 SDA(CS) -> GPIO10, SCK -> GPIO12, MOSI -> GPIO11,
 *   MISO -> GPIO13, RST -> GPIO9, 3.3V + GND. IRQ left unconnected.
 *
 *   init -m 11 -s 13 -c 12 -C 10 -r 9 -V 0x91
 */
#include <stdio.h>
#include "esp_console.h"
#include "esp_log.h"
#include "console_commands.h"

static const char *TAG = "mifare_test";

void app_main(void)
{
    ESP_LOGI(TAG, "mifare_test — Stage 2 harness (MFRC522/NZ3801-AB driver)");
    ESP_LOGI(TAG, "type 'help' for commands; typical start:");
    ESP_LOGI(TAG, "  init -m 11 -s 13 -c 12 -C 10 -r 9 -V 0x91");

    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = "mifare> ";
    repl_config.max_cmdline_length = 256;
    repl_config.task_stack_size = 8192;

    esp_console_dev_uart_config_t hw_config = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_uart(&hw_config, &repl_config, &repl));

    ESP_ERROR_CHECK(register_mifare_commands());
    ESP_ERROR_CHECK(esp_console_start_repl(repl));
}
