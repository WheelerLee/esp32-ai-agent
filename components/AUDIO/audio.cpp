#include "audio.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "micro_mp3/mp3_decoder.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "audio";

#define AUDIO_NVS_NAMESPACE "audio"
#define AUDIO_NVS_KEY_VOLUME "volume"

static i2s_chan_handle_t s_i2s_tx_chan;
static SemaphoreHandle_t s_audio_mutex;
static QueueHandle_t s_pcm_queue;
static TaskHandle_t s_pcm_play_task_handle;
static uint32_t s_current_sample_rate;
static bool s_i2s_enabled;
static volatile int s_volume_level = 6;
static audio_pcm_playback_text_cb_t s_pcm_playback_text_cb;

enum {
  audio_volume_min_level = 0,
  audio_volume_max_level = 10,
  audio_volume_default_level = 6,
  audio_volume_percent_per_level = 20,
  audio_pcm_queue_depth = 16,
  audio_i2s_dma_desc_num = 12,
  audio_i2s_dma_frame_num = 512,
  audio_pcm_play_task_stack = 8192,
  audio_pcm_play_task_priority = 7,
  audio_playback_tail_silence_ms = 80,
  audio_i2s_idle_stop_delay_ms = 700,
  audio_start_prebuffer_ms = 350,
  audio_mono_write_chunk_frames = 4096,
  audio_text_task_stack = 4096,
  audio_text_task_priority = 4,
};

typedef struct {
  uint8_t *data;
  char *text;
  size_t bytes;
  uint32_t sample_rate_hz;
  int channels;
  int64_t enqueue_us;
} audio_pcm_queue_item_t;

static char *audio_strdup(const char *text)
{
  if (text == nullptr || text[0] == '\0') {
    return nullptr;
  }

  size_t len = strlen(text);
  char *copy = (char *)malloc(len + 1);
  if (copy != nullptr) {
    memcpy(copy, text, len + 1);
  }
  return copy;
}

static void audio_text_task(void *arg)
{
  char *text = (char *)arg;
  audio_pcm_playback_text_cb_t cb = s_pcm_playback_text_cb;
  if (cb != nullptr) {
    cb(text != nullptr ? text : "");
  }
  free(text);
  vTaskDelete(nullptr);
}

static void audio_notify_playback_text_async(const char *text)
{
  if (s_pcm_playback_text_cb == nullptr || text == nullptr || text[0] == '\0') {
    return;
  }

  char *text_copy = audio_strdup(text);
  if (text_copy == nullptr) {
    ESP_LOGW(TAG, "copy playback text for async UI update failed");
    return;
  }

  BaseType_t task_ret = xTaskCreate(audio_text_task,
                                    "audio_text",
                                    audio_text_task_stack,
                                    text_copy,
                                    audio_text_task_priority,
                                    nullptr);
  if (task_ret != pdPASS) {
    ESP_LOGW(TAG, "create playback text task failed");
    free(text_copy);
  }
}

static int audio_clamp_volume_level(int volume_level)
{
  if (volume_level < audio_volume_min_level) {
    return audio_volume_min_level;
  }
  if (volume_level > audio_volume_max_level) {
    return audio_volume_max_level;
  }
  return volume_level;
}

static int audio_level_to_percent(int volume_level)
{
  return audio_clamp_volume_level(volume_level) * audio_volume_percent_per_level;
}

static esp_err_t audio_init_nvs(void)
{
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "erase NVS failed");
    err = nvs_flash_init();
  }
  return err;
}

