#ifndef LCD_H
#define LCD_H

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_err.h"

// SPI2 is the hardware SPI host used by this ILI9341 display.
#define LCD_SPI_HOST SPI2_HOST

// ILI9341 physical resolution is 240x320; use landscape logical 320x240.
#define LCD_PIXEL_CLOCK_HZ (40 * 1000 * 1000)
#define LCD_H_RES 320
#define LCD_V_RES 240
#define LCD_DRAW_BUF_LINES 40
#define LCD_SWAP_XY true
#define LCD_MIRROR_X false
#define LCD_MIRROR_Y false

// XPT2046 modules report raw 12-bit ADC coordinates. Keep calibration and
// orientation independent from the LCD panel rotation.
#define LCD_TOUCH_RAW_X_MIN 200
#define LCD_TOUCH_RAW_X_MAX 3900
#define LCD_TOUCH_RAW_Y_MIN 200
#define LCD_TOUCH_RAW_Y_MAX 3900
#define LCD_TOUCH_SWAP_XY true
#define LCD_TOUCH_MIRROR_X true
#define LCD_TOUCH_MIRROR_Y true

// ESP32-S3 SPI2/FSPI native IO_MUX pins.
#define LCD_PIN_NUM_SCLK GPIO_NUM_12
#define LCD_PIN_NUM_MOSI GPIO_NUM_11
#define LCD_PIN_NUM_MISO GPIO_NUM_13
#define LCD_PIN_NUM_CS GPIO_NUM_10

// LCD control pins are regular GPIOs, not SPI bus signals.
#define LCD_PIN_NUM_DC GPIO_NUM_8
#define LCD_PIN_NUM_RST GPIO_NUM_15
#define LCD_PIN_NUM_BK_LIGHT GPIO_NUM_21

// XPT2046 touch controller shares the LCD SPI bus but uses its own CS pin.
#define LCD_PIN_NUM_TOUCH_CS GPIO_NUM_9
#define LCD_PIN_NUM_TOUCH_IRQ GPIO_NUM_NC

#define LCD_BK_LIGHT_ON_LEVEL 1
#define LCD_BK_LIGHT_OFF_LEVEL !LCD_BK_LIGHT_ON_LEVEL

esp_err_t lcd_init(void);
void lcd_show_text(const char *text);

#endif
