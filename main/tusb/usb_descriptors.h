/*
* SPDX-FileCopyrightText: 2026 meibao-real (https://meibao.de)
 *
 * SPDX-License-Identifier: MIT
*/

#pragma once
#include "esp_err.h"
#include "tusb.h"
#include "sdkconfig.h"

esp_err_t usb_descriptors_init(uint16_t width, uint16_t height);

enum {
    ITF_NUM_VENDOR = 0,
    ITF_NUM_TOTAL,
};

enum {
    EPNUM_DEFAULT = 0,
    EPNUM_VENDOR,
    EPNUM_TOTAL
};
