/*
 * SPDX-FileCopyrightText: 2026 meibao-real (https://meibao.de)
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/param.h>

#include "app_lcd.h"

#include "driver/ledc.h"
#include "driver/jpeg_decode.h"
#include "driver/ppa.h"
#include "esp_check.h"
#include "esp_ldo_regulator.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "soc/mipi_dsi_host_struct.h"

#include "esp_lcd_sc7705.h"

static const char *TAG = "app_lcd";

/* ---- 面板硬件参数（换板实测定型） ---- */
#define LCD_H_RES                   (800)
#define LCD_V_RES                   (1280)
#define LCD_BIT_PER_PIXEL           (16)    /* RGB565 */
#define LCD_PIXEL_CLK_REAL_MHZ      (80)    /* PLL_F240M */

/* ---- USB 副屏逻辑分辨率（对 Windows 声明的横屏尺寸） ---- */
#define LCD_USB_W                   (1280)
#define LCD_USB_H                   (800)

/* 旋转方向：PPA 逆时针。实测 Windows"纵向"内容需 270（90 会上下颠倒） */
#define LCD_ROTATE_CCW_DEG          PPA_SRM_ROTATION_ANGLE_270

/* ---- 流水线 ---- */
#define LCD_DECODE_BUFFER_COUNT     CONFIG_EXAMPLE_LCD_BUF_COUNT
#define LCD_ROTATE_TASK_PRIORITY    (5)
#define LCD_JPEG_MAX_MCU_ALIGNMENT  16U
#define LCD_ALIGN_UP(v, a)          (((v) + (a) - 1) & ~((a) - 1))
#define LCD_BYTES_PER_PIXEL         (2)     /* RGB565 */

#define LCD_PIN_RST                 (36)
#define LCD_PIN_BK_LIGHT            (24)

#define LCD_BK_LIGHT_PWM_FREQ_HZ    (20000)
#define LCD_BK_LIGHT_PWM_DUTY_PCT   (80)
#define LCD_BK_LIGHT_PWM_DUTY_RES   (LEDC_TIMER_10_BIT)

#define MIPI_DSI_PHY_PWR_LDO_CHAN       (3)
#define MIPI_DSI_PHY_PWR_LDO_VOLTAGE_MV (2500)

static esp_lcd_panel_handle_t s_panel;
static esp_lcd_dsi_bus_handle_t s_mipi_dsi_bus;
static esp_lcd_panel_io_handle_t s_mipi_dbi_io;
static esp_ldo_channel_handle_t s_ldo_mipi_phy;

static void *s_decode_buffers[LCD_DECODE_BUFFER_COUNT];
static size_t s_decode_buffer_sizes[LCD_DECODE_BUFFER_COUNT];
static size_t s_decode_buffer_len;

static void *s_dpi_fb;                            /* DPI 帧缓冲（PPA 直写目标） */

/* 流水线队列：解码缓冲索引在 decode→rotate 间流转 */
static QueueHandle_t s_dec_free_q;                /* 空闲解码缓冲 */
static QueueHandle_t s_dec_done_q;                /* 解码完成待旋转 */

static jpeg_decoder_handle_t s_jpeg_decoder;
static ppa_client_handle_t s_ppa_client;

static jpeg_decode_cfg_t s_decode_config = {
    .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
    .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR, /* LE 帧缓冲映射，若红蓝对调改 RGB */
};

static esp_err_t app_lcd_get_jpeg_output_layout(const uint8_t *input, size_t input_len,
                                                uint16_t width, uint16_t height,
                                                uint32_t *output_width, uint32_t *output_height)
{
    jpeg_decode_picture_info_t picture_info = {0};
    ESP_RETURN_ON_ERROR(jpeg_decoder_get_info(input, input_len, &picture_info),
                        TAG, "parse JPEG header failed");
    ESP_RETURN_ON_FALSE(picture_info.width == width && picture_info.height == height,
                        ESP_ERR_INVALID_SIZE, TAG,
                        "JPEG size is %ux%u, expected %ux%u",
                        (unsigned)picture_info.width, (unsigned)picture_info.height, width, height);

    uint32_t horizontal_alignment;
    uint32_t vertical_alignment;
    switch (picture_info.sample_method) {
    case JPEG_DOWN_SAMPLING_YUV444:
        horizontal_alignment = 8;
        vertical_alignment = 8;
        break;
    case JPEG_DOWN_SAMPLING_YUV422:
        horizontal_alignment = 16;
        vertical_alignment = 8;
        break;
    case JPEG_DOWN_SAMPLING_YUV420:
        horizontal_alignment = 16;
        vertical_alignment = 16;
        break;
    case JPEG_DOWN_SAMPLING_GRAY:
        return ESP_ERR_NOT_SUPPORTED;
    default:
        return ESP_ERR_INVALID_ARG;
    }

    *output_width = LCD_ALIGN_UP(picture_info.width, horizontal_alignment);
    *output_height = LCD_ALIGN_UP(picture_info.height, vertical_alignment);
    return ESP_OK;
}

