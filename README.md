# esp32-ai-agent

ESP32-S3-N16R8 的 ESP-IDF 工程。

## 屏幕

工程通过 `espressif/esp_lcd` 初始化 2.8 寸 SPI TFT 屏幕，控制芯片为
ILI9341，并使用 LVGL 显示主页。主页提供 `WiFi` 按钮，点击后进入 WiFi
页面；该页面会显示当前连接状态，已连接时显示 SSID、IP 和网关信息，未连接时
扫描并列出可用 WiFi，点击 SSID 后可输入密码连接。LCD 和触摸驱动代码位于
`components/LCD`，WiFi station、扫描、连接和 DHCP 状态封装位于
`components/WIFI`。

WiFi 当前只支持 DHCP 获取 IP，TCP/IP 使用 ESP-IDF 默认的 `esp_netif` + lwIP
实现，不处理静态 IP 场景。

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
| 触摸驱动依赖 | `atanisoft/esp_lcd_touch_xpt2046 ^1.0.6` |
| LVGL 依赖 | `lvgl/lvgl ~8.3.11` |
| LVGL 内存 | `CONFIG_LV_MEM_CUSTOM=y` |
| 中文显示 | 默认从独立 `font` SPIFFS 分区加载 `llm_text_14.bin`，覆盖 ASCII、常用汉字和常见中文标点 |

当前大模型回复字库由 `fonts/puhui.ttf` 和 `fonts/常用字.txt` 生成，命令记录在
`fonts/readme.md`。LVGL 8.3 也支持 `lv_font_conv --format bin` 生成的二进制
字体文件；工程中保留了 `fonts/llm_text_14.bin`，并把同一文件放入
`font_active/` 用于生成 `font` 分区镜像。LCD 初始化时会
挂载 `/font`，注册 LVGL 的 `F:` 文件系统驱动，并通过
`lv_font_load("F:/llm_text_14.bin")` 加载字体。若字体分区未刷入或
加载失败，会回退到 `components/LCD/lv_font_chinese_16.c` 中的小型内置字体。
由于二进制字体加载时会占用较多动态内存，工程已启用 `CONFIG_LV_MEM_CUSTOM=y`。

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

## XPT2046 触摸接线

触摸控制器和 LCD 共用同一个 SPI 总线，但必须使用单独的触摸片选 `T_CS`。
默认触摸引脚同样定义在 `components/LCD/include/lcd.h`。

| XPT2046 触摸引脚 | ESP32-S3 引脚 | 说明 |
| --- | --- | --- |
| VCC | 3V3 | 使用 3.3 V 供电和逻辑电平 |
| GND | GND | 共地 |
| T_CLK / TP_CLK | GPIO12 | 与 LCD 的 SCK 共用 |
| T_DIN / TP_DIN | GPIO11 | 与 LCD 的 MOSI 共用 |
| T_DO / TP_DO | GPIO13 | 与 LCD 的 MISO 共用 |
| T_CS / TP_CS | GPIO9 | 触摸控制器片选，不能和 LCD CS 共用 |
| T_IRQ / TP_IRQ / PEN | 不接 | 当前使用轮询读取触摸，代码中为 `GPIO_NUM_NC` |

如果需要使用触摸中断，可以把 `T_IRQ` 接到一个空闲 GPIO，并修改
`LCD_PIN_NUM_TOUCH_IRQ`。

## MAX98357A 喇叭接线

MAX98357A 使用 I2S 数字音频输入，不能直接接模拟音频。默认接线定义在
`components/AUDIO/include/audio.h`，避开了当前已经使用的 LCD、触摸、KEY、
LED 和 USB 引脚。

| MAX98357A 引脚 | ESP32-S3 引脚 | 说明 |
| --- | --- | --- |
| VIN / VDD / PVDD | 5V | 推荐 5 V 供电，4Ω3W 喇叭需要电源至少约 800 mA |
| GND | GND | 与 ESP32-S3 共地 |
| BCLK / BCK | GPIO4 | I2S bit clock |
| LRC / LRCLK / WS | GPIO5 | I2S word select / left-right clock |
| DIN / SDIN | GPIO6 | I2S audio data out from ESP32-S3 |
| GAIN / GAIN_SLOT | 悬空 | 默认约 9 dB；需要更大音量时按芯片手册改电阻 |
| SD / SD_MODE | 通过 1 MΩ 电阻接 VIN / VDDIO | 启用功放并选择单喇叭左右声道混音，不能悬空 |
| OUT+ / SPK+ | 喇叭一端 | 桥接功放输出 |
| OUT- / SPK- | 喇叭另一端 | 不能接 GND |

主页提供 `Play` 按钮，点击后会通过 HTTP 下载并播放
`http://192.168.1.254:5500/music.mp3`。MP3 解码使用 `esphome/micro-mp3`
组件，解码后的 16-bit PCM 通过 I2S 输出到 MAX98357A。裸 BGA/WLP 芯片建议
做 PCB 或使用转接板，电源脚旁边放置 0.1 uF 和 10 uF 去耦电容。`SD_MODE`
使用 1 MΩ 上拉时，单个喇叭会播放 `(Left + Right) / 2` 混音；如果后续要做
双喇叭立体声，再分别按芯片手册为左右声道配置 `SD_MODE`。

## INMP441 录音上传

工程通过 `components/VOICE_UPLOAD` 实现按键录音和 WebSocket 上传。默认按键
使用 `GPIO2`，内部上拉，按钮另一端接 GND，采用灌电流方式；代码使用 GPIO
中断获取按下/松开事件，不做周期轮询。按下后发送 `{"type":"start"}`，
随后持续发送 16 kHz、单声道、16-bit signed little-endian PCM 二进制帧；
松开后发送 `{"type":"end"}`。连接保持长连接，并每 30 秒发送
`{"type":"ping"}` 心跳。服务端返回 `tts_audio` JSON 元信息后，紧随其后的
PCM 二进制分片会进入喇叭播放队列；如果当前没有播放会立即播放，如果正在播放
则排在队列后面顺序输出到 MAX98357A。

默认 WebSocket 地址在 `components/VOICE_UPLOAD/include/voice_upload.h` 的
`VOICE_UPLOAD_WS_URI` 中配置，当前为 `ws://192.168.1.254:3001`。ESP32 上的
`localhost` 表示 ESP32 自己，不是运行服务端的电脑。

| 按键引脚 | ESP32-S3 引脚 | 说明 |
| --- | --- | --- |
| 按钮一端 | GPIO2 | 内部上拉，低电平表示按下 |
| 按钮另一端 | GND | 按下时 GPIO2 被拉到 GND，灌电流方式 |

| INMP441 引脚 | ESP32-S3 引脚 | 说明 |
| --- | --- | --- |
| VDD | 3V3 | 使用 3.3 V 供电 |
| GND | GND | 共地 |
| SCK / BCLK | GPIO16 | I2S bit clock |
| WS / LRCL | GPIO17 | I2S word select |
| SD / DOUT | GPIO18 | I2S data in to ESP32-S3 |
| L/R | GND | 选择左声道；如接 3V3，需要同步调整代码 slot mask |

如果你的板子已经占用了 `GPIO16/17/18`，可以修改
`VOICE_UPLOAD_I2S_PIN_BCLK`、`VOICE_UPLOAD_I2S_PIN_WS` 和
`VOICE_UPLOAD_I2S_PIN_DIN`。
