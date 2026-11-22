/*
* SC7705 MIPI显示驱动
 * @author  https://meibao.de
 * 梅宝的 - 汉语拼音 - 梅宝的博客
 */

#include "soc/soc_caps.h"

#if SOC_MIPI_DSI_SUPPORTED
#include "esp_check.h"
#include "esp_log.h"
#include "esp_lcd_panel_commands.h"
#include "esp_lcd_panel_interface.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_vendor.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_lcd_sc7705.h"
#include "esp_idf_version.h"

typedef struct {
    esp_lcd_panel_io_handle_t io;
    int reset_gpio_num;
    uint8_t madctl_val; // save current value of LCD_CMD_MADCTL register
    uint8_t colmod_val; // save current value of LCD_CMD_COLMOD register
    const sc7705_lcd_init_cmd_t *init_cmds;
    uint16_t init_cmds_size;
    struct {
        unsigned int reset_level: 1;
    } flags;
    // To save the original functions of MIPI DPI panel
    esp_err_t (*del)(esp_lcd_panel_t *panel);
    esp_err_t (*init)(esp_lcd_panel_t *panel);
} sc7705_panel_t;

static const char *TAG = "sc7705";

static esp_err_t panel_sc7705_del(esp_lcd_panel_t *panel);
static esp_err_t panel_sc7705_init(esp_lcd_panel_t *panel);
static esp_err_t panel_sc7705_reset(esp_lcd_panel_t *panel);
static esp_err_t panel_sc7705_invert_color(esp_lcd_panel_t *panel, bool invert_color_data);
static esp_err_t panel_sc7705_disp_on_off(esp_lcd_panel_t *panel, bool on_off);

esp_err_t esp_lcd_new_panel_sc7705(const esp_lcd_panel_io_handle_t io, const esp_lcd_panel_dev_config_t *panel_dev_config,
                                   esp_lcd_panel_handle_t *ret_panel)
{
    ESP_LOGI(TAG, "version: %d.%d.%d", ESP_LCD_SC7705_VER_MAJOR, ESP_LCD_SC7705_VER_MINOR,
             ESP_LCD_SC7705_VER_PATCH);
    ESP_RETURN_ON_FALSE(io && panel_dev_config && ret_panel, ESP_ERR_INVALID_ARG, TAG, "invalid arguments");
    sc7705_vendor_config_t *vendor_config = (sc7705_vendor_config_t *)panel_dev_config->vendor_config;
    ESP_RETURN_ON_FALSE(vendor_config && vendor_config->mipi_config.dpi_config && vendor_config->mipi_config.dsi_bus, ESP_ERR_INVALID_ARG, TAG,
                        "invalid vendor config");

    esp_err_t ret = ESP_OK;
    sc7705_panel_t *sc7705 = (sc7705_panel_t *)calloc(1, sizeof(sc7705_panel_t));
    ESP_RETURN_ON_FALSE(sc7705, ESP_ERR_NO_MEM, TAG, "no mem for sc7705 panel");

    if (panel_dev_config->reset_gpio_num >= 0) {
        gpio_config_t io_conf = {
            .mode = GPIO_MODE_OUTPUT,
            .pin_bit_mask = 1ULL << panel_dev_config->reset_gpio_num,
        };
        ESP_GOTO_ON_ERROR(gpio_config(&io_conf), err, TAG, "configure GPIO for RST line failed");
    }

    switch (panel_dev_config->rgb_ele_order) {
    case LCD_RGB_ELEMENT_ORDER_RGB:
        sc7705->madctl_val = 0;
        break;
    case LCD_RGB_ELEMENT_ORDER_BGR:
        sc7705->madctl_val |= LCD_CMD_BGR_BIT;
        break;
    default:
        ESP_GOTO_ON_FALSE(false, ESP_ERR_NOT_SUPPORTED, err, TAG, "unsupported color space");
        break;
    }

    sc7705->io = io;
    sc7705->init_cmds = vendor_config->init_cmds;
    sc7705->init_cmds_size = vendor_config->init_cmds_size;
    sc7705->reset_gpio_num = panel_dev_config->reset_gpio_num;
    sc7705->flags.reset_level = panel_dev_config->flags.reset_active_high;

    // Create MIPI DPI panel
    esp_lcd_panel_handle_t panel_handle = NULL;
    ESP_GOTO_ON_ERROR(esp_lcd_new_panel_dpi(vendor_config->mipi_config.dsi_bus, vendor_config->mipi_config.dpi_config, &panel_handle), err, TAG,
                      "create MIPI DPI panel failed");
    // DMA2D 由 SC7705_USE_DMA2D（esp_lcd_sc7705.h）经 dpi_config.flags 控制，
    // 默认关闭（CPU 同步拷贝）；开启后 draw_bitmap 走 2D-DMA 异步拷贝
    ESP_LOGD(TAG, "new MIPI DPI panel @%p", panel_handle);

    // Save the original functions of MIPI DPI panel
    sc7705->del = panel_handle->del;
    sc7705->init = panel_handle->init;
    // Overwrite the functions of MIPI DPI panel
    panel_handle->del = panel_sc7705_del;
    panel_handle->init = panel_sc7705_init;
    panel_handle->reset = panel_sc7705_reset;
    panel_handle->invert_color = panel_sc7705_invert_color;
    panel_handle->disp_on_off = panel_sc7705_disp_on_off;
    panel_handle->user_data = sc7705;
    *ret_panel = panel_handle;
    ESP_LOGD(TAG, "new sc7705 panel @%p", sc7705);

    return ESP_OK;

err:
    if (sc7705) {
        if (panel_dev_config->reset_gpio_num >= 0) {
            gpio_reset_pin(panel_dev_config->reset_gpio_num);
        }
        free(sc7705);
    }
    return ret;
}

