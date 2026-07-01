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
static QueueHandle_t s_text_queue;
static TaskHandle_t s_pcm_play_task_handle;
static uint32_t s_current_sample_rate;
static bool s_i2s_enabled;
static volatile int s_volume_level = 6;
static volatile uint32_t s_playback_stop_generation;
static volatile bool s_pcm_playback_active;
static audio_pcm_playback_text_cb_t s_pcm_playback_text_cb;
static audio_pcm_playback_ref_cb_t s_pcm_playback_ref_cb;

enum {
  audio_volume_min_level = 0,
  audio_volume_max_level = 10,
  audio_volume_default_level = 6,
  audio_volume_percent_per_level = 20,
  audio_pcm_queue_depth = 16,
  audio_i2s_dma_desc_num = 4,
  audio_i2s_dma_frame_num = 128,
  audio_pcm_play_task_stack = 8192,
  audio_pcm_play_task_priority = 7,
  audio_playback_tail_silence_ms = 80,
  audio_i2s_idle_stop_delay_ms = 700,
  audio_start_prebuffer_ms = 350,
  audio_i2s_write_timeout_ms = 20,
  audio_interruptible_write_frames = 128,
  audio_mono_write_chunk_frames = 4096,
  audio_pcm_play_log_interval = 16,
  audio_text_queue_depth = 4,
  audio_text_task_stack = 4096,
  audio_text_task_priority = 4,
};

typedef struct {
  uint8_t *data;
  char *text;
  size_t bytes;
  uint32_t sample_rate_hz;
  int channels;
  uint32_t playback_generation;
  int64_t enqueue_us;
} audio_pcm_queue_item_t;

// 复制一份播放文本，便于跨任务异步传递。
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

// 文本回调任务：把播放中的 TTS 文本转发给 UI。
static void audio_text_task(void *arg)
{
  (void)arg;

  while (true) {
    char *text = nullptr;
    if (xQueueReceive(s_text_queue, &text, portMAX_DELAY) != pdTRUE) {
      continue;
    }

    audio_pcm_playback_text_cb_t cb = s_pcm_playback_text_cb;
    if (cb != nullptr) {
      cb(text != nullptr ? text : "");
    }
    free(text);
  }
}

// 将播放文本异步投递到文本队列，避免音频写入任务直接操作 UI。
static void audio_notify_playback_text_async(const char *text)
{
  // 没有回调或文本为空时无需通知。
  if (s_pcm_playback_text_cb == nullptr || text == nullptr || text[0] == '\0') {
    return;
  }

  char *text_copy = audio_strdup(text);
  if (text_copy == nullptr) {
    ESP_LOGW(TAG, "copy playback text for async UI update failed");
    return;
  }

  if (s_text_queue == nullptr ||
      xQueueSend(s_text_queue, &text_copy, 0) != pdTRUE) {
    ESP_LOGW(TAG, "playback text queue is full");
    free(text_copy);
  }
}

// 把正在播放的 PCM 作为参考信号送给回声消除/调试模块。
static void audio_notify_playback_ref(const int16_t *pcm,
                                      size_t frames,
                                      int channels,
                                      uint32_t sample_rate_hz)
{
  audio_pcm_playback_ref_cb_t cb = s_pcm_playback_ref_cb;
  // 回调参数不完整时直接跳过，避免下游处理无效音频。
  if (cb == nullptr || pcm == nullptr || frames == 0 || channels <= 0 ||
      sample_rate_hz == 0) {
    return;
  }

  cb(pcm, frames, channels, sample_rate_hz);
}

// 判断当前播放项是否已经被新的停止请求作废。
static bool audio_playback_stop_requested(uint32_t stop_generation)
{
  return stop_generation != s_playback_stop_generation;
}

// 停止播放时关闭 I2S，防止旧 DMA 数据继续发声。
static void audio_disable_i2s_for_stop(void)
{
  if (!s_i2s_enabled) {
    return;
  }

  esp_err_t err = i2s_channel_disable(s_i2s_tx_chan);
  if (err == ESP_OK) {
    s_i2s_enabled = false;
  } else {
    ESP_LOGW(TAG, "disable I2S for playback stop failed: %s", esp_err_to_name(err));
  }
}

// 释放队列项持有的 PCM 和文本内存。
static void audio_free_pcm_queue_item(audio_pcm_queue_item_t *item)
{
  if (item == nullptr) {
    return;
  }

  free(item->data);
  free(item->text);
  memset(item, 0, sizeof(*item));
}

