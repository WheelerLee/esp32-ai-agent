#ifndef VOICE_UPLOAD_H
#define VOICE_UPLOAD_H

#include "driver/gpio.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// ESP32 上的 "localhost" 表示 ESP32 自身，不是运行 WebSocket 服务的电脑。
#ifndef VOICE_UPLOAD_WS_URI
#define VOICE_UPLOAD_WS_URI "ws://192.168.1.254:3001"
#endif

// INMP441 I2S 麦克风引脚；避开本项目已使用的 LCD/触摸 SPI、MAX98357A I2S、USB、LED 和低电平有效按键 GPIO。
#define VOICE_UPLOAD_I2S_PIN_BCLK GPIO_NUM_16
#define VOICE_UPLOAD_I2S_PIN_WS GPIO_NUM_17
#define VOICE_UPLOAD_I2S_PIN_DIN GPIO_NUM_18

#define VOICE_UPLOAD_SAMPLE_RATE_HZ 16000

// 初始化麦克风采集、语音识别、按键处理和上传任务。
esp_err_t voice_upload_init(void);

#ifdef __cplusplus
}
#endif

#endif