// 初始化表
//{cmd, { data }, data_bytes, delay_ms}
static const sc7705_lcd_init_cmd_t vendor_specific_init_default[] = {

    {0xB9, (uint8_t[]){0xF1, 0x12, 0x84}, 3, 0}, // unlock

    // BA[6]=0x02 为本机面板与官方 sc7705.py（0x20）的差异之一（实测两值均可显示，保留实机值）
    {0xBA, (uint8_t[]){
        0x31, 0x81, 0x05, 0xF9, 0x0E, 0x0E,
        0x02, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x44, 0x25, 0x00, 0x91,
        0x0A, 0x00, 0x00, 0x00, 0x4F, 0x01,
        0x00, 0x00, 0x37},
     27, 0}, // 面板时序/电源

    {0xB8, (uint8_t[]){0xA8}, 1, 0},
    {0xB3, (uint8_t[]){
        0x00, 0x00, 0x00, 0x00, 0x07, 0x0B,
        0x1E, 0x1E},
     8, 0},
    {0xC0, (uint8_t[]){
        0x73, 0x73, 0x50, 0x50, 0x80, 0x00,
        0x08, 0x70, 0x00},
     9, 0},
    {0xCC, (uint8_t[]){0x0B}, 1, 0},
    {0xBF, (uint8_t[]){0x02, 0x10, 0x82}, 3, 0},
    {0xBC, (uint8_t[]){0x46}, 1, 0},
    {0xB4, (uint8_t[]){0x80}, 1, 0},
    {0xB2, (uint8_t[]){0x40, 0x08}, 2, 0},
    {0xE3, (uint8_t[]){
        0x03, 0x03, 0x00, 0x00, 0x03, 0x03,
        0x00, 0x00, 0x00, 0xC0},
     10, 0}, // 电源/驱动
    {0xB1, (uint8_t[]){
        0x43, 0x54, 0x23, 0x1E, 0x1E, 0x22,
        0x77, 0x04, 0xDB, 0x4C},
     10, 0},
    {0xB5, (uint8_t[]){0x0A, 0x0A}, 2, 0},
    {0xB6, (uint8_t[]){0x44, 0x44}, 2, 0},

    // E9/EA（GOA 时序）与 E0（Gamma）为与官方示例差异最大的部分——
    // 实测（2026-08-20）：换成官方 _2 表后全屏条纹不可用，
    // 证明本表（实机日志抓取版）才是这块玻璃的正确配置，勿再动
    {0xE9, (uint8_t[]){
        0x02, 0x00, 0x04, 0x05, 0x0F, 0x80,
        0x81, 0x12, 0x31, 0x23, 0x4F, 0x06,
        0x80, 0x28, 0x47, 0x08, 0x00, 0x00,
        0x0F, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x0F, 0x00, 0x00, 0x00, 0x11, 0x44,
        0xFF, 0x88, 0x8B, 0xBA, 0xA3, 0x31,
        0x17, 0x75, 0x58, 0x00, 0x44, 0xFF,
        0x88, 0x8B, 0xBA, 0xA2, 0x20, 0x06,
        0x64, 0x48, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00},
     63, 0}, // GOA 时序1
    {0xEA, (uint8_t[]){
        0x96, 0x12, 0x00, 0x00, 0x00, 0x70,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x11, 0x44, 0x88, 0xFF, 0x8B, 0xBA,
        0xA4, 0x46, 0x60, 0x02, 0x28, 0x00,
        0x44, 0x88, 0xFF, 0x8B, 0xBA, 0xA5,
        0x57, 0x71, 0x13, 0x38, 0x23, 0x10,
        0x00, 0x00, 0x01, 0x40, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x40, 0x80, 0x81, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00},
     63, 0}, // GOA 时序2
    {0xE0, (uint8_t[]){
        0x01, 0x13, 0x19, 0x27, 0x2F, 0x39,
        0x4A, 0x3C, 0x07, 0x0C, 0x0E, 0x13,
        0x14, 0x12, 0x13, 0x11, 0x1A, 0x01,
        0x13, 0x19, 0x27, 0x2F, 0x39, 0x4A,
        0x3C, 0x07, 0x0C, 0x0E, 0x13, 0x14,
        0x12, 0x13, 0x11, 0x1A},
     34, 0}, // Gamma（正/负对称）

    {0x11, (uint8_t[]){0x00}, 0, 150}, // Sleep Out（无参数短写，延时沿用官方示例）
    {0x29, (uint8_t[]){0x00}, 0, 50},  // Display On
};