static void audio_load_volume_level(void)
{
  esp_err_t err = audio_init_nvs();
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "init NVS for volume failed: %s", esp_err_to_name(err));
    s_volume_level = audio_volume_default_level;
    return;
  }

  nvs_handle_t nvs_handle;
  err = nvs_open(AUDIO_NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
  if (err == ESP_ERR_NVS_NOT_FOUND) {
    s_volume_level = audio_volume_default_level;
    ESP_LOGI(TAG, "no saved volume, use default: %d", s_volume_level);
    return;
  }
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "open volume NVS failed: %s", esp_err_to_name(err));
    s_volume_level = audio_volume_default_level;
    return;
  }

  int32_t saved_level = audio_volume_default_level;
  err = nvs_get_i32(nvs_handle, AUDIO_NVS_KEY_VOLUME, &saved_level);
  nvs_close(nvs_handle);

  if (err == ESP_ERR_NVS_NOT_FOUND) {
    s_volume_level = audio_volume_default_level;
  } else if (err == ESP_OK) {
    s_volume_level = audio_clamp_volume_level(saved_level);
  } else {
    ESP_LOGW(TAG, "read saved volume failed: %s", esp_err_to_name(err));
    s_volume_level = audio_volume_default_level;
  }

  ESP_LOGI(TAG, "volume level loaded: %d", s_volume_level);
}

static void audio_save_volume_level(int volume_level)
{
  nvs_handle_t nvs_handle;
  esp_err_t err = nvs_open(AUDIO_NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "open volume NVS for write failed: %s", esp_err_to_name(err));
    return;
  }

  err = nvs_set_i32(nvs_handle, AUDIO_NVS_KEY_VOLUME, audio_clamp_volume_level(volume_level));
  if (err == ESP_OK) {
    err = nvs_commit(nvs_handle);
  }
  nvs_close(nvs_handle);

  if (err != ESP_OK) {
    ESP_LOGW(TAG, "save volume failed: %s", esp_err_to_name(err));
  }
}

static int16_t audio_abs_i16(int16_t value)
{
  if (value == INT16_MIN) {
    return INT16_MAX;
  }
  return value < 0 ? -value : value;
}

static int16_t audio_pcm_peak(const int16_t *pcm, size_t sample_count)
{
  int16_t peak = 0;
  for (size_t i = 0; i < sample_count; ++i) {
    int16_t abs_sample = audio_abs_i16(pcm[i]);
    if (abs_sample > peak) {
      peak = abs_sample;
    }
  }
  return peak;
}

static void audio_apply_gain(int16_t *pcm, size_t sample_count)
{
  int volume_percent = audio_level_to_percent(s_volume_level);
  if (volume_percent == 100) {
    return;
  }

  for (size_t i = 0; i < sample_count; ++i) {
    int32_t sample = (int32_t)pcm[i] * volume_percent / 100;
    if (sample > INT16_MAX) {
      sample = INT16_MAX;
    } else if (sample < INT16_MIN) {
      sample = INT16_MIN;
    }
    pcm[i] = (int16_t)sample;
  }
}

static void audio_log_pcm_stats(const char *prefix,
                                size_t frame,
                                const int16_t *pcm,
                                size_t samples_per_channel,
                                int channels,
                                size_t bytes_written,
                                size_t input_len,
                                size_t total_http_bytes,
                                int16_t peak_before_gain,
                                int16_t peak_after_gain)
{
  size_t pcm_value_count = samples_per_channel * (channels > 1 ? 2 : 1);
  ESP_LOGI(TAG,
           "%s frame=%u samples/ch=%u channels=%d peak=%d volume=%d/10 peak_out=%d first=[%d,%d,%d,%d] i2s=%u bytes buffered=%u downloaded=%u",
           prefix,
           (unsigned)frame,
           (unsigned)samples_per_channel,
           channels,
           peak_before_gain,
           audio_get_volume_level(),
           peak_after_gain,
           pcm_value_count > 0 ? pcm[0] : 0,
           pcm_value_count > 1 ? pcm[1] : 0,
           pcm_value_count > 2 ? pcm[2] : 0,
           pcm_value_count > 3 ? pcm[3] : 0,
           (unsigned)bytes_written,
           (unsigned)input_len,
           (unsigned)total_http_bytes);
}

