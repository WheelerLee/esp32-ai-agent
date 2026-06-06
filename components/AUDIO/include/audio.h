#ifndef AUDIO_H
#define AUDIO_H

#include "driver/gpio.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// MAX98357A I2S pins. These GPIOs are currently unused by LCD, touch, key, LED,
// USB, and boot strapping in this project.
#define AUDIO_I2S_PIN_BCLK GPIO_NUM_4
#define AUDIO_I2S_PIN_LRCLK GPIO_NUM_5
#define AUDIO_I2S_PIN_DIN GPIO_NUM_6

#define AUDIO_I2S_SAMPLE_RATE_HZ 16000
#define AUDIO_MP3_INPUT_BUFFER_BYTES 8192

esp_err_t audio_init(void);
esp_err_t audio_play_test_tone(void);
esp_err_t audio_play_mp3_url(const char *url);
int audio_volume_up(void);
int audio_volume_down(void);
int audio_get_volume_percent(void);

#ifdef __cplusplus
}
#endif

#endif