// 将音量等级限制在有效范围内。
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

// 把 0-10 的音量等级换算成百分比增益。
static int audio_level_to_percent(int volume_level)
{
  return audio_clamp_volume_level(volume_level) * audio_volume_percent_per_level;
}

// 初始化音量配置使用的 NVS 分区。
static esp_err_t audio_init_nvs(void)
{
  esp_err_t err = nvs_flash_init();
  // NVS 分区旧版本或空间不足时，需要擦除后重新初始化。
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "erase NVS failed");
    err = nvs_flash_init();
  }
  return err;
}

// 从 NVS 读取保存的音量等级，失败时回落到默认音量。
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
    // 首次启动没有保存值，使用默认音量即可。
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

// 将音量等级写入 NVS，供下次启动恢复。
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

// 计算 int16_t 绝对值，特殊处理 INT16_MIN 溢出边界。
static int16_t audio_abs_i16(int16_t value)
{
  if (value == INT16_MIN) {
    return INT16_MAX;
  }
  return value < 0 ? -value : value;
}

// 计算 PCM 缓冲中的峰值，用于日志和削波观察。
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

// 按当前音量等级对 PCM 样本施加增益，并限制到 int16_t 范围。
static void audio_apply_gain(int16_t *pcm, size_t sample_count)
{
  int volume_percent = audio_level_to_percent(s_volume_level);
  // 100% 音量无需遍历修改，减少播放路径开销。
  if (volume_percent == 100) {
    return;
  }

  for (size_t i = 0; i < sample_count; ++i) {
    int32_t sample = (int32_t)pcm[i] * volume_percent / 100;
    if (sample > INT16_MAX) {
      // 放大后超过 int16_t 时进行饱和，避免整数回绕。
      sample = INT16_MAX;
    } else if (sample < INT16_MIN) {
      sample = INT16_MIN;
    }
    pcm[i] = (int16_t)sample;
  }
}

// 打印 PCM 播放统计，帮助定位解码、音量和 I2S 写入问题。
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

