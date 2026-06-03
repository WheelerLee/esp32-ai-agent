#include "esp_err.h"
#include "lcd.h"

void app_main(void)
{
  // Initialize the SPI ILI9341 panel and LVGL, then show one line of text.
  ESP_ERROR_CHECK(lcd_init());
  lcd_show_text("你好 ESP32-S3 横屏");
}