static esp_err_t audio_configure_i2s(uint32_t sample_rate_hz)
{
  if (sample_rate_hz == 0) {
    return ESP_ERR_INVALID_ARG;
  }

  if (s_current_sample_rate == sample_rate_hz && s_i2s_enabled) {
    ESP_LOGI(TAG, "I2S already configured: %lu Hz", (unsigned long)sample_rate_hz);
    return ESP_OK;
  }

  if (s_i2s_enabled) {
    ESP_LOGI(TAG,
             "reconfigure I2S sample rate: %lu Hz -> %lu Hz",
             (unsigned long)s_current_sample_rate,
             (unsigned long)sample_rate_hz);
    ESP_RETURN_ON_ERROR(i2s_channel_disable(s_i2s_tx_chan), TAG, "disable I2S tx failed");
    s_i2s_enabled = false;
  }

  i2s_std_clk_config_t clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate_hz);
  i2s_std_slot_config_t slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                                       I2S_SLOT_MODE_STEREO);

  ESP_RETURN_ON_ERROR(i2s_channel_reconfig_std_clock(s_i2s_tx_chan, &clk_cfg),
                      TAG,
                      "reconfigure I2S clock failed");
  ESP_RETURN_ON_ERROR(i2s_channel_reconfig_std_slot(s_i2s_tx_chan, &slot_cfg),
                      TAG,
                      "reconfigure I2S slot failed");
  ESP_RETURN_ON_ERROR(i2s_channel_enable(s_i2s_tx_chan), TAG, "enable I2S tx failed");

  s_current_sample_rate = sample_rate_hz;
  s_i2s_enabled = true;
  ESP_LOGI(TAG, "I2S configured: %lu Hz, 16-bit stereo Philips format", (unsigned long)sample_rate_hz);
  return ESP_OK;
}

static esp_err_t audio_write_pcm(const int16_t *pcm,
                                 size_t samples_per_channel,
                                 int channels,
                                 size_t *bytes_written_total)
{
  if (pcm == nullptr || samples_per_channel == 0 || channels <= 0) {
    if (bytes_written_total != nullptr) {
      *bytes_written_total = 0;
    }
    return ESP_OK;
  }

  size_t total_written = 0;
  if (channels >= 2) {
    size_t bytes_written = 0;
    size_t expected_bytes = samples_per_channel * 2 * sizeof(int16_t);
    esp_err_t err = i2s_channel_write(s_i2s_tx_chan,
                                      pcm,
                                      expected_bytes,
                                      &bytes_written,
                                      portMAX_DELAY);
    if (bytes_written_total != nullptr) {
      *bytes_written_total = bytes_written;
    }
    if (err == ESP_OK && bytes_written != expected_bytes) {
      ESP_LOGW(TAG, "partial I2S stereo write: %u/%u bytes", (unsigned)bytes_written, (unsigned)expected_bytes);
    }
    return err;
  }

  int16_t *stereo = (int16_t *)malloc(audio_mono_write_chunk_frames * 2 * sizeof(int16_t));
  ESP_RETURN_ON_FALSE(stereo != nullptr, ESP_ERR_NO_MEM, TAG, "allocate mono conversion buffer failed");

  size_t offset = 0;
  while (offset < samples_per_channel) {
    size_t chunk = samples_per_channel - offset;
    if (chunk > audio_mono_write_chunk_frames) {
      chunk = audio_mono_write_chunk_frames;
    }

    for (size_t i = 0; i < chunk; ++i) {
      stereo[i * 2] = pcm[offset + i];
      stereo[i * 2 + 1] = pcm[offset + i];
    }

    size_t bytes_written = 0;
    size_t expected_bytes = chunk * 2 * sizeof(int16_t);
    esp_err_t err = i2s_channel_write(s_i2s_tx_chan,
                                      stereo,
                                      expected_bytes,
                                      &bytes_written,
                                      portMAX_DELAY);
    if (err != ESP_OK) {
      free(stereo);
      ESP_LOGE(TAG, "write mono PCM failed: %s", esp_err_to_name(err));
      return err;
    }
    if (bytes_written != expected_bytes) {
      ESP_LOGW(TAG, "partial I2S mono write: %u/%u bytes", (unsigned)bytes_written, (unsigned)expected_bytes);
    }
    total_written += bytes_written;
    offset += chunk;
  }

  if (bytes_written_total != nullptr) {
    *bytes_written_total = total_written;
  }
  free(stereo);
  return ESP_OK;
}