/* 旋转级：取解码完成的帧，PPA 旋转 90° 直写 DPI 帧缓冲 */
static void rotate_task(void *arg)
{
    int idx;
    while (1) {
        if (xQueueReceive(s_dec_done_q, &idx, portMAX_DELAY) != pdPASS) {
            continue;
        }

        ppa_srm_oper_config_t rot_config = {
            .in = {
                .buffer = s_decode_buffers[idx],
                .pic_w = LCD_USB_W,
                .pic_h = LCD_USB_H,
                .block_w = LCD_USB_W,
                .block_h = LCD_USB_H,
                .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
            },
            .out = {
                .buffer = s_dpi_fb,
                .buffer_size = LCD_H_RES * LCD_V_RES * LCD_BYTES_PER_PIXEL,
                .pic_w = LCD_H_RES,
                .pic_h = LCD_V_RES,
                .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
            },
            .rotation_angle = LCD_ROTATE_CCW_DEG,
            .scale_x = 1.0,
            .scale_y = 1.0,
            .mirror_x = false,
            .mirror_y = false,
            .mode = PPA_TRANS_MODE_BLOCKING,
        };
        esp_err_t ret = ppa_do_scale_rotate_mirror(s_ppa_client, &rot_config);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "PPA rotate failed: %s", esp_err_to_name(ret));
        }

        static int fps_count;
        static int64_t start_time;
        if (start_time == 0) {
            start_time = esp_timer_get_time();
        }
        if (++fps_count == 50) {
            int64_t end_time = esp_timer_get_time();
            ESP_LOGI(TAG, "fps: %f", 1000000.0 / ((end_time - start_time) / 50.0));
            start_time = end_time;
            fps_count = 0;
        }

        xQueueSend(s_dec_free_q, &idx, 0);
    }
}

/**
 * 解码级（transfer 任务上下文调用）：校验→硬解→入旋转队列后立即返回，
 * JPEG 输入缓冲随即归还 USB 接收池，收帧不被解码阻塞
 */
void app_lcd_draw(uint8_t *buf, uint32_t len, uint16_t width, uint16_t height)
{
    if (width != LCD_USB_W || height != LCD_USB_H) {
        ESP_LOGW(TAG, "Drop JPEG with unexpected size: %ux%u, expected %ux%u",
                 width, height, LCD_USB_W, LCD_USB_H);
        return;
    }

    uint32_t output_width;
    uint32_t output_height;
    esp_err_t layout_ret = app_lcd_get_jpeg_output_layout(buf, len, width, height,
                                                          &output_width, &output_height);
    if (layout_ret != ESP_OK) {
        ESP_LOGD(TAG, "Unsupported JPEG layout: %s", esp_err_to_name(layout_ret));
        return;
    }

    int idx;
    if (xQueueReceive(s_dec_free_q, &idx, pdMS_TO_TICKS(1)) != pdPASS) {
        static uint32_t dropped;
        if (++dropped == 1 || dropped % 50 == 0) {
            ESP_LOGW(TAG, "decoder busy, frame dropped=%"PRIu32, dropped);
        }
        return;
    }

    size_t expected_output_size = output_width * output_height * LCD_BYTES_PER_PIXEL;
    if (expected_output_size > s_decode_buffer_sizes[idx]) {
        ESP_LOGW(TAG, "JPEG output too large: %u > %u", (unsigned)expected_output_size,
                 (unsigned)s_decode_buffer_sizes[idx]);
        xQueueSend(s_dec_free_q, &idx, 0);
        return;
    }

    uint32_t output_size = 0;
    esp_err_t decode_ret = jpeg_decoder_process(s_jpeg_decoder, &s_decode_config, buf, len,
                                                s_decode_buffers[idx],
                                                s_decode_buffer_sizes[idx], &output_size);
    if (decode_ret != ESP_OK) {
        ESP_LOGD(TAG, "JPEG decode failed: %s", esp_err_to_name(decode_ret));
        xQueueSend(s_dec_free_q, &idx, 0);
        return;
    }
    if (output_size != expected_output_size) {
        ESP_LOGW(TAG, "Unexpected JPEG output size: %u, expected %u",
                 (unsigned)output_size, (unsigned)expected_output_size);
        xQueueSend(s_dec_free_q, &idx, 0);
        return;
    }

    /* 硬解按 MCU 对齐写行，去掉行尾 padding（1280x800 恰好全对齐，防御性保留） */
    if (output_width != width) {
        uint8_t *output = s_decode_buffers[idx];
        size_t visible_row_size = width * LCD_BYTES_PER_PIXEL;
        size_t decoded_row_size = output_width * LCD_BYTES_PER_PIXEL;
        for (size_t row = 1; row < height; row++) {
            memmove(output + row * visible_row_size, output + row * decoded_row_size,
                    visible_row_size);
        }
    }

    xQueueSend(s_dec_done_q, &idx, 0);
}

