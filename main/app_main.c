/*
* SPDX-FileCopyrightText: 2026 meibao-real (https://meibao.de)
 *
 * SPDX-License-Identifier: MIT
*/

#include "esp_log.h"
#include "esp_err.h"

#include "app_lcd.h"
#include "app_usb.h"

static const char *TAG = "baibai_usb_display";

void app_main(void)
{
    ESP_LOGI(TAG, "baibai_usb_display: SC7705 800x1280 USB extend screen");
    ESP_ERROR_CHECK(app_lcd_init());
    ESP_ERROR_CHECK(app_usb_init());
    /* 此后无主循环任务：USB 收帧回调 → transfer_task 解码上屏，全事件驱动 */
}