static esp_err_t audio_write_tail_silence(uint32_t sample_rate_hz)
{
  if (sample_rate_hz == 0) {
    return ESP_ERR_INVALID_ARG;
  }

  int16_t silence[128 * 2] = {};
  size_t silence_frames = (size_t)sample_rate_hz * audio_playback_tail_silence_ms / 1000;
  while (silence_frames > 0) {
    size_t chunk = silence_frames;
    if (chunk > 128) {
      chunk = 128;
    }

    size_t bytes_written = 0;
    ESP_RETURN_ON_ERROR(audio_write_pcm(silence, chunk, 2, &bytes_written),
                        TAG,
                        "write playback tail silence failed");
    silence_frames -= chunk;
  }

  return ESP_OK;
}

static esp_err_t audio_stop_i2s_when_idle(void)
{
  if (!s_i2s_enabled || s_pcm_queue == nullptr || uxQueueMessagesWaiting(s_pcm_queue) > 0) {
    return ESP_OK;
  }

  ESP_RETURN_ON_ERROR(audio_write_tail_silence(s_current_sample_rate),
                      TAG,
                      "flush playback tail failed");
  ESP_RETURN_ON_ERROR(i2s_channel_disable(s_i2s_tx_chan), TAG, "disable idle I2S tx failed");
  s_i2s_enabled = false;
  return ESP_OK;
}

static void audio_pcm_play_task(void *arg)
{
  (void)arg;
  bool playback_idle = true;

  while (true) {
    audio_pcm_queue_item_t item = {};
    if (xQueueReceive(s_pcm_queue, &item, pdMS_TO_TICKS(audio_i2s_idle_stop_delay_ms)) != pdTRUE) {
      if (xSemaphoreTake(s_audio_mutex, portMAX_DELAY) == pdTRUE) {
        esp_err_t idle_ret = audio_stop_i2s_when_idle();
        if (idle_ret != ESP_OK) {
          ESP_LOGW(TAG, "stop idle I2S failed: %s", esp_err_to_name(idle_ret));
        }
        xSemaphoreGive(s_audio_mutex);
      }
      playback_idle = true;
      continue;
    }

    if (item.data == nullptr || item.bytes == 0) {
      free(item.data);
      free(item.text);
      continue;
    }

    if (playback_idle) {
      vTaskDelay(pdMS_TO_TICKS(audio_start_prebuffer_ms));
    }

    if (xSemaphoreTake(s_audio_mutex, portMAX_DELAY) != pdTRUE) {
      free(item.data);
      free(item.text);
      continue;
    }

    esp_err_t ret = audio_configure_i2s(item.sample_rate_hz);
    if (ret == ESP_OK) {
      audio_notify_playback_text_async(item.text);

      size_t sample_count = item.bytes / sizeof(int16_t);
      int16_t *pcm = (int16_t *)item.data;
      int16_t peak_before_gain = audio_pcm_peak(pcm, sample_count);
      audio_apply_gain(pcm, sample_count);
      int16_t peak_after_gain = audio_pcm_peak(pcm, sample_count);

      size_t samples_per_channel = sample_count / (size_t)item.channels;
      size_t bytes_written = 0;
      int64_t write_start_us = esp_timer_get_time();
      ret = audio_write_pcm(pcm, samples_per_channel, item.channels, &bytes_written);
      int64_t write_us = esp_timer_get_time() - write_start_us;
      if (ret != ESP_OK) {
        ESP_LOGE(TAG, "write queued PCM failed: %s", esp_err_to_name(ret));
      } else if (bytes_written > 0) {
        uint32_t audio_ms = (uint32_t)(samples_per_channel * 1000U / item.sample_rate_hz);
        int64_t queue_wait_us = write_start_us - item.enqueue_us;
        ESP_LOGI(TAG,
                 "queued PCM played: %u bytes, %lu Hz, channels=%d, audio_ms=%u queue_wait=%lld ms write=%lld ms q_left=%u volume=%d/10 peak=%d peak_out=%d i2s=%u",
                 (unsigned)item.bytes,
                 (unsigned long)item.sample_rate_hz,
                 item.channels,
                 (unsigned)audio_ms,
                 (long long)(queue_wait_us / 1000),
                 (long long)(write_us / 1000),
                 (unsigned)uxQueueMessagesWaiting(s_pcm_queue),
                 audio_get_volume_level(),
                 peak_before_gain,
                 peak_after_gain,
                 (unsigned)bytes_written);
      }
    } else {
      ESP_LOGE(TAG,
               "configure queued PCM failed: %s, sample_rate=%lu",
               esp_err_to_name(ret),
               (unsigned long)item.sample_rate_hz);
    }

    xSemaphoreGive(s_audio_mutex);
    playback_idle = false;
    free(item.data);
    free(item.text);
  }
}

