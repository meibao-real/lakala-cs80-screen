# esp_lcd_sc7705

> English TL;DR: An `esp_lcd` compatible panel driver for the SC7705 800x1280 MIPI-DSI display, for ESP32-P4 only.
> All timings and the initialization sequence below were measured/validated on real hardware.

SC7705 800×1280 竖屏 MIPI-DSI 面板的 [ESP-IDF](https://github.com/espressif/esp-idf) `esp_lcd` 驱动组件。

## 面板与实测参数

| 项 | 值 |
|---|---|
| 驱动 IC | SC7705（LP 读回自证：寄存器 0xDA = 0x48） |
| 分辨率 | 800×1280（竖屏原生） |
| 接口 | MIPI-DSI，2 data lane + 1 clock lane |
| 像素格式 | RGB565 |
| Lane 速率 | 600 Mbps（这块面板 DPHY RX 的实测封顶） |
| DPI 时钟 | 80 MHz（PLL_F240M / 3） |
| 行时序 | HSync=120 / HBP=140 / HFP=140（H_total=1200） |
| 场时序 | VSync=4 / VBP=10 / VFP=156（V_total=1450 ≈ **46.0 Hz**） |

时序说明：

- **VBP=10 不是随意值**：面板 gate0 约在 VSYNC 起点 +14 行处开窗，数据起点必须与之对齐（VSync 4 + VBP 10 = 14 行），错位会导致图像整体平移或出黑带。
- `SC7705_800_1280_PANEL_60HZ_DPI_CONFIG()` 宏名中的 "60HZ" 只是沿用官方组件命名习惯，该配置实际刷新率约 46 Hz（见下方"已知限制"）。
- 头文件中另有 `SC7705_800_1280_PANEL_50HZ_DPI_CONFIG()` 实验档（DPI 120 MHz），实测 lane 提速不可用，仅留档，不建议使用。

## 已知限制（使用前务必阅读）

1. **仅支持 ESP32-P4**（依赖 MIPI DSI 外设，`SOC_MIPI_DSI_SUPPORTED`）。在 ESP32-P4 芯片 v3.2 + ESP-IDF v5.5.5 上定型验证；建议 sdkconfig 开启 `CONFIG_ESP32P4_REV_MIN_301=y`（≥ v3.0 芯片 DPHY PLL 参考时钟走 XTAL 分支）。
2. **60 Hz 不可达**：lane 600 Mbps 是该面板 DPHY RX 的实测上限——800 Mbps（DPI 80 MHz）出现黑斑、1200 Mbps（DPI 120 MHz）右半屏黑、1500 Mbps 全黑。
3. **LP 读回诊断默认开启**（`SC7705_READBACK_DIAG=1`）：init 表发送完、DPI 视频流启动前，BTA 读回 0xDA（期望 0x48）与 0xBA[0]（期望 0x31）打一条校验日志，用于确认命令真正写入了面板。量产固件可置 0 关闭。
4. **DSI host 侧微调在应用层完成**：PHY LP↔HS 转换时序覆写、`eotp_tx_en`、`frame_bta_ack_en` 等少量 host 寄存器调整在参考工程的 app 侧完成，不在本组件内。若首点出现行首数据丢失 / 右侧黑斑 / 帧首抖动，可参考[作者博客](https://meibao.de)的处理方式。
5. DPI 帧缓冲为单缓冲（`num_fbs=1`），适合配合 `esp_lcd_dpi_panel_get_frame_buffer()` 取 fb 地址做 PPA / DMA2D 直写；普通 `draw_bitmap` 路径下 `SC7705_USE_DMA2D` 默认关闭（CPU 同步拷贝）。

## 使用

### 添加依赖

使用乐鑫官方组件库：

```yaml
idf.py add-dependency "meibao-real/esp_lcd_sc7705^1.0.0"
```

或直接使用 GitHub 仓库：

```yaml
dependencies:
  esp_lcd_sc7705:
    git: https://github.com/meibao-real/esp_lcd_sc7705.git
```

### 初始化示例

```c
#include "esp_ldo_regulator.h"
#include "esp_lcd_sc7705.h"

// 0) MIPI DSI PHY 供电（板级相关：LDO 通道与电压按你的原理图调整）
esp_ldo_channel_handle_t ldo_mipi_phy = NULL;
esp_ldo_channel_config_t ldo_mipi_phy_config = {
    .chan_id = 3,
    .voltage_mv = 2500,
    .flags.mipi_phy = true,
};
ESP_ERROR_CHECK(esp_ldo_acquire_channel(&ldo_mipi_phy_config, &ldo_mipi_phy));

// 1) 创建 MIPI DSI bus（2 lane @ 600 Mbps，时钟 lane 全程 HS）
esp_lcd_dsi_bus_handle_t mipi_dsi_bus = NULL;
const esp_lcd_dsi_bus_config_t bus_config = SC7705_PANEL_BUS_DSI_2CH_CONFIG();
ESP_ERROR_CHECK(esp_lcd_new_dsi_bus(&bus_config, &mipi_dsi_bus));

// 2) 创建 DBI panel IO（发 init 命令用）
esp_lcd_panel_io_handle_t mipi_dbi_io = NULL;
const esp_lcd_panel_io_dbi_config_t dbi_config = SC7705_PANEL_IO_DBI_CONFIG();
ESP_ERROR_CHECK(esp_lcd_new_panel_io_dbi(mipi_dsi_bus, &dbi_config, &mipi_dbi_io));

// 3) 创建 SC7705 面板
esp_lcd_panel_handle_t panel = NULL;
const esp_lcd_dpi_panel_config_t dpi_config = SC7705_800_1280_PANEL_60HZ_DPI_CONFIG(LCD_COLOR_FMT_RGB565);
const sc7705_vendor_config_t vendor_config = {
    .init_cmds = NULL,        // NULL = 使用内置 init 表；可传入自定义表
    .init_cmds_size = 0,
    .mipi_config = {
        .dsi_bus = mipi_dsi_bus,
        .dpi_config = &dpi_config,
    },
};
const esp_lcd_panel_dev_config_t dev_config = {
    .reset_gpio_num = GPIO_NUM_36,   // 板级相关，按原理图调整
    .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
    .bits_per_pixel = 16,            // RGB565
    .vendor_config = &vendor_config,
};
ESP_ERROR_CHECK(esp_lcd_new_panel_sc7705(mipi_dbi_io, &dev_config, &panel));

// 4) 复位 + 初始化 + 点亮（顺序不可调换）
ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));     // 硬复位：50/10/200 ms 时序
ESP_ERROR_CHECK(esp_lcd_panel_init(panel));      // 发送 19 条 init 命令 + LP 读回诊断
ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));
```

之后即可用标准 `esp_lcd_panel_*` 接口 `draw_bitmap` 刷图，或用 `esp_lcd_dpi_panel_get_frame_buffer(panel, 1, &fb0)` 拿帧缓冲地址做直写。

## API

| 符号 | 说明 |
|---|---|
| `esp_lcd_new_panel_sc7705()` | 创建面板，返回标准 `esp_lcd_panel_handle_t` |
| `SC7705_PANEL_BUS_DSI_2CH_CONFIG()` | DSI bus 配置（2 lane @ 600 Mbps，`clock_lane_force_hs=1`） |
| `SC7705_PANEL_IO_DBI_CONFIG()` | DBI panel IO 配置 |
| `SC7705_800_1280_PANEL_60HZ_DPI_CONFIG(fmt)` | DPI 配置（80 MHz，≈46 Hz，`num_fbs=1`） |
| `sc7705_vendor_config_t` | vendor 配置：自定义 init 表 + DSI bus / DPI config |
| `SC7705_USE_DMA2D` | 编译期开关（默认 0）：draw_bitmap 是否走 2D-DMA 异步拷贝 |
| `SC7705_READBACK_DIAG` | 编译期开关（默认 1）：init 后 LP 读回 0xDA/0xBA 校验日志 |

## 验证环境

- ESP32-P4（芯片 v3.2），ESP-IDF v5.5.5，`CONFIG_ESP32P4_REV_MIN_301=y`
- 参考应用：USB 副屏（JPEG 硬解 + PPA 旋转直写 DPI 帧缓冲，持续满带宽刷新 ≈46 fps），长时目检验证无黑斑、无撕裂、无帧边界闪烁

## 作者

梅宝 — [meibao.de](https://meibao.de)（点屏过程与参数推导详见博客）

## License

[MIT](LICENSE)
