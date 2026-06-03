# esp32-ai-agent

ESP32-S3-N16R8 的 ESP-IDF 工程。

## 屏幕

工程通过 `espressif/esp_lcd` 初始化 2.8 寸 SPI TFT 屏幕，控制芯片为
ILI9341，并使用 LVGL 在屏幕中央显示一行文字。LCD 驱动代码位于
`components/LCD`。

默认屏幕参数：

| 项目 | 值 |
| --- | --- |
| MCU | ESP32-S3-N16R8 |
| 屏幕 | 2.8 寸 TFT SPI |
| LCD 控制芯片 | ILI9341 |
| 物理分辨率 | 240 x 320 |
| 显示方向 | 横屏，逻辑分辨率 320 x 240，`swap_xy=true, mirror_x=false, mirror_y=false` |
| SPI 主机 | SPI2_HOST |
| SPI 引脚 | ESP32-S3 SPI2/FSPI 原生 IO_MUX 引脚 |
| 像素时钟 | 40 MHz |
| 色深 | RGB565, 16 bpp |
| LCD 驱动依赖 | `espressif/esp_lcd_ili9341 ^2.0.2` |
| LVGL 依赖 | `lvgl/lvgl ~8.3.11` |
| 中文显示 | 使用 15px 预渲染中文 alpha 位图，白底黑字，避免 LVGL 内置 CJK 字体缺字 |

## ILI9341 接线

默认接线定义在 `components/LCD/include/lcd.h`。

| ILI9341 引脚 | ESP32-S3 引脚 | 说明 |
| --- | --- | --- |
| VCC | 3V3 | 使用 3.3 V 供电和逻辑电平 |
| GND | GND | 共地 |
| SCK / SCL / CLK | GPIO12 | SPI2/FSPI 原生 SCLK |
| MOSI / SDA / SDI | GPIO11 | SPI2/FSPI 原生 FSPID |
| MISO / SDO | GPIO13 | SPI2/FSPI 原生 FSPIQ；不读屏时可不接 |
| CS | GPIO10 | SPI2/FSPI 原生 CS0 |
| DC / RS | GPIO8 | 数据/命令选择，普通 GPIO |
| RST / RESET | GPIO15 | LCD 复位，普通 GPIO |
| LED / BL | GPIO21 | 背光控制，普通 GPIO，高电平点亮 |

如果你的 TFT 模块已经将复位或背光直接接到电源，可以在硬件上不接对应 GPIO，
或修改 `components/LCD/include/lcd.h` 中的宏定义。