// 按目标采样率配置 I2S；采样率变化时先关闭再重配。
static esp_err_t audio_configure_i2s(uint32_t sample_rate_hz)
{
  if (sample_rate_hz == 0) {
    return ESP_ERR_INVALID_ARG;
  }

  if (s_current_sample_rate == sample_rate_hz && s_i2s_enabled) {
    // 当前配置已经匹配，避免不必要的 I2S 重启。
    return ESP_OK;
  }

  if (s_i2s_enabled) {
    // ESP-IDF 要求通道关闭后才能修改时钟和 slot 配置。
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

// 将 PCM 写入 I2S；单声道会扩展成左右声道相同的立体声。
static esp_err_t audio_write_pcm(const int16_t *pcm,
                                 size_t samples_per_channel,
                                 int channels,
                                 size_t *bytes_written_total,
                                 uint32_t stop_generation)
{
  if (pcm == nullptr || samples_per_channel == 0 || channels <= 0) {
    if (bytes_written_total != nullptr) {
      *bytes_written_total = 0;
    }
    return ESP_OK;
  }

  size_t total_written = 0;
  if (channels >= 2) {
    // 立体声数据已交织，直接分块写入 I2S。
    size_t offset = 0;
    while (offset < samples_per_channel) {
      if (audio_playback_stop_requested(stop_generation)) {
        if (bytes_written_total != nullptr) {
          *bytes_written_total = total_written;
        }
        return ESP_ERR_INVALID_STATE;
      }

      size_t chunk = samples_per_channel - offset;
      if (chunk > audio_interruptible_write_frames) {
        // 小块写入可以让停止请求更快生效。
        chunk = audio_interruptible_write_frames;
      }

      size_t bytes_written = 0;
      size_t expected_bytes = chunk * 2 * sizeof(int16_t);
      esp_err_t err = i2s_channel_write(s_i2s_tx_chan,
                                        pcm + offset * 2,
                                        expected_bytes,
                                        &bytes_written,
                                        pdMS_TO_TICKS(audio_i2s_write_timeout_ms));
      if (audio_playback_stop_requested(stop_generation)) {
        if (bytes_written_total != nullptr) {
          *bytes_written_total = total_written + bytes_written;
        }
        return ESP_ERR_INVALID_STATE;
      }
      size_t written_frames = bytes_written / (2 * sizeof(int16_t));
      if (written_frames > 0) {
        audio_notify_playback_ref(pcm + offset * 2,
                                  written_frames,
                                  2,
                                  s_current_sample_rate);
        total_written += bytes_written;
        offset += written_frames;
      }
      if (err != ESP_OK) {
        if (err == ESP_ERR_TIMEOUT) {
          // I2S 短暂繁忙时继续尝试，避免轻微阻塞中断播放。
          continue;
        }
        if (bytes_written_total != nullptr) {
          *bytes_written_total = total_written;
        }
        return err;
      }
      if (bytes_written != expected_bytes) {
        ESP_LOGW(TAG, "partial I2S stereo write: %u/%u bytes", (unsigned)bytes_written, (unsigned)expected_bytes);
      }
      if (written_frames == 0) {
        offset += chunk;
      }
    }

    if (bytes_written_total != nullptr) {
      *bytes_written_total = total_written;
    }
    return ESP_OK;
  }

  int16_t *stereo = (int16_t *)malloc(audio_mono_write_chunk_frames * 2 * sizeof(int16_t));
  ESP_RETURN_ON_FALSE(stereo != nullptr, ESP_ERR_NO_MEM, TAG, "allocate mono conversion buffer failed");

  size_t offset = 0;
  while (offset < samples_per_channel) {
    if (audio_playback_stop_requested(stop_generation)) {
      if (bytes_written_total != nullptr) {
        *bytes_written_total = total_written;
      }
      free(stereo);
      return ESP_ERR_INVALID_STATE;
    }

    size_t chunk = samples_per_channel - offset;
    if (chunk > audio_interruptible_write_frames) {
      chunk = audio_interruptible_write_frames;
    }

    for (size_t i = 0; i < chunk; ++i) {
      // 单声道样本复制到左右声道，适配 MAX98357A 的立体声 slot 配置。
      stereo[i * 2] = pcm[offset + i];
      stereo[i * 2 + 1] = pcm[offset + i];
    }

    size_t bytes_written = 0;
    size_t expected_bytes = chunk * 2 * sizeof(int16_t);
    esp_err_t err = i2s_channel_write(s_i2s_tx_chan,
                                      stereo,
                                      expected_bytes,
                                      &bytes_written,
                                      pdMS_TO_TICKS(audio_i2s_write_timeout_ms));
    if (audio_playback_stop_requested(stop_generation)) {
      if (bytes_written_total != nullptr) {
        *bytes_written_total = total_written + bytes_written;
      }
      free(stereo);
      return ESP_ERR_INVALID_STATE;
    }
    size_t written_frames = bytes_written / (2 * sizeof(int16_t));
    if (written_frames > 0) {
      audio_notify_playback_ref(pcm + offset,
                                written_frames,
                                1,
                                s_current_sample_rate);
      total_written += bytes_written;
      offset += written_frames;
    }
    if (err != ESP_OK) {
      if (err == ESP_ERR_TIMEOUT) {
        // 写超时通常表示 DMA 还没腾出空间，继续下一轮尝试。
        continue;
      }
      free(stereo);
      ESP_LOGE(TAG, "write mono PCM failed: %s", esp_err_to_name(err));
      return err;
    }
    if (bytes_written != expected_bytes) {
      ESP_LOGW(TAG, "partial I2S mono write: %u/%u bytes", (unsigned)bytes_written, (unsigned)expected_bytes);
    }
    if (written_frames == 0) {
      offset += chunk;
    }
  }

  if (bytes_written_total != nullptr) {
    *bytes_written_total = total_written;
  }
  free(stereo);
  return ESP_OK;
}

// 在播放尾部写入短静音，帮助功放和 DMA 平滑收尾。
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
    ESP_RETURN_ON_ERROR(audio_write_pcm(silence, chunk, 2, &bytes_written, s_playback_stop_generation),
                        TAG,
                        "write playback tail silence failed");
    silence_frames -= chunk;
  }

  return ESP_OK;
}

// 队列空闲一段时间后关闭 I2S，减少空闲噪声和资源占用。
static esp_err_t audio_stop_i2s_when_idle(void)
{
  if (!s_i2s_enabled || s_pcm_queue == nullptr || uxQueueMessagesWaiting(s_pcm_queue) > 0) {
    // 仍有播放任务或 I2S 未开启时不做任何处理。
    return ESP_OK;
  }

  ESP_RETURN_ON_ERROR(audio_write_tail_silence(s_current_sample_rate),
                      TAG,
                      "flush playback tail failed");
  ESP_RETURN_ON_ERROR(i2s_channel_disable(s_i2s_tx_chan), TAG, "disable idle I2S tx failed");
  s_i2s_enabled = false;
  return ESP_OK;
}

