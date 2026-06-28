#include "esp_err.h"
#include "audio.h"
#include "lcd.h"
#include "voice_upload.h"

// 应用入口：依次启动音频输出、屏幕 UI 和语音上传服务。
void app_main(void)
{
  // 先初始化音频，后续 UI 和 WebSocket 流程才能播放提示音。
  ESP_ERROR_CHECK(audio_init());

  // 初始化 SPI ILI9341 屏幕、触摸、LVGL 和 WiFi 首页界面。
  ESP_ERROR_CHECK(lcd_init());
  ESP_ERROR_CHECK(voice_upload_init());
}
