/*
* SPDX-FileCopyrightText: 2026 meibao-real (https://meibao.de)
 *
 * SPDX-License-Identifier: MIT
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_check.h"
#include "tusb.h"
#include "tusb_config.h"
#include "usb_descriptors.h"
#include "sdkconfig.h"

//--------------------------------------------------------------------+
// Device Descriptors
//--------------------------------------------------------------------+
tusb_desc_device_t const desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,

    .bDeviceClass       = TUSB_CLASS_UNSPECIFIED,
    .bDeviceSubClass    = TUSB_CLASS_UNSPECIFIED,
    .bDeviceProtocol    = TUSB_CLASS_UNSPECIFIED,

    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,

    .idVendor           = USB_VID,
    .idProduct          = USB_PID,
    .bcdDevice          = 0x0101,

    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,

    .bNumConfigurations = 0x01
};

// Invoked when received GET DEVICE DESCRIPTOR
// Application return pointer to descriptor
uint8_t const *tud_descriptor_device_cb(void)
{
    return (uint8_t const *) &desc_device;
}

enum {
    STR_INDEX_VENDOR = 4,
};

//--------------------------------------------------------------------+
// Configuration Descriptor
//--------------------------------------------------------------------+

#define CONFIG_TOTAL_LEN    (TUD_CONFIG_DESC_LEN + TUD_VENDOR_DESC_LEN * CFG_TUD_VENDOR)

uint8_t const desc_fs_configuration[] = {
    // Config number, interface count, string index, total length, attribute, power in mA
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0, 100),
    // Interface number, string index, EP Out & IN address, EP size
#if CFG_TUD_VENDOR
    TUD_VENDOR_DESCRIPTOR(ITF_NUM_VENDOR, STR_INDEX_VENDOR, EPNUM_VENDOR, 0x80 | EPNUM_VENDOR, CFG_TUD_VENDOR_EPSIZE),
#endif
};

// Invoked when received GET CONFIGURATION DESCRIPTOR
// Application return pointer to descriptor
// Descriptor contents must exist long enough for transfer to complete
uint8_t const *tud_descriptor_configuration_cb(uint8_t index)
{
    (void) index; // for multiple configurations
    return desc_fs_configuration;
}

//--------------------------------------------------------------------+
// String Descriptors
//--------------------------------------------------------------------+

static char vendor_str[64];

// array of pointer to string descriptors
char const *string_desc_arr [] = {
    (const char[]) { 0x09, 0x04 },    // 0: is supported language is English (0x0409)
    USB_MANUFACTURER,                 // 1: Manufacturer
    USB_PRODUCT,                      // 2: Product（设备名 baibai_usb_display）
    "012-2021",                       // 3: Serials, should use chip ID
    vendor_str,                       // 4: Vendor Interface
};

static uint16_t _desc_str[64];

esp_err_t usb_descriptors_init(uint16_t width, uint16_t height)
{
    ESP_RETURN_ON_FALSE(width && height, ESP_ERR_INVALID_ARG, "usb_desc",
                        "invalid display resolution");

    /* vendor 接口字符串是 Windows IDD 驱动的握手协议，格式不能改：
     * <target>udisp0_R<宽>x<高>_Ejpg<质量>_Fps<帧率>_Bl<帧上限> */
    int written = snprintf(vendor_str, sizeof(vendor_str),
                           CONFIG_IDF_TARGET "udisp0_R%ux%u_Ejpg%d_Fps%d_Bl%d",
                           width, height, CONFIG_USB_EXTEND_SCREEN_JPEG_QUALITY,
                           CONFIG_USB_EXTEND_SCREEN_MAX_FPS,
                           CONFIG_USB_EXTEND_SCREEN_FRAME_LIMIT_B);
    ESP_RETURN_ON_FALSE(written > 0 && written < sizeof(vendor_str), ESP_ERR_INVALID_SIZE,
                        "usb_desc", "vendor descriptor string is too long");
    ESP_LOGI("usb_desc", "vendor string: %s", vendor_str);
    return ESP_OK;
}

// Invoked when received GET STRING DESCRIPTOR request
// Application return pointer to descriptor, whose contents must exist long enough for transfer to complete
uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    (void) langid;

    uint8_t chr_count;

    if (index == 0) {
        memcpy(&_desc_str[1], string_desc_arr[0], 2);
        chr_count = 1;
    } else {
        // Note: the 0xEE index string is a Microsoft OS 1.0 Descriptors.

        if (!(index < sizeof(string_desc_arr) / sizeof(string_desc_arr[0]))) {
            return NULL;
        }

        const char *str = (char *)string_desc_arr[index];

        // Cap at max char
        chr_count = (uint8_t) strlen(str);
        if (chr_count > 63) {
            chr_count = 63;
        }

        // Convert ASCII string into UTF-16
        for (uint8_t i = 0; i < chr_count; i++) {
            _desc_str[1 + i] = str[i];
        }
    }

    // first byte is length (including header), second byte is string type
    _desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * chr_count + 2));

    return _desc_str;
}
