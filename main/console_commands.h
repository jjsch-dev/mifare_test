/**
 * @file console_commands.h
 * @brief ESP Console CLI registration for the mifare driver test harness.
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Register all mifare test commands with the ESP console.
 *        Mirrors the fpm harness pattern (argtable3 + register_*_commands).
 */
esp_err_t register_mifare_commands(void);

#ifdef __cplusplus
}
#endif