esp_err_t app_lcd_get_resolution(uint16_t *width, uint16_t *height)
{
    /* 返回 USB 逻辑分辨率（横屏）：驱动按此发帧，字符串描述符按此声明 */
    ESP_RETURN_ON_FALSE(width && height, ESP_ERR_INVALID_ARG, TAG, "resolution output is NULL");
    ESP_RETURN_ON_FALSE(s_panel, ESP_ERR_INVALID_STATE, TAG, "display is not initialized");
    *width = LCD_USB_W;
    *height = LCD_USB_H;
    return ESP_OK;
}

static esp_err_t app_lcd_backlight_init(void)
{
    ESP_LOGI(TAG, "Turn on LCD backlight (GPIO%d, %d Hz, %d%% duty)",
             LCD_PIN_BK_LIGHT, LCD_BK_LIGHT_PWM_FREQ_HZ, LCD_BK_LIGHT_PWM_DUTY_PCT);
    ledc_timer_config_t bk_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LCD_BK_LIGHT_PWM_DUTY_RES,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = LCD_BK_LIGHT_PWM_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&bk_timer), TAG, "config backlight timer failed");
    ledc_channel_config_t bk_channel = {
        .gpio_num = LCD_PIN_BK_LIGHT,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_0,
        .duty = (1U << LCD_BK_LIGHT_PWM_DUTY_RES) * LCD_BK_LIGHT_PWM_DUTY_PCT / 100,
        .hpoint = 0,
    };
    return ledc_channel_config(&bk_channel);
}

