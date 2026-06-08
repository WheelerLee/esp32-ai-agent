#ifndef AUDIO_H
#define AUDIO_H

#include <stddef.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*audio_pcm_playback_text_cb_t)(const char *text);

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
esp_err_t audio_queue_pcm_s16le(const void *pcm,
                                size_t bytes,
                                uint32_t sample_rate_hz,
                                int channels);
esp_err_t audio_queue_pcm_s16le_with_text(const void *pcm,
                                          size_t bytes,
                                          uint32_t sample_rate_hz,
                                          int channels,
                                          const char *text);
void audio_set_pcm_playback_text_cb(audio_pcm_playback_text_cb_t cb);
int audio_volume_up(void);
int audio_volume_down(void);
int audio_get_volume_level(void);

#ifdef __cplusplus
}
#endif

#endif
