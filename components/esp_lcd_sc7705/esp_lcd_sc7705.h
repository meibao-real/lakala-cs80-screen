/*
 * SC7705 MIPI显示驱动
 * @author  https://meibao.de
 * 梅宝的 - 汉语拼音 - 梅宝的博客
 */

#pragma once

#include <stdint.h>
#include "soc/soc_caps.h"

#if SOC_MIPI_DSI_SUPPORTED
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_mipi_dsi.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 *
 * @brief LCD panel initialization commands.
 *
 */
typedef struct {
    int cmd;                /*<! The specific LCD command */
    const void *data;       /*<! Buffer that holds the command specific data */
    size_t data_bytes;      /*<! Size of `data` in memory, in bytes */
    unsigned int delay_ms;  /*<! Delay in milliseconds after this command */
} sc7705_lcd_init_cmd_t;

/**
 * @brief LCD panel vendor configuration.
 *
 * @note  This structure needs to be passed to the `vendor_config` field in `esp_lcd_panel_dev_config_t`.
 *
 */
typedef struct {
    const sc7705_lcd_init_cmd_t *init_cmds;          /*!< Pointer to initialization commands array. Set to NULL if using default commands.
                                                      *   The array should be declared as `static const` and positioned outside the function.
                                                      *   Please refer to `vendor_specific_init_default` in source file.
                                                      */
    uint16_t init_cmds_size;                         /*<! Number of commands in above array */
    struct {
        esp_lcd_dsi_bus_handle_t dsi_bus;               /*!< MIPI-DSI bus configuration */
        const esp_lcd_dpi_panel_config_t *dpi_config;   /*!< MIPI-DPI panel configuration */
    } mipi_config;
} sc7705_vendor_config_t;

/**
 * @brief Create LCD panel for model SC7705
 *
 * @note  Vendor specific initialization can be different between manufacturers, should consult the LCD supplier for initialization sequence code.
 *
 * @param[in]  io LCD panel IO handle
 * @param[in]  panel_dev_config General panel device configuration
 * @param[out] ret_panel Returned LCD panel handle
 * @return
 *      - ESP_ERR_INVALID_ARG   if parameter is invalid
 *      - ESP_OK                on success
 *      - Otherwise             on fail
 */
esp_err_t esp_lcd_new_panel_sc7705(const esp_lcd_panel_io_handle_t io, const esp_lcd_panel_dev_config_t *panel_dev_config,
                                   esp_lcd_panel_handle_t *ret_panel);

/**
 * @brief MIPI-DSI bus configuration structure
 *
 * @note  2 data lanes（sc7705.py: DataLane=2）
 *
 */
#define SC7705_PANEL_BUS_DSI_2CH_CONFIG()                  \
    {                                                      \
        .bus_id = 0,                                       \
        .num_data_lanes = 2,                               \
        .phy_clk_src = MIPI_DSI_PHY_PLLREF_CLK_SRC_DEFAULT,\
        .lane_bit_rate_mbps = 600,                         \
        .flags = {                                         \
            /* 时钟 lane 全程保持 HS：V 消隐 1.3ms 的 LP 空闲期间 */ \
            /* 面板时钟 RX 端接不放电，帧首数据立即锁定—— */ \
            /* 治帧边界(顶部细条+底部1/5)闪烁 */ \
            .clock_lane_force_hs = 1,                      \
        },                                                 \
    }

/**
 * @brief MIPI-DBI panel IO configuration structure
 *
 */
#define SC7705_PANEL_IO_DBI_CONFIG()   \
    {                                 \
        .virtual_channel = 0,         \
        .lcd_cmd_bits = 8,            \
        .lcd_param_bits = 8,          \
    }

/**
 * @brief DMA2D 加速开关（应用于下方两个 DPI 配置宏）
 *
 * 0 = 关闭（默认）：esp_lcd_panel_draw_bitmap 为 CPU 同步拷贝，
 *     完成后即触发 on_color_trans_done，信号量等待逻辑最直观
 * 1 = 开启：draw_bitmap 走 esp_async_fbcpy（P4 2D-DMA）异步拷贝，
 *     拷贝期间 CPU 被释放，on_color_trans_done 在 DMA 完成时触发
 * 注意：DMA2D 对源/目的地址与行宽有对齐要求，非对齐的局部小矩形
 * 刷新（任意坐标 draw_bitmap）需实测验证
 */
#define SC7705_USE_DMA2D   0

/**
 * @brief LP 读回诊断开关：init 表发送完、DPI 视频流启动前，
 *        BTA 读 0xDA（SC7705 家族=0x48）与 0xBA[0]（本机写入 0x31），
 *        验证命令是否真正写入面板。读回全 0 = 面板不支持 LP 读
 */
#define SC7705_READBACK_DIAG  1

/**
 * @brief MIPI DPI configuration structure (800x1280 portrait, 60Hz)
 *
 * @param[in] px_format Input color format of the panel (LCD_COLOR_FMT_RGB565 / LCD_COLOR_FMT_RGB888)
 */
#define SC7705_800_1280_PANEL_60HZ_DPI_CONFIG(px_format)   \
    {                                                      \
        .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,       \
        .dpi_clock_freq_mhz = 80,    /* DPI源PLL_F240M: 240/3=80MHz, lane600配套 */ \
        .virtual_channel = 0,                              \
        .in_color_format = px_format,                      \
        .num_fbs = 1,                                      \
        .flags = {                                         \
            .use_dma2d = SC7705_USE_DMA2D,                 \
        },                                                 \
        .video_timing = {                                  \
            .h_size = 800,                                 \
            .v_size = 1280, \
            .hsync_pulse_width = 120,                      \
            .hsync_back_porch = 140,                       \
            .hsync_front_porch = 140,                      \
            .vsync_pulse_width = 4,    /* 数据起点=VSync+VBP, 须等于面板开窗行(实测 gate0≈VSYNC起点+14行) */ \
            .vsync_back_porch = 10,    /* 对齐: 4+10=14行。实测链: VBP300下移260行→VBP40剩26行→VBP14剩4行 */ \
            .vsync_front_porch = 156,  /* 压帧提刷：官方VFP值, V_total 1450 → 46.0Hz(39.5提速档);
                                         * 若帧边界异常回退 395(V_total 1689, 39.5Hz) */ \
        },                                                 \
    }

/**
 * @brief MIPI DPI configuration structure (800x1280 portrait, 高帧率待试档)
 *
 */
#define SC7705_800_1280_PANEL_50HZ_DPI_CONFIG(px_format)   \
    {                                                      \
        .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,       \
        .dpi_clock_freq_mhz = 120,                         \
        .virtual_channel = 0,                              \
        .in_color_format = px_format,                      \
        .num_fbs = 1,                                      \
        .flags = {                                         \
            .use_dma2d = SC7705_USE_DMA2D,                 \
        },                                                 \
        .video_timing = {                                  \
            .h_size = 800,                                 \
            .v_size = 1280,                                \
            .hsync_pulse_width = 120,                      \
            .hsync_back_porch = 140,                       \
            .hsync_front_porch = 140,                      \
            .vsync_pulse_width = 4,                        \
            .vsync_back_porch = 10,                        \
            .vsync_front_porch = 395,                      \
        },                                                 \
    }

#ifdef __cplusplus
}
#endif

#endif
