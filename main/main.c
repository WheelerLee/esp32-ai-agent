#include "esp_err.h"
#include "audio.h"
#include "lcd.h"

void app_main(void)
{
  ESP_ERROR_CHECK(audio_init());

  // Initialize the SPI ILI9341 panel, touch, LVGL, and the WiFi home UI.
  ESP_ERROR_CHECK(lcd_init());
}