extern "C" esp_err_t audio_init(void)
{
  if (s_i2s_tx_chan != nullptr) {
    return ESP_OK;
  }

  audio_load_volume_level();

  s_audio_mutex = xSemaphoreCreateMutex();
  ESP_RETURN_ON_FALSE(s_audio_mutex != nullptr, ESP_ERR_NO_MEM, TAG, "create audio mutex failed");

  i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  chan_cfg.dma_desc_num = audio_i2s_dma_desc_num;
  chan_cfg.dma_frame_num = audio_i2s_dma_frame_num;
  chan_cfg.auto_clear_after_cb = true;
  chan_cfg.auto_clear_before_cb = true;
  ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_i2s_tx_chan, nullptr),
                      TAG,
                      "create I2S tx channel failed");

  i2s_std_config_t std_cfg = {
    .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_I2S_SAMPLE_RATE_HZ),
    .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                    I2S_SLOT_MODE_STEREO),
    .gpio_cfg = {
      .mclk = I2S_GPIO_UNUSED,
      .bclk = AUDIO_I2S_PIN_BCLK,
      .ws = AUDIO_I2S_PIN_LRCLK,
      .dout = AUDIO_I2S_PIN_DIN,
      .din = I2S_GPIO_UNUSED,
      .invert_flags = {
        .mclk_inv = false,
        .bclk_inv = false,
        .ws_inv = false,
      },
    },
  };

  ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_i2s_tx_chan, &std_cfg),
                      TAG,
                      "init I2S std mode failed");
  ESP_RETURN_ON_ERROR(i2s_channel_enable(s_i2s_tx_chan), TAG, "enable I2S tx failed");

  s_current_sample_rate = AUDIO_I2S_SAMPLE_RATE_HZ;
  s_i2s_enabled = true;

  s_pcm_queue = xQueueCreate(audio_pcm_queue_depth, sizeof(audio_pcm_queue_item_t));
  ESP_RETURN_ON_FALSE(s_pcm_queue != nullptr, ESP_ERR_NO_MEM, TAG, "create PCM queue failed");

  BaseType_t task_ret = xTaskCreatePinnedToCore(audio_pcm_play_task,
                                                "audio_pcm_play",
                                                audio_pcm_play_task_stack,
                                                nullptr,
                                                audio_pcm_play_task_priority,
                                                &s_pcm_play_task_handle,
                                                1);
  ESP_RETURN_ON_FALSE(task_ret == pdPASS, ESP_ERR_NO_MEM, TAG, "create PCM playback task failed");

  ESP_LOGI(TAG,
           "MAX98357A I2S ready: BCLK=%d LRCLK=%d DIN=%d volume=%d/10 dma=%dx%d task_prio=%d",
           AUDIO_I2S_PIN_BCLK,
           AUDIO_I2S_PIN_LRCLK,
           AUDIO_I2S_PIN_DIN,
           audio_get_volume_level(),
           audio_i2s_dma_desc_num,
           audio_i2s_dma_frame_num,
           audio_pcm_play_task_priority);
  return ESP_OK;
}

extern "C" esp_err_t audio_queue_pcm_s16le(const void *pcm,
                                           size_t bytes,
                                           uint32_t sample_rate_hz,
                                           int channels)
{
  return audio_queue_pcm_s16le_with_text(pcm, bytes, sample_rate_hz, channels, nullptr);
}

