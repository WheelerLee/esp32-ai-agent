#ifndef VOICE_UPLOAD_H
#define VOICE_UPLOAD_H

#include "driver/gpio.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// "localhost" on ESP32 means the ESP32 itself, not the PC running the
// WebSocket service.
#ifndef VOICE_UPLOAD_WS_URI
#define VOICE_UPLOAD_WS_URI "ws://192.168.1.254:3001"
#endif

// INMP441 I2S microphone pins. Avoid LCD/touch SPI, MAX98357A I2S, USB, LED,
// and the active-low interrupt key GPIOs already used by this project.
#define VOICE_UPLOAD_I2S_PIN_BCLK GPIO_NUM_16
#define VOICE_UPLOAD_I2S_PIN_WS GPIO_NUM_17
#define VOICE_UPLOAD_I2S_PIN_DIN GPIO_NUM_18

#define VOICE_UPLOAD_SAMPLE_RATE_HZ 16000

esp_err_t voice_upload_init(void);

#ifdef __cplusplus
}
#endif

#endif
