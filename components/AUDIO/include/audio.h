#ifndef AUDIO_H
#define AUDIO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*audio_pcm_playback_text_cb_t)(const char *text);
typedef void (*audio_pcm_playback_ref_cb_t)(const int16_t *pcm,
                                            size_t frames,
                                            int channels,
                                            uint32_t sample_rate_hz);

// MAX98357A I2S 引脚；这些 GPIO 当前未被 LCD、触摸、按键、LED、USB 和启动绑带占用。
#define AUDIO_I2S_PIN_BCLK GPIO_NUM_4
#define AUDIO_I2S_PIN_LRCLK GPIO_NUM_5
#define AUDIO_I2S_PIN_DIN GPIO_NUM_6

#define AUDIO_I2S_SAMPLE_RATE_HZ 16000
#define AUDIO_MP3_INPUT_BUFFER_BYTES 8192

// 初始化 MAX98357A I2S 播放通道和后台 PCM 播放任务。
esp_err_t audio_init(void);

// 播放一段本地短音，用于验证扬声器和 I2S 输出。
esp_err_t audio_play_test_tone(void);

// 从 HTTP 拉取 MP3 流，解码后写入 I2S。
esp_err_t audio_play_mp3_url(const char *url);

// 队列播放 signed 16-bit little-endian PCM，不附带显示文本。
esp_err_t audio_queue_pcm_s16le(const void *pcm,
                                size_t bytes,
                                uint32_t sample_rate_hz,
                                int channels);

// 队列播放 signed 16-bit little-endian PCM，并可附带用于 UI 回调的文本。
esp_err_t audio_queue_pcm_s16le_with_text(const void *pcm,
                                          size_t bytes,
                                          uint32_t sample_rate_hz,
                                          int channels,
                                          const char *text);

// 停止当前播放并清空所有排队的 PCM 缓冲。
void audio_stop_playback(void);

// 当前正在播放或队列中仍有缓冲时返回 true。
bool audio_is_playback_active(void);

// 注册 TTS 文本开始播放时触发的回调。
void audio_set_pcm_playback_text_cb(audio_pcm_playback_text_cb_t cb);

// 注册播放参考 PCM 回调，用于回声处理。
void audio_set_pcm_playback_ref_cb(audio_pcm_playback_ref_cb_t cb);

// 提高并持久化音量等级，返回限制在 0-10 的结果。
int audio_volume_up(void);

// 降低并持久化音量等级，返回限制在 0-10 的结果。
int audio_volume_down(void);

// 读取当前 0-10 的播放音量等级。
int audio_get_volume_level(void);

#ifdef __cplusplus
}
#endif

#endif
