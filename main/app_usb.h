/*
 * SPDX-FileCopyrightText: 2024-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 * 移植自 esp-iot-solution usb_extend_screen 示例（去掉触摸/音频）
 */
#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "tusb.h"
#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize tinyusb device.
 */
esp_err_t app_usb_init(void);

#if CFG_TUD_VENDOR
esp_err_t app_vendor_init(void);
#endif

#ifdef __cplusplus
}
#endif