static esp_err_t panel_sc7705_del(esp_lcd_panel_t *panel)
{
    sc7705_panel_t *sc7705 = (sc7705_panel_t *)panel->user_data;

    // Delete MIPI DPI panel
    ESP_RETURN_ON_ERROR(sc7705->del(panel), TAG, "del sc7705 panel failed");
    if (sc7705->reset_gpio_num >= 0) {
        gpio_reset_pin(sc7705->reset_gpio_num);
    }
    ESP_LOGD(TAG, "del sc7705 panel @%p", sc7705);
    free(sc7705);

    return ESP_OK;
}

static esp_err_t panel_sc7705_init(esp_lcd_panel_t *panel)
{
    sc7705_panel_t *sc7705 = (sc7705_panel_t *)panel->user_data;
    esp_lcd_panel_io_handle_t io = sc7705->io;
    const sc7705_lcd_init_cmd_t *init_cmds = NULL;
    uint16_t init_cmds_size = 0;
    bool is_cmd_overwritten = false;

    if (sc7705->init_cmds) {
        init_cmds = sc7705->init_cmds;
        init_cmds_size = sc7705->init_cmds_size;
    } else {
        init_cmds = vendor_specific_init_default;
        init_cmds_size = sizeof(vendor_specific_init_default) / sizeof(sc7705_lcd_init_cmd_t);
    }

    for (int i = 0; i < init_cmds_size; i++) {
        // Check if the command has been used or conflicts with the internal
        if (init_cmds[i].data_bytes > 0) {
            switch (init_cmds[i].cmd) {
            case LCD_CMD_MADCTL:
                is_cmd_overwritten = true;
                sc7705->madctl_val = ((uint8_t *)init_cmds[i].data)[0];
                break;
            default:
                is_cmd_overwritten = false;
                break;
            }

            if (is_cmd_overwritten) {
                is_cmd_overwritten = false;
                ESP_LOGW(TAG, "The %02Xh command has been used and will be overwritten by external initialization sequence",
                         init_cmds[i].cmd);
            }
        }

        // Send command
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, init_cmds[i].cmd, init_cmds[i].data, init_cmds[i].data_bytes), TAG, "send command failed");
        vTaskDelay(pdMS_TO_TICKS(init_cmds[i].delay_ms));
    }
    ESP_LOGD(TAG, "send init commands success");