esp_err_t app_lcd_init(void)
{
    /* 1. 背光 PWM */
    ESP_RETURN_ON_ERROR(app_lcd_backlight_init(), TAG, "init backlight failed");

    /* 2. MIPI DSI PHY 供电：LDO 通道 3 @ 2500 mV */
    ESP_LOGI(TAG, "MIPI DSI PHY powered on (LDO channel %d, %dmV)",
             MIPI_DSI_PHY_PWR_LDO_CHAN, MIPI_DSI_PHY_PWR_LDO_VOLTAGE_MV);
    esp_ldo_channel_config_t ldo_mipi_phy_config = {
        .chan_id = MIPI_DSI_PHY_PWR_LDO_CHAN,
        .voltage_mv = MIPI_DSI_PHY_PWR_LDO_VOLTAGE_MV,
    };
    ESP_RETURN_ON_ERROR(esp_ldo_acquire_channel(&ldo_mipi_phy_config, &s_ldo_mipi_phy),
                        TAG, "acquire LDO for MIPI PHY failed");

    /* 3. MIPI DSI bus：60fps 档 lane 1200 + DPHY PLLREF=XTAL（芯片 v3.x 要求） */
    ESP_LOGI(TAG, "Initialize MIPI DSI bus");
    esp_lcd_dsi_bus_config_t bus_config = SC7705_PANEL_BUS_DSI_2CH_CONFIG();
    ESP_RETURN_ON_ERROR(esp_lcd_new_dsi_bus(&bus_config, &s_mipi_dsi_bus),
                        TAG, "create DSI bus failed");

    /* 3.5 PHY LP<->HS 转换时序（治行首数据丢失，单位=lane byte clk=lane/8）
     * lane 600 ⇒ byte clk 75MHz，旧板实测有效值原样复用 */
    MIPI_DSI_HOST.phy_tmr_cfg.phy_hs2lp_time = 100;            /* IDF 默认 50 */
    MIPI_DSI_HOST.phy_tmr_cfg.phy_lp2hs_time = 200;            /* IDF 默认 104 */
    MIPI_DSI_HOST.phy_tmr_lpclk_cfg.phy_clkhs2lp_time = 92;    /* IDF 默认 46 */
    MIPI_DSI_HOST.phy_tmr_lpclk_cfg.phy_clklp2hs_time = 256;   /* IDF 默认 128 */
    ESP_LOGI(TAG, "PHY switch time (byte clks @75MHz): hs2lp=100 lp2hs=200 clk_hs2lp=92 clk_lp2hs=256");

    /* 4. DBI panel IO（发初始化命令） */
    esp_lcd_dbi_io_config_t dbi_config = SC7705_PANEL_IO_DBI_CONFIG();
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_dbi(s_mipi_dsi_bus, &dbi_config, &s_mipi_dbi_io),
                        TAG, "create DBI IO failed");

    /* 5. SC7705 面板（DPI 120MHz ≈ 59.2Hz；垂直时序 VSync=4 VBP=10 VFP=395 定型） */
    ESP_LOGI(TAG, "Install LCD driver of sc7705");
    esp_lcd_dpi_panel_config_t dpi_config = SC7705_800_1280_PANEL_60HZ_DPI_CONFIG(LCD_COLOR_FMT_RGB565);
    sc7705_vendor_config_t vendor_config = {
        .mipi_config = {
            .dsi_bus = s_mipi_dsi_bus,
            .dpi_config = &dpi_config,
        },
    };
    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = LCD_PIN_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = LCD_BIT_PER_PIXEL,
        .vendor_config = &vendor_config,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_sc7705(s_mipi_dbi_io, &panel_config, &s_panel),
                        TAG, "create SC7705 panel failed");

    /* 5.5 关闭每帧 BTA 应答：消除帧首延迟抖动（换板前实验结论，保留） */
    MIPI_DSI_HOST.vid_mode_cfg.frame_bta_ack_en = 0;

    /* 5.6 HS EoTp 保持使能（IDF 默认；换板实测关闭会致右侧黑斑） */
    MIPI_DSI_HOST.pckhdl_cfg.eotp_tx_en = 1;

    /* 6. 硬件复位（sunxi 时序 50/10/200ms）→ init（19 条实机抓取命令+LP读回诊断）→ 开显示 */
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "panel reset failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "panel init failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true), TAG, "display on failed");

    /* 7. DPI 帧缓冲地址（PPA 直写目标，num_fbs=1） */
    ESP_RETURN_ON_ERROR(esp_lcd_dpi_panel_get_frame_buffer(s_panel, 1, &s_dpi_fb),
                        TAG, "get DPI frame buffer failed");

    /* 8. JPEG 硬解引擎 + 流水线队列/任务 + PPA 客户端 */
    jpeg_decode_engine_cfg_t engine_config = {
        .intr_priority = 1,
        .timeout_ms = 50,
    };
    ESP_RETURN_ON_ERROR(jpeg_new_decoder_engine(&engine_config, &s_jpeg_decoder),
                        TAG, "create JPEG decoder failed");

    size_t output_width = LCD_ALIGN_UP(LCD_USB_W, LCD_JPEG_MAX_MCU_ALIGNMENT);
    size_t output_height = LCD_ALIGN_UP(LCD_USB_H, LCD_JPEG_MAX_MCU_ALIGNMENT);
    s_decode_buffer_len = output_width * output_height * LCD_BYTES_PER_PIXEL;
    s_dec_free_q = xQueueCreate(LCD_DECODE_BUFFER_COUNT, sizeof(int));
    s_dec_done_q = xQueueCreate(LCD_DECODE_BUFFER_COUNT, sizeof(int));
    ESP_RETURN_ON_FALSE(s_dec_free_q && s_dec_done_q, ESP_ERR_NO_MEM, TAG, "create pipeline queues failed");
    for (int i = 0; i < LCD_DECODE_BUFFER_COUNT; i++) {
        jpeg_decode_memory_alloc_cfg_t output_memory_config = {
            .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER,
        };
        s_decode_buffers[i] = jpeg_alloc_decoder_mem(s_decode_buffer_len, &output_memory_config,
                                                     &s_decode_buffer_sizes[i]);
        ESP_RETURN_ON_FALSE(s_decode_buffers[i], ESP_ERR_NO_MEM, TAG,
                            "allocate decode buffer %d failed", i);
        xQueueSend(s_dec_free_q, &i, 0);
    }
    ESP_LOGI(TAG, "decode buffers: %u x %u bytes", LCD_DECODE_BUFFER_COUNT, (unsigned)s_decode_buffer_len);

    ppa_client_config_t ppa_config = {
        .oper_type = PPA_OPERATION_SRM,
    };
    ESP_RETURN_ON_ERROR(ppa_register_client(&ppa_config, &s_ppa_client),
                        TAG, "register PPA client failed");

    BaseType_t task_ret = xTaskCreatePinnedToCore(rotate_task, "rotate_task", 4096, NULL,
                                                  LCD_ROTATE_TASK_PRIORITY, NULL, 0);
    ESP_RETURN_ON_FALSE(task_ret == pdPASS, ESP_FAIL, TAG, "create rotate task failed");

    ESP_LOGI(TAG, "LCD ready: panel %ux%u RGB565 @ %dMHz (≈%.1fHz), USB %ux%u landscape, pipeline decode||rotate",
             LCD_H_RES, LCD_V_RES, LCD_PIXEL_CLK_REAL_MHZ,
             (float)LCD_PIXEL_CLK_REAL_MHZ * 1000000.0f / 1200 / 1450, LCD_USB_W, LCD_USB_H);
    return ESP_OK;
}