extern "C" esp_err_t audio_queue_pcm_s16le_with_text(const void *pcm,
                                                     size_t bytes,
                                                     uint32_t sample_rate_hz,
                                                     int channels,
                                                     const char *text)
{
  ESP_RETURN_ON_ERROR(audio_init(), TAG, "audio init failed");
  ESP_RETURN_ON_FALSE(pcm != nullptr && bytes > 0, ESP_ERR_INVALID_ARG, TAG, "empty PCM");
  ESP_RETURN_ON_FALSE((bytes % sizeof(int16_t)) == 0, ESP_ERR_INVALID_ARG, TAG, "unaligned PCM");
  ESP_RETURN_ON_FALSE(sample_rate_hz > 0, ESP_ERR_INVALID_ARG, TAG, "invalid sample rate");
  ESP_RETURN_ON_FALSE(channels == 1 || channels == 2, ESP_ERR_NOT_SUPPORTED, TAG, "unsupported channel count: %d", channels);

  size_t sample_count = bytes / sizeof(int16_t);
  ESP_RETURN_ON_FALSE((sample_count % (size_t)channels) == 0,
                      ESP_ERR_INVALID_ARG,
                      TAG,
                      "PCM sample count does not match channels");

  uint8_t *copy = (uint8_t *)malloc(bytes);
  ESP_RETURN_ON_FALSE(copy != nullptr, ESP_ERR_NO_MEM, TAG, "copy queued PCM failed");
  memcpy(copy, pcm, bytes);

  char *text_copy = audio_strdup(text);
  if (text != nullptr && text[0] != '\0' && text_copy == nullptr) {
    free(copy);
    ESP_LOGW(TAG, "copy queued PCM text failed");
    return ESP_ERR_NO_MEM;
  }

  audio_pcm_queue_item_t item = {
    .data = copy,
    .text = text_copy,
    .bytes = bytes,
    .sample_rate_hz = sample_rate_hz,
    .channels = channels,
    .enqueue_us = esp_timer_get_time(),
  };

  if (xQueueSend(s_pcm_queue, &item, portMAX_DELAY) != pdTRUE) {
    free(copy);
    free(text_copy);
    ESP_LOGW(TAG, "PCM playback queue is full");
    return ESP_ERR_TIMEOUT;
  }

  return ESP_OK;
}

extern "C" void audio_set_pcm_playback_text_cb(audio_pcm_playback_text_cb_t cb)
{
  s_pcm_playback_text_cb = cb;
}

extern "C" int audio_get_volume_level(void)
{
  return s_volume_level;
}

extern "C" int audio_volume_up(void)
{
  s_volume_level = audio_clamp_volume_level(s_volume_level + 1);
  audio_save_volume_level(s_volume_level);
  ESP_LOGI(TAG, "volume up: %d/10", s_volume_level);
  return s_volume_level;
}

extern "C" int audio_volume_down(void)
{
  s_volume_level = audio_clamp_volume_level(s_volume_level - 1);
  audio_save_volume_level(s_volume_level);
  ESP_LOGI(TAG, "volume down: %d/10", s_volume_level);
  return s_volume_level;
}