#if SC7705_READBACK_DIAG
    {
        uint8_t id = 0xFF, ba = 0xFF;
        esp_err_t e1 = esp_lcd_panel_io_rx_param(io, 0xDA, &id, 1);
        esp_err_t e2 = esp_lcd_panel_io_rx_param(io, 0xBA, &ba, 1);
        ESP_LOGW(TAG, "LP readback diag: DA=%02X(%s) BA[0]=%02X(%s)  [SC7705 家族 DA=0x48, 期望 BA=0x31]",
                 id, esp_err_to_name(e1), ba, esp_err_to_name(e2));
    }
#endif

    ESP_RETURN_ON_ERROR(sc7705->init(panel), TAG, "init MIPI DPI panel failed");

    return ESP_OK;
}

static esp_err_t panel_sc7705_reset(esp_lcd_panel_t *panel)
{
    sc7705_panel_t *sc7705 = (sc7705_panel_t *)panel->user_data;
    esp_lcd_panel_io_handle_t io = sc7705->io;

    // Perform hardware reset
    if (sc7705->reset_gpio_num >= 0) {
        gpio_set_level(sc7705->reset_gpio_num, !sc7705->flags.reset_level);
        vTaskDelay(pdMS_TO_TICKS(50));
        gpio_set_level(sc7705->reset_gpio_num, sc7705->flags.reset_level);
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level(sc7705->reset_gpio_num, !sc7705->flags.reset_level);
        vTaskDelay(pdMS_TO_TICKS(200));
    } else if (io) { // Perform software reset
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_SWRESET, NULL, 0), TAG, "send command failed");
        vTaskDelay(pdMS_TO_TICKS(120));
    }

    return ESP_OK;
}

static esp_err_t panel_sc7705_invert_color(esp_lcd_panel_t *panel, bool invert_color_data)
{
    sc7705_panel_t *sc7705 = (sc7705_panel_t *)panel->user_data;
    esp_lcd_panel_io_handle_t io = sc7705->io;
    uint8_t command = 0;

    ESP_RETURN_ON_FALSE(io, ESP_ERR_INVALID_STATE, TAG, "invalid panel IO");

    if (invert_color_data) {
        command = LCD_CMD_INVON;
    } else {
        command = LCD_CMD_INVOFF;
    }
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, command, NULL, 0), TAG, "send command failed");

    return ESP_OK;
}

static esp_err_t panel_sc7705_disp_on_off(esp_lcd_panel_t *panel, bool on_off)
{
    sc7705_panel_t *sc7705 = (sc7705_panel_t *)panel->user_data;
    esp_lcd_panel_io_handle_t io = sc7705->io;
    int command = 0;

    if (on_off) {
        command = LCD_CMD_DISPON;
    } else {
        command = LCD_CMD_DISPOFF;
    }
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, command, NULL, 0), TAG, "send command failed");
    return ESP_OK;
}
#endif