// PCM 播放任务：消费队列、配置 I2S、应用音量并写入样本。
static void audio_pcm_play_task(void *arg)
{
  (void)arg;
  bool playback_idle = true;
  uint32_t played_items = 0;

  while (true) {
    audio_pcm_queue_item_t item = {};
    if (xQueueReceive(s_pcm_queue, &item, pdMS_TO_TICKS(audio_i2s_idle_stop_delay_ms)) != pdTRUE) {
      // 长时间没有新 PCM 时，写尾部静音并关闭 I2S。
      if (xSemaphoreTake(s_audio_mutex, portMAX_DELAY) == pdTRUE) {
        esp_err_t idle_ret = audio_stop_i2s_when_idle();
        if (idle_ret != ESP_OK) {
          ESP_LOGW(TAG, "stop idle I2S failed: %s", esp_err_to_name(idle_ret));
        }
        xSemaphoreGive(s_audio_mutex);
      }
      playback_idle = true;
      played_items = 0;
      s_pcm_playback_active = false;
      continue;
    }

    if (item.data == nullptr || item.bytes == 0) {
      // 防御无效队列项，确保异常数据不会进入播放链路。
      free(item.data);
      free(item.text);
      continue;
    }

    uint32_t stop_generation = item.playback_generation;
    if (audio_playback_stop_requested(stop_generation)) {
      // 入队后发生过停止请求，丢弃这段旧音频。
      audio_free_pcm_queue_item(&item);
      continue;
    }

    if (playback_idle) {
      // 首包稍作预缓冲，减少 TTS 分片播放时的断续感。
      vTaskDelay(pdMS_TO_TICKS(audio_start_prebuffer_ms));
    }

    if (xSemaphoreTake(s_audio_mutex, portMAX_DELAY) != pdTRUE) {
      audio_free_pcm_queue_item(&item);
      continue;
    }

    if (audio_playback_stop_requested(stop_generation)) {
      xSemaphoreGive(s_audio_mutex);
      audio_free_pcm_queue_item(&item);
      continue;
    }

    bool playback_stopped = false;
    esp_err_t ret = audio_configure_i2s(item.sample_rate_hz);
    if (ret == ESP_OK) {
      audio_notify_playback_text_async(item.text);

      // 播放前计算峰值，应用音量后再记录输出峰值。
      size_t sample_count = item.bytes / sizeof(int16_t);
      int16_t *pcm = (int16_t *)item.data;
      int16_t peak_before_gain = audio_pcm_peak(pcm, sample_count);
      audio_apply_gain(pcm, sample_count);
      int16_t peak_after_gain = audio_pcm_peak(pcm, sample_count);

      size_t samples_per_channel = sample_count / (size_t)item.channels;
      size_t bytes_written = 0;
      int64_t write_start_us = esp_timer_get_time();
      ret = audio_write_pcm(pcm, samples_per_channel, item.channels, &bytes_written, stop_generation);
      int64_t write_us = esp_timer_get_time() - write_start_us;
      if (ret == ESP_ERR_INVALID_STATE && audio_playback_stop_requested(stop_generation)) {
        // 停止请求通过 generation 命中，立即关闭 I2S 并丢弃剩余数据。
        audio_disable_i2s_for_stop();
        playback_stopped = true;
        ESP_LOGI(TAG, "queued PCM playback stopped");
      } else if (ret != ESP_OK) {
        ESP_LOGE(TAG, "write queued PCM failed: %s", esp_err_to_name(ret));
      } else if (bytes_written > 0) {
        uint32_t audio_ms = (uint32_t)(samples_per_channel * 1000U / item.sample_rate_hz);
        int64_t queue_wait_us = write_start_us - item.enqueue_us;
        uint32_t queued_items = uxQueueMessagesWaiting(s_pcm_queue);
        ++played_items;
        if (queued_items == 0) {
          s_pcm_playback_active = false;
        }
        if (played_items <= 2 ||
            queued_items == 0 ||
            (played_items % audio_pcm_play_log_interval) == 0U) {
          // 只在开头、队列尾部和固定间隔打印，避免日志淹没播放任务。
          ESP_LOGI(TAG,
                   "queued PCM played: item=%lu bytes=%u, %lu Hz, channels=%d, audio_ms=%u queue_wait=%lld ms write=%lld ms q_left=%u volume=%d/10 peak=%d peak_out=%d i2s=%u",
                   (unsigned long)played_items,
                   (unsigned)item.bytes,
                   (unsigned long)item.sample_rate_hz,
                   item.channels,
                   (unsigned)audio_ms,
                   (long long)(queue_wait_us / 1000),
                   (long long)(write_us / 1000),
                   (unsigned)queued_items,
                   audio_get_volume_level(),
                   peak_before_gain,
                   peak_after_gain,
                   (unsigned)bytes_written);
        }
      }
    } else {
      ESP_LOGE(TAG,
               "configure queued PCM failed: %s, sample_rate=%lu",
               esp_err_to_name(ret),
               (unsigned long)item.sample_rate_hz);
    }

    xSemaphoreGive(s_audio_mutex);
    playback_idle = playback_stopped;
    audio_free_pcm_queue_item(&item);
  }
}