extern "C" esp_err_t audio_play_test_tone(void)
{
  ESP_RETURN_ON_ERROR(audio_init(), TAG, "audio init failed");

  enum {
    tone_hz = 880,
    duration_ms = 180,
    amplitude = 1200,
    frames_per_chunk = 128,
  };

  ESP_RETURN_ON_ERROR(audio_configure_i2s(AUDIO_I2S_SAMPLE_RATE_HZ), TAG, "configure test tone failed");
  ESP_LOGI(TAG,
           "play test tone: %d Hz, %d ms, amplitude=%d",
           tone_hz,
           duration_ms,
           amplitude);

  int16_t samples[frames_per_chunk * 2];
  const int total_frames = AUDIO_I2S_SAMPLE_RATE_HZ * duration_ms / 1000;
  int written_frames = 0;

  while (written_frames < total_frames) {
    int chunk_frames = total_frames - written_frames;
    if (chunk_frames > frames_per_chunk) {
      chunk_frames = frames_per_chunk;
    }

    for (int i = 0; i < chunk_frames; ++i) {
      float phase = 2.0f * (float)M_PI * (float)tone_hz *
                    (float)(written_frames + i) / (float)AUDIO_I2S_SAMPLE_RATE_HZ;
      int16_t sample = (int16_t)((float)amplitude * sinf(phase));
      samples[i * 2] = sample;
      samples[i * 2 + 1] = sample;
    }
    audio_apply_gain(samples, chunk_frames * 2);

    size_t bytes_written = 0;
    ESP_RETURN_ON_ERROR(audio_write_pcm(samples, chunk_frames, 2, &bytes_written),
                        TAG,
                        "write test tone failed");
    written_frames += chunk_frames;
  }

  return ESP_OK;
}

