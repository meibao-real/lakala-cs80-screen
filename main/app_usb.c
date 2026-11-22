/*
* SPDX-FileCopyrightText: 2026 meibao-real (https://meibao.de)
 *
 * SPDX-License-Identifier: MIT
*/
#include <stdint.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_private/usb_phy.h"
#include "usb_descriptors.h"
#include "device/usbd.h"
#include "app_lcd.h"
#include "app_usb.h"

static const char *TAG = "app_usb";
static usb_phy_handle_t s_phy_hdl;

//--------------------------------------------------------------------+
// USB PHY config
//--------------------------------------------------------------------+
static esp_err_t usb_phy_init(void)
{
    if (s_phy_hdl) {
        return ESP_OK;
    }

    // Configure USB PHY
    usb_phy_config_t phy_conf = {
        .controller = USB_PHY_CTRL_OTG,
        .otg_mode = USB_OTG_MODE_DEVICE,
        .otg_speed = USB_PHY_SPEED_UNDEFINED,
    };

#if CONFIG_TINYUSB_RHPORT_HS
    phy_conf.target = USB_PHY_TARGET_UTMI;      // P4 HS：内部 UTMI PHY
    phy_conf.otg_speed = USB_PHY_SPEED_HIGH;
#else
    phy_conf.target = USB_PHY_TARGET_INT;       // FS
    phy_conf.otg_speed = USB_PHY_SPEED_FULL;
#endif

    ESP_RETURN_ON_ERROR(usb_new_phy(&phy_conf, &s_phy_hdl), TAG, "USB PHY init failed");
    return ESP_OK;
}

static void tusb_device_task(void *arg)
{
    while (1) {
        tud_task();
    }
}

esp_err_t app_usb_init(void)
{
    uint16_t screen_width;
    uint16_t screen_height;

    ESP_RETURN_ON_ERROR(app_lcd_get_resolution(&screen_width, &screen_height),
                        TAG, "get display resolution failed");
    // vendor 接口字符串描述符携带分辨率/JPEG质量/帧率/帧上限，Windows 驱动据此协商
    ESP_RETURN_ON_ERROR(usb_descriptors_init(screen_width, screen_height),
                        TAG, "initialize USB descriptors failed");
    ESP_RETURN_ON_ERROR(usb_phy_init(), TAG, "USB PHY init failed");
    bool usb_init = tusb_init();
    if (!usb_init) {
        ESP_LOGE(TAG, "USB Device Stack Init Fail");
        return ESP_FAIL;
    }

#if CFG_TUD_VENDOR
    ESP_RETURN_ON_ERROR(app_vendor_init(), TAG, "app_vendor_init failed");
#endif

    xTaskCreate(tusb_device_task, "tusb_device_task", 4096, NULL, CONFIG_USB_TASK_PRIORITY, NULL);
    return ESP_OK;
}

/************************************************** TinyUSB callbacks ***********************************************/
// Invoked when device is mounted
void tud_mount_cb(void)
{
    ESP_LOGI(TAG, "USB Mount");
}

// Invoked when device is unmounted
void tud_umount_cb(void)
{
    ESP_LOGI(TAG, "USB Un-Mount");
}

// Invoked when usb bus is suspended
void tud_suspend_cb(bool remote_wakeup_en)
{
    (void) remote_wakeup_en;
    ESP_LOGI(TAG, "USB Suspend");
}

// Invoked when usb bus is resumed
void tud_resume_cb(void)
{
    ESP_LOGI(TAG, "USB Resume");
}