// 初始化音频输出通道、播放队列和后台任务。
extern "C" esp_err_t audio_init(void)
{
  // I2S 通道已创建说明音频模块已经初始化。
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

  s_text_queue = xQueueCreate(audio_text_queue_depth, sizeof(char *));
  ESP_RETURN_ON_FALSE(s_text_queue != nullptr, ESP_ERR_NO_MEM, TAG, "create playback text queue failed");

  BaseType_t text_task_ret = xTaskCreate(audio_text_task,
                                         "audio_text",
                                         audio_text_task_stack,
                                         nullptr,
                                         audio_text_task_priority,
                                         nullptr);
  ESP_RETURN_ON_FALSE(text_task_ret == pdPASS,
                      ESP_ERR_NO_MEM,
                      TAG,
                      "create playback text task failed");

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

// 停止当前播放并清空所有已排队 PCM。
extern "C" void audio_stop_playback(void)
{
  // 增加 generation，让正在播放或排队的旧音频自动失效。
  ++s_playback_stop_generation;
  s_pcm_playback_active = false;

  if (s_pcm_queue != nullptr) {
    audio_pcm_queue_item_t item = {};
    uint32_t cleared = 0;
    while (xQueueReceive(s_pcm_queue, &item, 0) == pdTRUE) {
      // 队列里的数据已经不会播放，立即释放内存。
      audio_free_pcm_queue_item(&item);
      ++cleared;
    }
    if (cleared > 0) {
      ESP_LOGI(TAG, "cleared queued PCM: %lu item(s)", (unsigned long)cleared);
    }
  }

  if (s_audio_mutex != nullptr && xSemaphoreTake(s_audio_mutex, 0) == pdTRUE) {
    audio_disable_i2s_for_stop();
    xSemaphoreGive(s_audio_mutex);
  }
}

// 队列播放不带文本的 PCM 数据。
extern "C" esp_err_t audio_queue_pcm_s16le(const void *pcm,
                                           size_t bytes,
                                           uint32_t sample_rate_hz,
                                           int channels)
{
  return audio_queue_pcm_s16le_with_text(pcm, bytes, sample_rate_hz, channels, nullptr);
}

// 队列播放 PCM 数据，并可携带一段文本用于播放时刷新 UI。
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
    // 音频和文本需要一起入队；文本复制失败时放弃这次入队。
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
    .playback_generation = s_playback_stop_generation,
    .enqueue_us = esp_timer_get_time(),
  };

  if (xQueueSend(s_pcm_queue, &item, portMAX_DELAY) != pdTRUE) {
    // 入队失败时释放本函数申请的所有资源，避免泄漏。
    free(copy);
    free(text_copy);
    ESP_LOGW(TAG, "PCM playback queue is full");
    return ESP_ERR_TIMEOUT;
  }

  s_pcm_playback_active = true;
  return ESP_OK;
}

// 查询播放任务是否仍有活跃或排队音频。
extern "C" bool audio_is_playback_active(void)
{
  if (s_pcm_playback_active) {
    return true;
  }

  return s_pcm_queue != nullptr && uxQueueMessagesWaiting(s_pcm_queue) > 0;
}

// 设置 TTS 文本播放回调。
extern "C" void audio_set_pcm_playback_text_cb(audio_pcm_playback_text_cb_t cb)
{
  s_pcm_playback_text_cb = cb;
}