extern "C" esp_err_t audio_play_mp3_url(const char *url)
{
  ESP_RETURN_ON_ERROR(audio_init(), TAG, "audio init failed");
  ESP_RETURN_ON_FALSE(url != nullptr && url[0] != '\0', ESP_ERR_INVALID_ARG, TAG, "empty URL");

  if (xSemaphoreTake(s_audio_mutex, 0) != pdTRUE) {
    ESP_LOGW(TAG, "audio playback is already running");
    return ESP_ERR_INVALID_STATE;
  }

  esp_err_t ret = ESP_OK;
  esp_http_client_handle_t client = nullptr;
  uint8_t *input_buf = nullptr;
  int16_t *pcm_buf = nullptr;
  micro_mp3::Mp3Decoder decoder;
  size_t input_len = 0;
  bool stream_finished = false;
  bool stream_configured = false;
  size_t total_http_bytes = 0;
  size_t decoded_frames = 0;
  size_t total_pcm_frames = 0;
  size_t total_i2s_bytes = 0;
  size_t need_more_count = 0;
  size_t decode_error_count = 0;
  size_t zero_peak_frames = 0;
  int16_t max_peak = 0;
  int64_t content_length = 0;

  esp_http_client_config_t http_cfg = {};
  http_cfg.url = url;
  http_cfg.timeout_ms = 8000;
  http_cfg.buffer_size = 4096;

  ESP_LOGI(TAG, "start MP3 playback: %s", url);
  client = esp_http_client_init(&http_cfg);
  ESP_GOTO_ON_FALSE(client != nullptr, ESP_ERR_NO_MEM, cleanup, TAG, "create HTTP client failed");
  ESP_GOTO_ON_ERROR(esp_http_client_open(client, 0), cleanup, TAG, "open MP3 URL failed");
  content_length = esp_http_client_fetch_headers(client);
  ESP_LOGI(TAG,
           "HTTP status=%d content_length=%lld",
           esp_http_client_get_status_code(client),
           (long long)content_length);

  input_buf = (uint8_t *)malloc(AUDIO_MP3_INPUT_BUFFER_BYTES);
  pcm_buf = (int16_t *)malloc(micro_mp3::MP3_MIN_OUTPUT_BUFFER_BYTES);
  ESP_GOTO_ON_FALSE(input_buf != nullptr && pcm_buf != nullptr,
                    ESP_ERR_NO_MEM,
                    cleanup,
                    TAG,
                    "allocate MP3 buffers failed");

  while (!stream_finished || input_len > 0) {
    if (!stream_finished && input_len < AUDIO_MP3_INPUT_BUFFER_BYTES / 2) {
      int read_len = esp_http_client_read(client,
                                         (char *)input_buf + input_len,
                                         AUDIO_MP3_INPUT_BUFFER_BYTES - input_len);
      if (read_len < 0) {
        ret = ESP_FAIL;
        ESP_LOGE(TAG, "read MP3 stream failed");
        break;
      }
      if (read_len == 0) {
        stream_finished = true;
        ESP_LOGI(TAG,
                 "HTTP stream ended: downloaded=%u bytes buffered=%u bytes",
                 (unsigned)total_http_bytes,
                 (unsigned)input_len);
      } else {
        input_len += (size_t)read_len;
        total_http_bytes += (size_t)read_len;
      }
    }

    if (input_len == 0) {
      continue;
    }

    size_t consumed = 0;
    size_t samples = 0;
    micro_mp3::Mp3Result result = decoder.decode(input_buf,
                                                 input_len,
                                                 (uint8_t *)pcm_buf,
                                                 micro_mp3::MP3_MIN_OUTPUT_BUFFER_BYTES,
                                                 consumed,
                                                 samples);
    if (consumed > 0) {
      memmove(input_buf, input_buf + consumed, input_len - consumed);
      input_len -= consumed;
    }

    if (result == micro_mp3::MP3_STREAM_INFO_READY && !stream_configured) {
      ESP_LOGI(TAG,
               "MP3 stream: %d Hz, %d channel(s), %d kbps",
               decoder.get_sample_rate(),
               decoder.get_channels(),
               decoder.get_bitrate());
      ret = audio_configure_i2s((uint32_t)decoder.get_sample_rate());
      if (ret != ESP_OK) {
        break;
      }
      stream_configured = true;
      continue;
    }

    if (result == micro_mp3::MP3_NEED_MORE_DATA) {
      ++need_more_count;
      if (stream_finished) {
        ESP_LOGW(TAG,
                 "decoder needs more data after HTTP EOF: buffered=%u consumed=%u",
                 (unsigned)input_len,
                 (unsigned)consumed);
        break;
      }
      continue;
    }

    if (result == micro_mp3::MP3_DECODE_ERROR) {
      ++decode_error_count;
      ESP_LOGW(TAG, "skip corrupt MP3 frame");
      continue;
    }

    if (result < 0) {
      ret = ESP_FAIL;
      ESP_LOGE(TAG, "MP3 decode failed: %d", (int)result);
      break;
    }

    if (samples > 0) {
      ++decoded_frames;
      total_pcm_frames += samples;
      int channels = decoder.get_channels();
      size_t pcm_value_count = samples * (channels > 1 ? 2 : 1);
      int16_t peak = audio_pcm_peak(pcm_buf, pcm_value_count);
      if (peak == 0) {
        ++zero_peak_frames;
      }
      if (peak > max_peak) {
        max_peak = peak;
      }
      audio_apply_gain(pcm_buf, pcm_value_count);
      int16_t output_peak = audio_pcm_peak(pcm_buf, pcm_value_count);

      if (!stream_configured) {
        ret = audio_configure_i2s((uint32_t)decoder.get_sample_rate());
        if (ret != ESP_OK) {
          break;
        }
        stream_configured = true;
      }

      size_t bytes_written = 0;
      ret = audio_write_pcm(pcm_buf, samples, channels, &bytes_written);
      if (ret != ESP_OK) {
        ESP_LOGE(TAG, "write decoded PCM failed");
        break;
      }
      total_i2s_bytes += bytes_written;

      if (decoded_frames <= 5 || decoded_frames % 50 == 0) {
        audio_log_pcm_stats("pcm",
                            decoded_frames,
                            pcm_buf,
                            samples,
                            channels,
                            bytes_written,
                            input_len,
                            total_http_bytes,
                            peak,
                            output_peak);
      }
    }
  }

cleanup:
  if (client != nullptr) {
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
  }
  free(input_buf);
  free(pcm_buf);
  ESP_LOGI(TAG,
           "MP3 playback finished: ret=%s downloaded=%u decoded_frames=%u pcm_frames/ch=%u i2s_bytes=%u need_more=%u decode_errors=%u zero_peak_frames=%u max_peak=%d",
           esp_err_to_name(ret),
           (unsigned)total_http_bytes,
           (unsigned)decoded_frames,
           (unsigned)total_pcm_frames,
           (unsigned)total_i2s_bytes,
           (unsigned)need_more_count,
           (unsigned)decode_error_count,
           (unsigned)zero_peak_frames,
           max_peak);
  xSemaphoreGive(s_audio_mutex);
  return ret;
}
