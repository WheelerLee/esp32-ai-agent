#ifndef LCD_H
#define LCD_H

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_err.h"

// ILI9341 屏幕使用 SPI2 硬件主机。
#define LCD_SPI_HOST SPI2_HOST

// ILI9341 物理分辨率为 240x320，这里按横屏逻辑分辨率 320x240 使用。
#define LCD_PIXEL_CLOCK_HZ (40 * 1000 * 1000)
#define LCD_H_RES 320
#define LCD_V_RES 240
#define LCD_DRAW_BUF_LINES 40
#define LCD_SWAP_XY true
#define LCD_MIRROR_X false
#define LCD_MIRROR_Y false

// XPT2046 模块返回 12 位 ADC 原始坐标；触摸校准和方向独立于 LCD 旋转配置。
#define LCD_TOUCH_RAW_X_MIN 200
#define LCD_TOUCH_RAW_X_MAX 3900
#define LCD_TOUCH_RAW_Y_MIN 200
#define LCD_TOUCH_RAW_Y_MAX 3900
#define LCD_TOUCH_SWAP_XY true
#define LCD_TOUCH_MIRROR_X true
#define LCD_TOUCH_MIRROR_Y true

// ESP32-S3 SPI2/FSPI 原生 IO_MUX 引脚。
#define LCD_PIN_NUM_SCLK GPIO_NUM_12
#define LCD_PIN_NUM_MOSI GPIO_NUM_11
#define LCD_PIN_NUM_MISO GPIO_NUM_13
#define LCD_PIN_NUM_CS GPIO_NUM_10

// LCD 控制脚是普通 GPIO，不属于 SPI 总线信号。
#define LCD_PIN_NUM_DC GPIO_NUM_8
#define LCD_PIN_NUM_RST GPIO_NUM_15
#define LCD_PIN_NUM_BK_LIGHT GPIO_NUM_21

// XPT2046 触摸控制器复用 LCD SPI 总线，但使用独立 CS 引脚。
#define LCD_PIN_NUM_TOUCH_CS GPIO_NUM_9
#define LCD_PIN_NUM_TOUCH_IRQ GPIO_NUM_NC

#define LCD_BK_LIGHT_ON_LEVEL 1
#define LCD_BK_LIGHT_OFF_LEVEL !LCD_BK_LIGHT_ON_LEVEL

// 初始化 LCD 面板、触摸输入、LVGL 运行环境和默认界面。
esp_err_t lcd_init(void);

// 在当前界面显示临时的用户说话/聆听指示。
void lcd_show_user_speaking(void);

// 隐藏临时的用户说话/聆听指示。
void lcd_clear_user_speaking(void);

// 在对话区域显示最新识别到的用户问题。
void lcd_show_user_question(const char *text);

// 将助手回复文本追加到对话显示区域。
void lcd_show_text(const char *text);

#endif