// 设置播放参考 PCM 回调。
extern "C" void audio_set_pcm_playback_ref_cb(audio_pcm_playback_ref_cb_t cb)
{
  s_pcm_playback_ref_cb = cb;
}

// 获取当前音量等级。
extern "C" int audio_get_volume_level(void)
{
  return s_volume_level;
}

// 音量加一档并保存。
extern "C" int audio_volume_up(void)
{
  s_volume_level = audio_clamp_volume_level(s_volume_level + 1);
  audio_save_volume_level(s_volume_level);
  ESP_LOGI(TAG, "volume up: %d/10", s_volume_level);
  return s_volume_level;
}

// 音量减一档并保存。
extern "C" int audio_volume_down(void)
{
  s_volume_level = audio_clamp_volume_level(s_volume_level - 1);
  audio_save_volume_level(s_volume_level);
  ESP_LOGI(TAG, "volume down: %d/10", s_volume_level);
  return s_volume_level;
}

// 播放短测试音，用于验证 I2S 和功放工作状态。
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
  uint32_t stop_generation = s_playback_stop_generation;
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
      // 固定小块写入，便于测试音也能响应停止请求。
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
    esp_err_t write_ret = audio_write_pcm(samples,
                                          chunk_frames,
                                          2,
                                          &bytes_written,
                                          stop_generation);
    if (write_ret == ESP_ERR_INVALID_STATE && audio_playback_stop_requested(stop_generation)) {
      // 测试音播放中被打断时，立即关闭 I2S。
      audio_disable_i2s_for_stop();
      ESP_LOGI(TAG, "test tone stopped");
      return write_ret;
    }
    ESP_RETURN_ON_ERROR(write_ret, TAG, "write test tone failed");
    written_frames += chunk_frames;
  }

  return ESP_OK;
}

// 从 URL 拉取 MP3，边下载边解码播放。
extern "C" esp_err_t audio_play_mp3_url(const char *url)
{
  ESP_RETURN_ON_ERROR(audio_init(), TAG, "audio init failed");
  ESP_RETURN_ON_FALSE(url != nullptr && url[0] != '\0', ESP_ERR_INVALID_ARG, TAG, "empty URL");

  if (xSemaphoreTake(s_audio_mutex, 0) != pdTRUE) {
    // MP3 直连播放独占音频通道，避免和队列播放同时写 I2S。
    ESP_LOGW(TAG, "audio playback is already running");
    return ESP_ERR_INVALID_STATE;
  }

  uint32_t stop_generation = s_playback_stop_generation;
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
      // 输入缓冲低于一半时继续从 HTTP 补数据。
      int read_len = esp_http_client_read(client,
                                         (char *)input_buf + input_len,
                                         AUDIO_MP3_INPUT_BUFFER_BYTES - input_len);
      if (read_len < 0) {
        ret = ESP_FAIL;
        ESP_LOGE(TAG, "read MP3 stream failed");
        break;
      }
      if (read_len == 0) {
        // HTTP 读到 0 表示远端流结束，剩余缓冲仍需继续解码。
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
      // 解码器消费掉的字节从输入缓冲头部移除。
      memmove(input_buf, input_buf + consumed, input_len - consumed);
      input_len -= consumed;
    }

    if (result == micro_mp3::MP3_STREAM_INFO_READY && !stream_configured) {
      // 首次拿到 MP3 流参数后，按实际采样率配置 I2S。
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
        // 文件已经结束仍缺数据，说明尾部帧不完整，结束播放。
        ESP_LOGW(TAG,
                 "decoder needs more data after HTTP EOF: buffered=%u consumed=%u",
                 (unsigned)input_len,
                 (unsigned)consumed);
        break;
      }
      continue;
    }

    if (result == micro_mp3::MP3_DECODE_ERROR) {
      // 单帧损坏不影响后续帧，跳过继续解码。
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
        // 某些流可能先产出 PCM，再显式报告流信息，这里兜底配置。
        ret = audio_configure_i2s((uint32_t)decoder.get_sample_rate());
        if (ret != ESP_OK) {
          break;
        }
        stream_configured = true;
      }

      size_t bytes_written = 0;
      ret = audio_write_pcm(pcm_buf, samples, channels, &bytes_written, stop_generation);
      if (ret == ESP_ERR_INVALID_STATE && audio_playback_stop_requested(stop_generation)) {
        // 播放过程中收到停止请求，退出解码循环。
        audio_disable_i2s_for_stop();
        ESP_LOGI(TAG, "MP3 playback stopped");
        break;
      }
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
