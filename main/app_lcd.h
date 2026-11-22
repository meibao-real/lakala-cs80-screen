/*
 * SPDX-FileCopyrightText: 2026 meibao-real (https://meibao.de)
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef APP_LCD_H
#define APP_LCD_H

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t app_lcd_init(void);

esp_err_t app_lcd_get_resolution(uint16_t *width, uint16_t *height);

void app_lcd_draw(uint8_t *buf, uint32_t len, uint16_t width, uint16_t height);

#ifdef __cplusplus
}
#endif

#endif
