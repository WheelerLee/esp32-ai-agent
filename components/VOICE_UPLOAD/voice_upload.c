#include "voice_upload.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_wifi.h"
#include "audio.h"
#include "cJSON.h"
#include "driver/i2s_std.h"
#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_mn_iface.h"
#include "esp_mn_models.h"
#include "esp_mn_speech_commands.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "key.h"
#include "lcd.h"
#include "model_path.h"
#include "sdkconfig.h"

static const char *TAG = "voice_upload";

#define VOICE_UPLOAD_TASK_STACK 12288
#define VOICE_UPLOAD_TASK_PRIORITY 5
#define VOICE_UPLOAD_WS_MONITOR_TASK_STACK 4096
#define VOICE_UPLOAD_HEARTBEAT_MS 30000
#define VOICE_UPLOAD_CONNECT_TIMEOUT_MS 10000
#define VOICE_UPLOAD_STARTUP_WS_TIMEOUT_MS 30000
#define VOICE_UPLOAD_SEND_TIMEOUT_TICKS pdMS_TO_TICKS(3000)
#define VOICE_UPLOAD_RECONNECT_DELAY_MS 1000
#define VOICE_UPLOAD_FRAMES_PER_CHUNK 512
#define VOICE_UPLOAD_MIC_SHIFT 14
#define VOICE_UPLOAD_WS_RX_BUFFER_BYTES 4096
#define VOICE_UPLOAD_TTS_TEXT_SLOTS 4
#define VOICE_UPLOAD_IDLE_READ_TIMEOUT_MS 30
#define VOICE_UPLOAD_VAD_AVG_THRESHOLD 220
#define VOICE_UPLOAD_VAD_PEAK_THRESHOLD 900
#define VOICE_UPLOAD_VAD_START_CHUNKS 3
#define VOICE_UPLOAD_VAD_END_SILENCE_CHUNKS 25
#define VOICE_UPLOAD_VAD_FIRST_SPEECH_CHUNKS 157
#define VOICE_UPLOAD_VAD_MAX_RECORD_CHUNKS 938
#define VOICE_UPLOAD_SPEAKING_TASK_STACK 2048
#define VOICE_UPLOAD_RESPONSE_IDLE_TIMEOUT_MS 15000
#define VOICE_UPLOAD_VAD_DEBUG_ONLY 0
#define VOICE_UPLOAD_VAD_PEAK_AVG_RATIO_MAX 5
#define VOICE_UPLOAD_AFE_DEBUG_ENABLE 1
#define VOICE_UPLOAD_AFE_LOG_INTERVAL 100
#define VOICE_UPLOAD_AFE_REF_BUFFER_SAMPLES 2048
#define VOICE_UPLOAD_IDLE_VAD_LOG_INTERVAL 100
#define VOICE_UPLOAD_RECORD_VAD_LOG_INTERVAL 20
#define VOICE_UPLOAD_VAD_DEBUG_COOLDOWN_CHUNKS 100
#define VOICE_UPLOAD_HUMAN_VOICE_LOG_COOLDOWN_CHUNKS 200
#define VOICE_UPLOAD_TTS_CHUNK_LOG_INTERVAL 16
#define VOICE_UPLOAD_MULTINET_MODEL_NAME "mn7_cn"
#define VOICE_UPLOAD_MULTINET_WAKE_PHRASE "xi xi tong xue"
#define VOICE_UPLOAD_MULTINET_WAKE_DISPLAY "溪溪同学"
#define VOICE_UPLOAD_MULTINET_WAKE_COMMAND_ID 1
#define VOICE_UPLOAD_MULTINET_TIMEOUT_MS 3000
#define VOICE_UPLOAD_MULTINET_RESET_SILENCE_CHUNKS 10
#define VOICE_UPLOAD_PROMPT_NET_ERROR "/font/net_error.wav"
#define VOICE_UPLOAD_PROMPT_NET_ERROR_AND_TRY "/font/net_error_and_try.wav"
#define VOICE_UPLOAD_PROMPT_IM_READY "/font/im_ready.wav"

static i2s_chan_handle_t s_i2s_rx_chan;
static esp_websocket_client_handle_t s_ws_client;
static TaskHandle_t s_upload_task_handle;
static TaskHandle_t s_idle_vad_task_handle;
static TaskHandle_t s_ws_monitor_task_handle;
static SemaphoreHandle_t s_ws_mutex;
static volatile bool s_ws_connected;
static volatile bool s_accept_tts = true;
static volatile bool s_idle_vad_paused;
static volatile bool s_continue_after_tts;
static bool s_ws_started;
static bool s_initialized;
#if VOICE_UPLOAD_AFE_DEBUG_ENABLE
static const esp_afe_sr_iface_t *s_afe_handle;
static esp_afe_sr_data_t *s_afe_data;
static int16_t *s_afe_feed_buffer;
static int s_afe_feed_chunksize;
static int s_afe_feed_channels;
static size_t s_afe_pending_samples;
static uint32_t s_afe_feed_count;
static uint32_t s_afe_fetch_count;
static int16_t *s_afe_ref_ring;
static size_t s_afe_ref_ring_size;
static size_t s_afe_ref_read_pos;
static size_t s_afe_ref_write_pos;
static size_t s_afe_ref_count;
static uint32_t s_afe_ref_sample_rate;
static uint32_t s_afe_ref_resample_accum;
static vad_state_t s_afe_last_vad_state = VAD_SILENCE;
static vad_state_t s_afe_logged_vad_state = VAD_SILENCE;
static portMUX_TYPE s_afe_ref_mux = portMUX_INITIALIZER_UNLOCKED;
#endif

typedef enum {
  VOICE_STATE_IDLE = 0,
  VOICE_STATE_VAD_ACTIVE,
  VOICE_STATE_RECORDING,
  VOICE_STATE_WAITING_RESPONSE,
  VOICE_STATE_SPEAKING,
} voice_state_t;

typedef enum {
  RECORD_STOP_BY_KEY = 0,
  RECORD_STOP_BY_SILENCE,
} record_stop_mode_t;

typedef struct {
  bool active;
  uint8_t *data;
  size_t expected_bytes;
  size_t received_bytes;
  uint32_t sample_rate_hz;
  int channels;
  int response_id;
  int speech_index;
  int chunk_index;
  int64_t meta_us;
  char *text;
} tts_audio_rx_t;

typedef struct {
  bool active;
  int response_id;
  int speech_index;
  char *text;
} tts_text_slot_t;

typedef struct {
  bool active;
  char *data;
  size_t expected_bytes;
  size_t received_bytes;
} ws_text_rx_t;

static tts_audio_rx_t s_tts_audio_rx;
static tts_text_slot_t s_tts_text_slots[VOICE_UPLOAD_TTS_TEXT_SLOTS];
static ws_text_rx_t s_ws_text_rx;
static volatile voice_state_t s_voice_state = VOICE_STATE_IDLE;
static volatile uint32_t s_state_generation;
static const esp_mn_iface_t *s_mn_handle;
static model_iface_data_t *s_mn_data;
static int16_t *s_mn_feed_buffer;
static int s_mn_feed_chunksize;
static int s_mn_sample_rate;
static size_t s_mn_pending_samples;
static bool s_mn_ready;
static bool s_mn_init_attempted;
static srmodel_list_t *s_srmodels;

static esp_err_t voice_srmodel_init(void);

static void voice_set_state(voice_state_t state)
{
  if (s_voice_state == state) {
    return;
  }

  s_voice_state = state;
  ++s_state_generation;
}

static void waiting_response_timeout_task(void *arg)
{
  uint32_t generation = (uint32_t)(uintptr_t)arg;

  vTaskDelay(pdMS_TO_TICKS(VOICE_UPLOAD_RESPONSE_IDLE_TIMEOUT_MS));
  if (generation == s_state_generation && s_voice_state == VOICE_STATE_WAITING_RESPONSE) {
    ESP_LOGW(TAG, "response timeout, return to idle");
    voice_set_state(VOICE_STATE_IDLE);
  }

  vTaskDelete(NULL);
}

static void schedule_waiting_response_timeout(void)
{
  uint32_t generation = s_state_generation;
  BaseType_t ret = xTaskCreate(waiting_response_timeout_task,
                               "voice_resp_timeout",
                               VOICE_UPLOAD_SPEAKING_TASK_STACK,
                               (void *)(uintptr_t)generation,
                               VOICE_UPLOAD_TASK_PRIORITY - 1,
                               NULL);
  if (ret != pdPASS) {
    ESP_LOGW(TAG, "create response timeout task failed");
  }
}

static char *voice_upload_strdup(const char *text)
{
  if (text == NULL || text[0] == '\0') {
    return NULL;
  }

  size_t len = strlen(text);
  char *copy = (char *)malloc(len + 1);
  if (copy != NULL) {
    memcpy(copy, text, len + 1);
  }
  return copy;
}

static uint16_t voice_read_le16(const uint8_t *data)
{
  return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static uint32_t voice_read_le32(const uint8_t *data)
{
  return (uint32_t)data[0] |
         ((uint32_t)data[1] << 8) |
         ((uint32_t)data[2] << 16) |
         ((uint32_t)data[3] << 24);
}

static esp_err_t voice_skip_file_bytes(FILE *file, uint32_t bytes)
{
  if (bytes == 0) {
    return ESP_OK;
  }
  return fseek(file, (long)bytes, SEEK_CUR) == 0 ? ESP_OK : ESP_FAIL;
}

static esp_err_t voice_play_wav_prompt(const char *path, bool wait_done)
{
  FILE *file = fopen(path, "rb");
  ESP_RETURN_ON_FALSE(file != NULL, ESP_ERR_NOT_FOUND, TAG, "open prompt failed: %s", path);

  esp_err_t ret = ESP_OK;
  uint8_t header[12] = {0};
  uint16_t audio_format = 0;
  uint16_t channels = 0;
  uint32_t sample_rate_hz = 0;
  uint16_t bits_per_sample = 0;
  uint32_t data_bytes = 0;
  uint8_t *pcm = NULL;

  if (fread(header, 1, sizeof(header), file) != sizeof(header) ||
      memcmp(header, "RIFF", 4) != 0 ||
      memcmp(header + 8, "WAVE", 4) != 0) {
    ret = ESP_ERR_INVALID_ARG;
    goto cleanup;
  }

  while (true) {
    uint8_t chunk_header[8] = {0};
    if (fread(chunk_header, 1, sizeof(chunk_header), file) != sizeof(chunk_header)) {
      ret = ESP_ERR_INVALID_SIZE;
      goto cleanup;
    }

    uint32_t chunk_size = voice_read_le32(chunk_header + 4);
    if (memcmp(chunk_header, "fmt ", 4) == 0) {
      uint8_t fmt[16] = {0};
      if (chunk_size < sizeof(fmt) || fread(fmt, 1, sizeof(fmt), file) != sizeof(fmt)) {
        ret = ESP_ERR_INVALID_SIZE;
        goto cleanup;
      }
      audio_format = voice_read_le16(fmt);
      channels = voice_read_le16(fmt + 2);
      sample_rate_hz = voice_read_le32(fmt + 4);
      bits_per_sample = voice_read_le16(fmt + 14);
      ret = voice_skip_file_bytes(file, chunk_size - sizeof(fmt));
      if (ret != ESP_OK) {
        goto cleanup;
      }
    } else if (memcmp(chunk_header, "data", 4) == 0) {
      data_bytes = chunk_size;
      break;
    } else {
      ret = voice_skip_file_bytes(file, chunk_size);
      if (ret != ESP_OK) {
        goto cleanup;
      }
    }

    if ((chunk_size & 1U) != 0U) {
      ret = voice_skip_file_bytes(file, 1);
      if (ret != ESP_OK) {
        goto cleanup;
      }
    }
  }

  if (audio_format != 1 ||
      (channels != 1 && channels != 2) ||
      sample_rate_hz == 0 ||
      bits_per_sample != 16 ||
      data_bytes == 0 ||
      (data_bytes % (sizeof(int16_t) * channels)) != 0) {
    ESP_LOGW(TAG,
             "unsupported prompt WAV: path=%s format=%u channels=%u rate=%lu bits=%u bytes=%lu",
             path,
             audio_format,
             channels,
             (unsigned long)sample_rate_hz,
             bits_per_sample,
             (unsigned long)data_bytes);
    ret = ESP_ERR_NOT_SUPPORTED;
    goto cleanup;
  }

  pcm = (uint8_t *)malloc(data_bytes);
  if (pcm == NULL) {
    ret = ESP_ERR_NO_MEM;
    goto cleanup;
  }
  if (fread(pcm, 1, data_bytes, file) != data_bytes) {
    ret = ESP_ERR_INVALID_SIZE;
    goto cleanup;
  }

  ret = audio_queue_pcm_s16le(pcm, data_bytes, sample_rate_hz, channels);
  if (ret != ESP_OK) {
    goto cleanup;
  }

  ESP_LOGI(TAG,
           "prompt queued: %s bytes=%lu rate=%lu channels=%u wait=%d",
           path,
           (unsigned long)data_bytes,
           (unsigned long)sample_rate_hz,
           channels,
           wait_done);

  if (wait_done) {
    size_t frames = data_bytes / (sizeof(int16_t) * channels);
    int64_t deadline = esp_timer_get_time() +
                       ((int64_t)frames * 1000000 / sample_rate_hz) +
                       2000000;
    while (audio_is_playback_active() && esp_timer_get_time() < deadline) {
      vTaskDelay(pdMS_TO_TICKS(20));
    }
  }

cleanup:
  free(pcm);
  fclose(file);
  if (ret != ESP_OK) {
    ESP_LOGW(TAG, "play prompt failed: %s %s", path, esp_err_to_name(ret));
  }
  return ret;
}

static void voice_multinet_reset(void)
{
  if (s_mn_handle != NULL && s_mn_data != NULL && s_mn_handle->clean != NULL) {
    s_mn_handle->clean(s_mn_data);
  }
  s_mn_pending_samples = 0;
}

static esp_err_t voice_multinet_init(void)
{
  if (s_mn_ready) {
    return ESP_OK;
  }

  ESP_RETURN_ON_ERROR(voice_srmodel_init(), TAG, "SR model init failed");

  s_mn_handle = esp_mn_handle_from_name((char *)VOICE_UPLOAD_MULTINET_MODEL_NAME);
  ESP_RETURN_ON_FALSE(s_mn_handle != NULL,
                      ESP_ERR_NOT_FOUND,
                      TAG,
                      "MultiNet model unavailable: %s",
                      VOICE_UPLOAD_MULTINET_MODEL_NAME);

  s_mn_data = s_mn_handle->create(VOICE_UPLOAD_MULTINET_MODEL_NAME,
                                  VOICE_UPLOAD_MULTINET_TIMEOUT_MS);
  ESP_RETURN_ON_FALSE(s_mn_data != NULL,
                      ESP_ERR_NO_MEM,
                      TAG,
                      "create MultiNet model failed");

  s_mn_feed_chunksize = s_mn_handle->get_samp_chunksize(s_mn_data);
  s_mn_sample_rate = s_mn_handle->get_samp_rate(s_mn_data);
  ESP_RETURN_ON_FALSE(s_mn_feed_chunksize > 0 && s_mn_sample_rate > 0,
                      ESP_ERR_INVALID_STATE,
                      TAG,
                      "invalid MultiNet params: chunk=%d sample_rate=%d",
                      s_mn_feed_chunksize,
                      s_mn_sample_rate);
  ESP_RETURN_ON_FALSE(s_mn_sample_rate == VOICE_UPLOAD_SAMPLE_RATE_HZ,
                      ESP_ERR_INVALID_STATE,
                      TAG,
                      "MultiNet sample rate mismatch: model=%d mic=%d",
                      s_mn_sample_rate,
                      VOICE_UPLOAD_SAMPLE_RATE_HZ);

  s_mn_feed_buffer = (int16_t *)malloc((size_t)s_mn_feed_chunksize * sizeof(int16_t));
  ESP_RETURN_ON_FALSE(s_mn_feed_buffer != NULL,
                      ESP_ERR_NO_MEM,
                      TAG,
                      "allocate MultiNet buffer failed");

  esp_err_t ret = esp_mn_commands_alloc(s_mn_handle, s_mn_data);
  if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
    ESP_LOGW(TAG, "allocate MultiNet commands failed: %s", esp_err_to_name(ret));
    return ret;
  }

  ret = esp_mn_commands_clear();
  if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
    ESP_LOGW(TAG, "clear MultiNet commands failed: %s", esp_err_to_name(ret));
  }

  ret = esp_mn_commands_add(VOICE_UPLOAD_MULTINET_WAKE_COMMAND_ID,
                            VOICE_UPLOAD_MULTINET_WAKE_PHRASE);
  ESP_RETURN_ON_ERROR(ret, TAG, "add MultiNet wake phrase failed");

  esp_mn_error_t *cmd_err = esp_mn_commands_update();
  ESP_RETURN_ON_FALSE(cmd_err == NULL,
                      ESP_ERR_INVALID_STATE,
                      TAG,
                      "MultiNet wake phrase cannot be parsed: command=%s display=%s",
                      VOICE_UPLOAD_MULTINET_WAKE_PHRASE,
                      VOICE_UPLOAD_MULTINET_WAKE_DISPLAY);

  s_mn_ready = true;
  ESP_LOGI(TAG,
           "MultiNet ready: model=%s command=%s display=%s command_id=%d chunk=%d sample_rate=%d timeout_ms=%d",
           VOICE_UPLOAD_MULTINET_MODEL_NAME,
           VOICE_UPLOAD_MULTINET_WAKE_PHRASE,
           VOICE_UPLOAD_MULTINET_WAKE_DISPLAY,
           VOICE_UPLOAD_MULTINET_WAKE_COMMAND_ID,
           s_mn_feed_chunksize,
           s_mn_sample_rate,
           VOICE_UPLOAD_MULTINET_TIMEOUT_MS);
  return ESP_OK;
}

static bool voice_multinet_feed(const int16_t *pcm, size_t samples)
{
  if (!s_mn_ready && !s_mn_init_attempted) {
    s_mn_init_attempted = true;
    esp_err_t ret = voice_multinet_init();
    if (ret != ESP_OK) {
      ESP_LOGW(TAG, "MultiNet wake disabled: %s", esp_err_to_name(ret));
      s_mn_pending_samples = 0;
    }
  }

  if (!s_mn_ready || s_mn_handle == NULL || s_mn_data == NULL || s_mn_feed_buffer == NULL ||
      pcm == NULL || samples == 0) {
    return false;
  }

  size_t offset = 0;
  while (offset < samples) {
    size_t need = (size_t)s_mn_feed_chunksize - s_mn_pending_samples;
    size_t copy_samples = samples - offset;
    if (copy_samples > need) {
      copy_samples = need;
    }

    memcpy(&s_mn_feed_buffer[s_mn_pending_samples],
           &pcm[offset],
           copy_samples * sizeof(int16_t));
    s_mn_pending_samples += copy_samples;
    offset += copy_samples;

    if (s_mn_pending_samples < (size_t)s_mn_feed_chunksize) {
      continue;
    }

    s_mn_pending_samples = 0;
    esp_mn_state_t state = s_mn_handle->detect(s_mn_data, s_mn_feed_buffer);
    if (state == ESP_MN_STATE_DETECTING) {
      continue;
    }

    if (state == ESP_MN_STATE_TIMEOUT) {
      voice_multinet_reset();
      continue;
    }

    if (state != ESP_MN_STATE_DETECTED) {
      continue;
    }

    esp_mn_results_t *results = s_mn_handle->get_results(s_mn_data);
    if (results == NULL) {
      ESP_LOGW(TAG, "MultiNet detected but result is NULL");
      voice_multinet_reset();
      continue;
    }

    if (results->num > 0) {
      ESP_LOGI(TAG,
               "MultiNet result: num=%d top_id=%d top_prob=%d.%03d text=%s raw=%s",
               results->num,
               results->command_id[0],
               (int)results->prob[0],
               (int)(results->prob[0] * 1000.0f) % 1000,
               results->string,
               results->raw_string);
    }

    for (int i = 0; i < results->num && i < ESP_MN_RESULT_MAX_NUM; ++i) {
      if (results->command_id[i] == VOICE_UPLOAD_MULTINET_WAKE_COMMAND_ID) {
        ESP_LOGI(TAG,
                 "wake phrase detected: phrase=%s command=%s prob=%d.%03d text=%s raw=%s",
                 VOICE_UPLOAD_MULTINET_WAKE_DISPLAY,
                 VOICE_UPLOAD_MULTINET_WAKE_PHRASE,
                 (int)results->prob[i],
                 (int)(results->prob[i] * 1000.0f) % 1000,
                 results->string,
                 results->raw_string);
        voice_multinet_reset();
        return true;
      }
    }

    voice_multinet_reset();
  }

  return false;
}

static void voice_log_model_partition_probe(void)
{
  const esp_partition_t *partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                                              ESP_PARTITION_SUBTYPE_ANY,
                                                              "model");
  if (partition == NULL) {
    ESP_LOGW(TAG, "model partition probe: partition not found");
    return;
  }

  uint8_t header[16] = {0};
  esp_err_t ret = esp_partition_read(partition, 0, header, sizeof(header));
  if (ret != ESP_OK) {
    ESP_LOGW(TAG, "model partition probe: read failed: %s", esp_err_to_name(ret));
    return;
  }

  uint32_t model_count = (uint32_t)header[0] |
                         ((uint32_t)header[1] << 8) |
                         ((uint32_t)header[2] << 16) |
                         ((uint32_t)header[3] << 24);
  ESP_LOGI(TAG,
           "model partition probe: offset=0x%lx size=%lu model_count=%lu head=%02x %02x %02x %02x",
           (unsigned long)partition->address,
           (unsigned long)partition->size,
           (unsigned long)model_count,
           header[0],
           header[1],
           header[2],
           header[3]);
}

static esp_err_t voice_srmodel_init(void)
{
  if (s_srmodels != NULL) {
    return ESP_OK;
  }

  s_srmodels = esp_srmodel_init("model");
  ESP_RETURN_ON_FALSE(s_srmodels != NULL,
                      ESP_ERR_NOT_FOUND,
                      TAG,
                      "load SR model list failed");

  ESP_LOGI(TAG, "SR model list ready: count=%d", s_srmodels->num);
  for (int i = 0; i < s_srmodels->num; ++i) {
    ESP_LOGI(TAG,
             "SR model[%d]: name=%s info=%s",
             i,
             s_srmodels->model_name != NULL && s_srmodels->model_name[i] != NULL
                 ? s_srmodels->model_name[i]
                 : "",
             s_srmodels->model_info != NULL && s_srmodels->model_info[i] != NULL
                 ? s_srmodels->model_info[i]
                 : "");
  }

  ESP_RETURN_ON_FALSE(esp_srmodel_exists(s_srmodels,
                                         (char *)VOICE_UPLOAD_MULTINET_MODEL_NAME) >= 0,
                      ESP_ERR_NOT_FOUND,
                      TAG,
                      "MultiNet model is not in SR model list: %s",
                      VOICE_UPLOAD_MULTINET_MODEL_NAME);

  return ESP_OK;
}

static bool should_log_tts_chunk(int chunk_index, const char *text)
{
  return chunk_index <= 1 ||
         (chunk_index % VOICE_UPLOAD_TTS_CHUNK_LOG_INTERVAL) == 0 ||
         (text != NULL && text[0] != '\0');
}

static void clear_pending_tts_audio(void)
{
  free(s_tts_audio_rx.data);
  free(s_tts_audio_rx.text);
  memset(&s_tts_audio_rx, 0, sizeof(s_tts_audio_rx));
}

static void clear_tts_text_slot(tts_text_slot_t *slot)
{
  if (slot == NULL) {
    return;
  }

  free(slot->text);
  memset(slot, 0, sizeof(*slot));
}

static void clear_pending_tts_texts(void)
{
  for (size_t i = 0; i < VOICE_UPLOAD_TTS_TEXT_SLOTS; ++i) {
    clear_tts_text_slot(&s_tts_text_slots[i]);
  }
}

static void clear_pending_ws_text(void)
{
  free(s_ws_text_rx.data);
  memset(&s_ws_text_rx, 0, sizeof(s_ws_text_rx));
}

static bool tts_ids_match(const tts_text_slot_t *slot, int response_id, int speech_index)
{
  return slot != NULL &&
         slot->active &&
         slot->response_id == response_id &&
         slot->speech_index == speech_index;
}

static void remember_tts_text(int response_id, int speech_index, const char *text)
{
  if (response_id < 0 || speech_index < 0 || text == NULL || text[0] == '\0') {
    return;
  }

  char *copy = voice_upload_strdup(text);
  if (copy == NULL) {
    ESP_LOGW(TAG, "copy TTS text failed");
    return;
  }

  tts_text_slot_t *slot = NULL;
  for (size_t i = 0; i < VOICE_UPLOAD_TTS_TEXT_SLOTS; ++i) {
    if (tts_ids_match(&s_tts_text_slots[i], response_id, speech_index)) {
      slot = &s_tts_text_slots[i];
      break;
    }
  }
  if (slot == NULL) {
    for (size_t i = 0; i < VOICE_UPLOAD_TTS_TEXT_SLOTS; ++i) {
      if (!s_tts_text_slots[i].active) {
        slot = &s_tts_text_slots[i];
        break;
      }
    }
  }
  if (slot == NULL) {
    slot = &s_tts_text_slots[0];
  }

  clear_tts_text_slot(slot);
  slot->active = true;
  slot->response_id = response_id;
  slot->speech_index = speech_index;
  slot->text = copy;
}

static char *take_tts_text(int response_id, int speech_index)
{
  for (size_t i = 0; i < VOICE_UPLOAD_TTS_TEXT_SLOTS; ++i) {
    tts_text_slot_t *slot = &s_tts_text_slots[i];
    if (!tts_ids_match(slot, response_id, speech_index)) {
      continue;
    }

    char *text = slot->text;
    slot->text = NULL;
    clear_tts_text_slot(slot);
    return text;
  }

  return NULL;
}

static void handle_tts_start_json(const cJSON *root)
{
  if (!s_accept_tts) {
    ESP_LOGI(TAG, "drop TTS start while new task is recording");
    return;
  }

  const cJSON *response_id = cJSON_GetObjectItem(root, "responseId");
  const cJSON *speech_index = cJSON_GetObjectItem(root, "speechIndex");
  const cJSON *text = cJSON_GetObjectItem(root, "text");

  if (!cJSON_IsNumber(response_id) ||
      !cJSON_IsNumber(speech_index) ||
      !cJSON_IsString(text)) {
    ESP_LOGW(TAG, "invalid tts_start metadata");
    return;
  }

  remember_tts_text(response_id->valueint, speech_index->valueint, text->valuestring);
}

static void handle_asr_result_json(const cJSON *root)
{
  const cJSON *text = cJSON_GetObjectItem(root, "text");

  if (!cJSON_IsString(text)) {
    ESP_LOGW(TAG, "invalid asr_result metadata");
    return;
  }

  lcd_show_user_question(text->valuestring);
}

static void handle_tts_audio_json(const cJSON *root)
{
  if (!s_accept_tts) {
    ESP_LOGI(TAG, "drop TTS audio metadata while new task is recording");
    return;
  }

  const cJSON *sample_rate = cJSON_GetObjectItem(root, "sampleRate");
  const cJSON *format = cJSON_GetObjectItem(root, "format");
  const cJSON *channels = cJSON_GetObjectItem(root, "channels");
  const cJSON *bytes = cJSON_GetObjectItem(root, "bytes");
  const cJSON *response_id = cJSON_GetObjectItem(root, "responseId");
  const cJSON *speech_index = cJSON_GetObjectItem(root, "speechIndex");
  const cJSON *chunk_index = cJSON_GetObjectItem(root, "chunkIndex");

  if (!cJSON_IsNumber(sample_rate) ||
      !cJSON_IsString(format) ||
      !cJSON_IsNumber(channels) ||
      !cJSON_IsNumber(bytes)) {
    ESP_LOGW(TAG, "invalid tts_audio metadata");
    return;
  }

  if (strcmp(format->valuestring, "pcm_s16le") != 0) {
    ESP_LOGW(TAG, "unsupported TTS PCM format: %s", format->valuestring);
    return;
  }

  if (channels->valueint != 1 && channels->valueint != 2) {
    ESP_LOGW(TAG, "unsupported TTS channel count: %d", channels->valueint);
    return;
  }

  if (sample_rate->valueint <= 0 || bytes->valueint <= 0) {
    ESP_LOGW(TAG, "invalid TTS audio size/rate: rate=%d bytes=%d", sample_rate->valueint, bytes->valueint);
    return;
  }

  clear_pending_tts_audio();
  s_tts_audio_rx.data = (uint8_t *)malloc((size_t)bytes->valueint);
  if (s_tts_audio_rx.data == NULL) {
    ESP_LOGE(TAG, "allocate TTS PCM buffer failed: %d bytes", bytes->valueint);
    return;
  }

  s_tts_audio_rx.active = true;
  s_tts_audio_rx.expected_bytes = (size_t)bytes->valueint;
  s_tts_audio_rx.sample_rate_hz = (uint32_t)sample_rate->valueint;
  s_tts_audio_rx.channels = channels->valueint;
  s_tts_audio_rx.response_id = cJSON_IsNumber(response_id) ? response_id->valueint : -1;
  s_tts_audio_rx.speech_index = cJSON_IsNumber(speech_index) ? speech_index->valueint : -1;
  s_tts_audio_rx.chunk_index = cJSON_IsNumber(chunk_index) ? chunk_index->valueint : -1;
  s_tts_audio_rx.meta_us = esp_timer_get_time();
  s_tts_audio_rx.text = take_tts_text(s_tts_audio_rx.response_id, s_tts_audio_rx.speech_index);

  if (should_log_tts_chunk(s_tts_audio_rx.chunk_index, s_tts_audio_rx.text)) {
    ESP_LOGI(TAG,
             "expect TTS PCM: response=%d speech=%d chunk=%d bytes=%u rate=%lu channels=%d text=%s",
             s_tts_audio_rx.response_id,
             s_tts_audio_rx.speech_index,
             s_tts_audio_rx.chunk_index,
             (unsigned)s_tts_audio_rx.expected_bytes,
             (unsigned long)s_tts_audio_rx.sample_rate_hz,
             s_tts_audio_rx.channels,
             s_tts_audio_rx.text != NULL ? s_tts_audio_rx.text : "");
  }
}

static void handle_server_json(const char *json, int len)
{
  char *json_copy = (char *)malloc((size_t)len + 1);
  if (json_copy == NULL) {
    ESP_LOGE(TAG, "allocate server JSON buffer failed");
    return;
  }
  memcpy(json_copy, json, (size_t)len);
  json_copy[len] = '\0';

  cJSON *root = cJSON_Parse(json_copy);
  if (root == NULL) {
    ESP_LOGW(TAG, "invalid server JSON: %.*s", len, json);
    free(json_copy);
    return;
  }

  const cJSON *type = cJSON_GetObjectItem(root, "type");
  if (!cJSON_IsString(type)) {
    ESP_LOGW(TAG, "server JSON without type");
    cJSON_Delete(root);
    free(json_copy);
    return;
  }

  if (strcmp(type->valuestring, "asr_result") == 0) {
    handle_asr_result_json(root);
    ESP_LOGI(TAG, "server: %.*s", len, json);
  } else if (strcmp(type->valuestring, "tts_start") == 0) {
    handle_tts_start_json(root);
    ESP_LOGI(TAG, "server: %.*s", len, json);
  } else if (strcmp(type->valuestring, "tts_audio") == 0) {
    handle_tts_audio_json(root);
  } else if (strcmp(type->valuestring, "tts_finished") == 0) {
    ESP_LOGI(TAG, "server: %.*s", len, json);
    if (!audio_is_playback_active() && s_voice_state != VOICE_STATE_RECORDING) {
      voice_set_state(VOICE_STATE_IDLE);
    }
  } else if (strcmp(type->valuestring, "ai_finished") == 0) {
    ESP_LOGI(TAG, "server: %.*s", len, json);
    if (s_ws_connected && s_voice_state != VOICE_STATE_RECORDING) {
      s_continue_after_tts = true;
    }
    if (!audio_is_playback_active() && s_voice_state != VOICE_STATE_RECORDING) {
      voice_set_state(VOICE_STATE_IDLE);
    }
  } else if (strcmp(type->valuestring, "error") == 0) {
    const cJSON *stage = cJSON_GetObjectItem(root, "stage");
    const cJSON *message = cJSON_GetObjectItem(root, "message");
    s_continue_after_tts = false;
    ESP_LOGE(TAG,
             "server error: stage=%s message=%s",
             cJSON_IsString(stage) ? stage->valuestring : "?",
             cJSON_IsString(message) ? message->valuestring : "?");
    voice_set_state(VOICE_STATE_IDLE);
  } else {
    ESP_LOGI(TAG, "server: %.*s", len, json);
  }

  cJSON_Delete(root);
  free(json_copy);
}

static void handle_server_binary(const uint8_t *data, int len)
{
  if (!s_accept_tts) {
    if (s_tts_audio_rx.active) {
      clear_pending_tts_audio();
    }
    ESP_LOGI(TAG, "drop TTS binary while new task is recording: %d bytes", len);
    return;
  }

  if (!s_tts_audio_rx.active || s_tts_audio_rx.data == NULL) {
    ESP_LOGW(TAG, "unexpected binary frame: %d bytes", len);
    return;
  }

  if (len <= 0) {
    return;
  }

  size_t available = s_tts_audio_rx.expected_bytes - s_tts_audio_rx.received_bytes;
  if ((size_t)len > available) {
    ESP_LOGW(TAG,
             "TTS PCM is larger than metadata: got=%d available=%u",
             len,
             (unsigned)available);
    clear_pending_tts_audio();
    return;
  }

  memcpy(s_tts_audio_rx.data + s_tts_audio_rx.received_bytes, data, (size_t)len);
  s_tts_audio_rx.received_bytes += (size_t)len;

  if (s_tts_audio_rx.received_bytes < s_tts_audio_rx.expected_bytes) {
    return;
  }

  int64_t receive_us = esp_timer_get_time() - s_tts_audio_rx.meta_us;
  if (should_log_tts_chunk(s_tts_audio_rx.chunk_index, s_tts_audio_rx.text)) {
    ESP_LOGI(TAG,
             "TTS PCM complete: response=%d speech=%d chunk=%d bytes=%u receive=%lld ms",
             s_tts_audio_rx.response_id,
             s_tts_audio_rx.speech_index,
             s_tts_audio_rx.chunk_index,
             (unsigned)s_tts_audio_rx.expected_bytes,
             (long long)(receive_us / 1000));
  }

  esp_err_t err = audio_queue_pcm_s16le_with_text(s_tts_audio_rx.data,
                                                  s_tts_audio_rx.expected_bytes,
                                                  s_tts_audio_rx.sample_rate_hz,
                                                  s_tts_audio_rx.channels,
                                                  s_tts_audio_rx.text);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "queue TTS PCM failed: %s", esp_err_to_name(err));
  } else {
    voice_set_state(VOICE_STATE_SPEAKING);
  }

  clear_pending_tts_audio();
}

static void handle_server_text_event(const esp_websocket_event_data_t *data)
{
  if (data == NULL || data->data_ptr == NULL || data->data_len <= 0) {
    return;
  }

  size_t payload_len = data->payload_len > 0 ? (size_t)data->payload_len : (size_t)data->data_len;
  size_t payload_offset = data->payload_offset > 0 ? (size_t)data->payload_offset : 0;
  bool fragmented = s_ws_text_rx.active ||
                    payload_offset > 0 ||
                    payload_len > (size_t)data->data_len ||
                    !data->fin;

  if (!fragmented) {
    handle_server_json((const char *)data->data_ptr, data->data_len);
    return;
  }

  if (!s_ws_text_rx.active || payload_offset == 0) {
    clear_pending_ws_text();
    s_ws_text_rx.data = (char *)malloc(payload_len + 1U);
    if (s_ws_text_rx.data == NULL) {
      ESP_LOGE(TAG, "allocate WebSocket text buffer failed: %u bytes", (unsigned)payload_len);
      return;
    }
    s_ws_text_rx.active = true;
    s_ws_text_rx.expected_bytes = payload_len;
  }

  if (!s_ws_text_rx.active ||
      payload_len != s_ws_text_rx.expected_bytes ||
      payload_offset + (size_t)data->data_len > s_ws_text_rx.expected_bytes) {
    ESP_LOGW(TAG,
             "invalid fragmented WebSocket text: offset=%u len=%d total=%u expected=%u",
             (unsigned)payload_offset,
             data->data_len,
             (unsigned)payload_len,
             (unsigned)s_ws_text_rx.expected_bytes);
    clear_pending_ws_text();
    return;
  }

  memcpy(s_ws_text_rx.data + payload_offset, data->data_ptr, (size_t)data->data_len);
  size_t end = payload_offset + (size_t)data->data_len;
  if (end > s_ws_text_rx.received_bytes) {
    s_ws_text_rx.received_bytes = end;
  }

  if (s_ws_text_rx.received_bytes < s_ws_text_rx.expected_bytes) {
    return;
  }

  s_ws_text_rx.data[s_ws_text_rx.expected_bytes] = '\0';
  handle_server_json(s_ws_text_rx.data, (int)s_ws_text_rx.expected_bytes);
  clear_pending_ws_text();
}

static void websocket_event_handler(void *handler_args,
                                    esp_event_base_t base,
                                    int32_t event_id,
                                    void *event_data)
{
  (void)handler_args;
  (void)base;

  esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;

  switch (event_id) {
  case WEBSOCKET_EVENT_CONNECTED:
    s_ws_connected = true;
    ESP_LOGI(TAG, "WebSocket connected");
    break;
  case WEBSOCKET_EVENT_DISCONNECTED:
    s_ws_connected = false;
    s_continue_after_tts = false;
    clear_pending_tts_audio();
    clear_pending_tts_texts();
    clear_pending_ws_text();
    voice_set_state(VOICE_STATE_IDLE);
    ESP_LOGW(TAG, "WebSocket disconnected");
    break;
  case WEBSOCKET_EVENT_DATA:
    if (data == NULL || data->data_len <= 0) {
      break;
    }
    if (data->op_code == WS_TRANSPORT_OPCODES_TEXT ||
        (data->op_code == WS_TRANSPORT_OPCODES_CONT && s_ws_text_rx.active)) {
      handle_server_text_event(data);
    } else if (data->op_code == WS_TRANSPORT_OPCODES_BINARY ||
               (data->op_code == WS_TRANSPORT_OPCODES_CONT && s_tts_audio_rx.active)) {
      handle_server_binary((const uint8_t *)data->data_ptr, data->data_len);
    }
    break;
  case WEBSOCKET_EVENT_ERROR:
    ESP_LOGE(TAG, "WebSocket error");
    break;
  default:
    break;
  }
}

static esp_err_t voice_i2s_init(void)
{
  if (s_i2s_rx_chan != NULL) {
    return ESP_OK;
  }

  i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
  ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, NULL, &s_i2s_rx_chan),
                      TAG,
                      "create I2S RX channel failed");

  i2s_std_config_t std_cfg = {
    .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(VOICE_UPLOAD_SAMPLE_RATE_HZ),
    .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,
                                                    I2S_SLOT_MODE_MONO),
    .gpio_cfg = {
      .mclk = I2S_GPIO_UNUSED,
      .bclk = VOICE_UPLOAD_I2S_PIN_BCLK,
      .ws = VOICE_UPLOAD_I2S_PIN_WS,
      .dout = I2S_GPIO_UNUSED,
      .din = VOICE_UPLOAD_I2S_PIN_DIN,
      .invert_flags = {
        .mclk_inv = false,
        .bclk_inv = false,
        .ws_inv = false,
      },
    },
  };
  std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;

  ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_i2s_rx_chan, &std_cfg),
                      TAG,
                      "init I2S RX std mode failed");
  ESP_RETURN_ON_ERROR(i2s_channel_enable(s_i2s_rx_chan), TAG, "enable I2S RX failed");

  ESP_LOGI(TAG,
           "INMP441 I2S ready: BCLK=%d WS=%d DIN=%d sample_rate=%d",
           VOICE_UPLOAD_I2S_PIN_BCLK,
           VOICE_UPLOAD_I2S_PIN_WS,
           VOICE_UPLOAD_I2S_PIN_DIN,
           VOICE_UPLOAD_SAMPLE_RATE_HZ);
  return ESP_OK;
}

#if VOICE_UPLOAD_AFE_DEBUG_ENABLE
static void voice_afe_ref_push_sample_locked(int16_t sample)
{
  if (s_afe_ref_ring == NULL || s_afe_ref_ring_size == 0) {
    return;
  }

  s_afe_ref_ring[s_afe_ref_write_pos] = sample;
  s_afe_ref_write_pos = (s_afe_ref_write_pos + 1U) % s_afe_ref_ring_size;
  if (s_afe_ref_count < s_afe_ref_ring_size) {
    ++s_afe_ref_count;
  } else {
    s_afe_ref_read_pos = (s_afe_ref_read_pos + 1U) % s_afe_ref_ring_size;
  }
}

static int16_t voice_afe_ref_pop_sample(void)
{
  int16_t sample = 0;

  portENTER_CRITICAL(&s_afe_ref_mux);
  if (s_afe_ref_ring != NULL && s_afe_ref_count > 0) {
    sample = s_afe_ref_ring[s_afe_ref_read_pos];
    s_afe_ref_read_pos = (s_afe_ref_read_pos + 1U) % s_afe_ref_ring_size;
    --s_afe_ref_count;
  }
  portEXIT_CRITICAL(&s_afe_ref_mux);

  return sample;
}

static size_t voice_afe_ref_available(void)
{
  size_t count = 0;

  portENTER_CRITICAL(&s_afe_ref_mux);
  count = s_afe_ref_count;
  portEXIT_CRITICAL(&s_afe_ref_mux);

  return count;
}

static uint32_t voice_afe_ref_sample_rate(void)
{
  uint32_t sample_rate = 0;

  portENTER_CRITICAL(&s_afe_ref_mux);
  sample_rate = s_afe_ref_sample_rate;
  portEXIT_CRITICAL(&s_afe_ref_mux);

  return sample_rate;
}

static vad_state_t voice_afe_last_vad_state(void)
{
  return s_afe_last_vad_state;
}

static bool voice_afe_vad_available(void)
{
  return s_afe_handle != NULL && s_afe_data != NULL;
}

static void voice_afe_playback_ref_cb(const int16_t *pcm,
                                      size_t frames,
                                      int channels,
                                      uint32_t sample_rate_hz)
{
  if (pcm == NULL || frames == 0 || channels <= 0 || sample_rate_hz == 0) {
    return;
  }

  portENTER_CRITICAL(&s_afe_ref_mux);
  if (s_afe_ref_sample_rate != sample_rate_hz) {
    s_afe_ref_sample_rate = sample_rate_hz;
    s_afe_ref_resample_accum = 0;
  }

  for (size_t i = 0; i < frames; ++i) {
    int16_t sample = pcm[i * (size_t)channels];
    if (channels >= 2) {
      int32_t mixed = (int32_t)pcm[i * (size_t)channels] +
                      (int32_t)pcm[i * (size_t)channels + 1U];
      sample = (int16_t)(mixed / 2);
    }

    s_afe_ref_resample_accum += VOICE_UPLOAD_SAMPLE_RATE_HZ;
    while (s_afe_ref_resample_accum >= sample_rate_hz) {
      voice_afe_ref_push_sample_locked(sample);
      s_afe_ref_resample_accum -= sample_rate_hz;
    }
  }
  portEXIT_CRITICAL(&s_afe_ref_mux);
}

static esp_err_t voice_afe_debug_init(void)
{
  if (s_afe_data != NULL) {
    return ESP_OK;
  }

  esp_err_t sr_ret = voice_srmodel_init();
  if (sr_ret != ESP_OK) {
    ESP_LOGW(TAG, "SR model list unavailable for AFE: %s", esp_err_to_name(sr_ret));
  }

  esp_log_level_set("AFE_CONFIG", ESP_LOG_ERROR);
  afe_config_t *afe_config = afe_config_init("MR",
                                             s_srmodels,
                                             AFE_TYPE_SR,
                                             AFE_MODE_LOW_COST);
  esp_log_level_set("AFE_CONFIG", ESP_LOG_WARN);
  ESP_RETURN_ON_FALSE(afe_config != NULL, ESP_ERR_NO_MEM, TAG, "create AFE config failed");

  afe_config->wakenet_init = false;
  afe_config->se_init = false;
  afe_config->vad_init = true;
  afe_config->aec_init = true;
  afe_config->ns_init = false;
  afe_config->agc_init = false;

  s_afe_handle = esp_afe_handle_from_config(afe_config);
  if (s_afe_handle == NULL) {
    afe_config_free(afe_config);
    ESP_LOGW(TAG, "AFE handle is unavailable");
    return ESP_ERR_NOT_FOUND;
  }

  s_afe_data = s_afe_handle->create_from_config(afe_config);
  afe_config_free(afe_config);
  ESP_RETURN_ON_FALSE(s_afe_data != NULL, ESP_ERR_NO_MEM, TAG, "create AFE failed");

  s_afe_feed_chunksize = s_afe_handle->get_feed_chunksize(s_afe_data);
  s_afe_feed_channels = s_afe_handle->get_feed_channel_num(s_afe_data);
  ESP_RETURN_ON_FALSE(s_afe_feed_chunksize > 0 && s_afe_feed_channels > 0,
                      ESP_ERR_INVALID_STATE,
                      TAG,
                      "invalid AFE feed shape: chunksize=%d channels=%d",
                      s_afe_feed_chunksize,
                      s_afe_feed_channels);

  s_afe_feed_buffer = (int16_t *)calloc((size_t)s_afe_feed_chunksize *
                                          (size_t)s_afe_feed_channels,
                                        sizeof(int16_t));
  if (s_afe_feed_buffer == NULL) {
    s_afe_handle->destroy(s_afe_data);
    s_afe_data = NULL;
    return ESP_ERR_NO_MEM;
  }

  s_afe_ref_ring_size = VOICE_UPLOAD_AFE_REF_BUFFER_SAMPLES;
  s_afe_ref_ring = (int16_t *)calloc(s_afe_ref_ring_size, sizeof(int16_t));
  if (s_afe_ref_ring == NULL) {
    free(s_afe_feed_buffer);
    s_afe_feed_buffer = NULL;
    s_afe_handle->destroy(s_afe_data);
    s_afe_data = NULL;
    return ESP_ERR_NO_MEM;
  }

  audio_set_pcm_playback_ref_cb(voice_afe_playback_ref_cb);

  ESP_LOGI(TAG,
           "AFE debug ready: feed_chunksize=%d feed_channels=%d fetch_chunksize=%d ref_buffer=%u samples",
           s_afe_feed_chunksize,
           s_afe_feed_channels,
           s_afe_handle->get_fetch_chunksize(s_afe_data),
           (unsigned)s_afe_ref_ring_size);
#if CONFIG_SR_VADN_VADNET1_MEDIUM
  ESP_LOGI(TAG, "voice VAD source: ESP-SR AFE VADNet1 Medium");
#elif CONFIG_SR_VADN_WEBRTC
  ESP_LOGI(TAG, "voice VAD source: ESP-SR AFE WebRTC VAD");
#else
  ESP_LOGI(TAG, "voice VAD source: ESP-SR AFE VAD");
#endif
  return ESP_OK;
}

static void voice_afe_debug_fetch(void)
{
  if (s_afe_handle == NULL || s_afe_data == NULL) {
    return;
  }

  afe_fetch_result_t *result = s_afe_handle->fetch_with_delay(s_afe_data, 0);
  if (result == NULL || result->ret_value == ESP_FAIL) {
    return;
  }

  ++s_afe_fetch_count;
  s_afe_last_vad_state = result->vad_state;
  bool vad_changed = result->vad_state != s_afe_logged_vad_state;
  bool periodic_log = (s_afe_fetch_count % VOICE_UPLOAD_AFE_LOG_INTERVAL) == 0U;
  if (vad_changed || periodic_log) {
    s_afe_logged_vad_state = result->vad_state;
    ESP_LOGI(TAG,
             "AFE debug: feed=%lu vad=%d changed=%d data_size=%d wake=%d trigger=%d volume=%.1f dB ref_avail=%u ref_rate=%lu",
             (unsigned long)s_afe_feed_count,
             result->vad_state,
             vad_changed,
             result->data_size,
             result->wakeup_state,
             result->trigger_channel_id,
             result->data_volume,
             (unsigned)voice_afe_ref_available(),
             (unsigned long)voice_afe_ref_sample_rate());
  }
}

static void voice_afe_debug_feed(const int16_t *mic, size_t samples)
{
  if (s_afe_handle == NULL || s_afe_data == NULL || s_afe_feed_buffer == NULL ||
      mic == NULL || samples == 0) {
    return;
  }

  size_t offset = 0;
  while (offset < samples) {
    size_t copy_samples = samples - offset;
    size_t available = (size_t)s_afe_feed_chunksize - s_afe_pending_samples;
    if (copy_samples > available) {
      copy_samples = available;
    }

    for (size_t i = 0; i < copy_samples; ++i) {
      size_t frame = s_afe_pending_samples + i;
      s_afe_feed_buffer[frame * (size_t)s_afe_feed_channels] = mic[offset + i];
      if (s_afe_feed_channels > 1) {
        s_afe_feed_buffer[frame * (size_t)s_afe_feed_channels + 1U] =
          voice_afe_ref_pop_sample();
      }
      for (int ch = 2; ch < s_afe_feed_channels; ++ch) {
        s_afe_feed_buffer[frame * (size_t)s_afe_feed_channels + (size_t)ch] = 0;
      }
    }

    s_afe_pending_samples += copy_samples;
    offset += copy_samples;

    if (s_afe_pending_samples < (size_t)s_afe_feed_chunksize) {
      continue;
    }

    int feed_ret = s_afe_handle->feed(s_afe_data, s_afe_feed_buffer);
    if (feed_ret < 0) {
      ESP_LOGW(TAG, "AFE feed failed: %d", feed_ret);
      s_afe_pending_samples = 0;
      return;
    }

    ++s_afe_feed_count;
    s_afe_pending_samples = 0;
    voice_afe_debug_fetch();
  }
}
#endif

static bool wifi_is_connected(void)
{
  app_wifi_status_t status = {0};
  app_wifi_get_status(&status);
  return status.connected;
}

static esp_err_t websocket_ensure_connected(void)
{
  if (s_ws_mutex != NULL) {
    xSemaphoreTake(s_ws_mutex, portMAX_DELAY);
  }

  if (s_ws_client != NULL && s_ws_connected) {
    if (s_ws_mutex != NULL) {
      xSemaphoreGive(s_ws_mutex);
    }
    return ESP_OK;
  }

  esp_err_t ret = ESP_OK;
  if (s_ws_client == NULL) {
    esp_websocket_client_config_t ws_cfg = {
      .uri = VOICE_UPLOAD_WS_URI,
      .buffer_size = VOICE_UPLOAD_WS_RX_BUFFER_BYTES,
      .network_timeout_ms = 5000,
      .reconnect_timeout_ms = 3000,
    };

    s_ws_client = esp_websocket_client_init(&ws_cfg);
    if (s_ws_client == NULL) {
      ret = ESP_ERR_NO_MEM;
      ESP_LOGE(TAG, "create WebSocket client failed");
      goto cleanup;
    }
    ret = esp_websocket_register_events(s_ws_client,
                                        WEBSOCKET_EVENT_ANY,
                                        websocket_event_handler,
                                        NULL);
    if (ret != ESP_OK) {
      ESP_LOGE(TAG, "register WebSocket events failed: %s", esp_err_to_name(ret));
      goto cleanup;
    }
  }

  if (!s_ws_started) {
    s_ws_connected = false;
    ESP_LOGI(TAG, "connecting WebSocket: %s", VOICE_UPLOAD_WS_URI);
    ret = esp_websocket_client_start(s_ws_client);
    if (ret != ESP_OK) {
      ESP_LOGE(TAG, "start WebSocket client failed: %s", esp_err_to_name(ret));
      goto cleanup;
    }
    s_ws_started = true;
  }

  int64_t deadline = esp_timer_get_time() + (int64_t)VOICE_UPLOAD_CONNECT_TIMEOUT_MS * 1000;
  while (!s_ws_connected && esp_timer_get_time() < deadline) {
    vTaskDelay(pdMS_TO_TICKS(50));
  }

  if (s_ws_connected) {
    ret = ESP_OK;
    goto cleanup;
  }

  ESP_LOGW(TAG, "WebSocket connect timeout, restart client next time");
  esp_websocket_client_stop(s_ws_client);
  s_ws_started = false;
  ret = ESP_ERR_TIMEOUT;

cleanup:
  if (s_ws_mutex != NULL) {
    xSemaphoreGive(s_ws_mutex);
  }
  return ret;
}

static esp_err_t websocket_send_text(const char *text)
{
  int ret = esp_websocket_client_send_text(s_ws_client,
                                           text,
                                           (int)strlen(text),
                                           VOICE_UPLOAD_SEND_TIMEOUT_TICKS);
  return ret >= 0 ? ESP_OK : ESP_FAIL;
}

static esp_err_t websocket_send_json_type(const char *type)
{
  char message[32];
  int len = snprintf(message, sizeof(message), "{\"type\":\"%s\"}", type);
  ESP_RETURN_ON_FALSE(len > 0 && len < (int)sizeof(message),
                      ESP_ERR_INVALID_ARG,
                      TAG,
                      "invalid JSON type");
  return websocket_send_text(message);
}

static esp_err_t websocket_send_bin(const int16_t *pcm, size_t sample_count)
{
  int ret = esp_websocket_client_send_bin(s_ws_client,
                                          (const char *)pcm,
                                          (int)(sample_count * sizeof(int16_t)),
                                          VOICE_UPLOAD_SEND_TIMEOUT_TICKS);
  return ret >= 0 ? ESP_OK : ESP_FAIL;
}

static void voice_ws_monitor_task(void *arg)
{
  (void)arg;
  int64_t startup_deadline = esp_timer_get_time() +
                             (int64_t)VOICE_UPLOAD_STARTUP_WS_TIMEOUT_MS * 1000;
  bool startup_prompt_played = false;

  while (true) {
    if (!s_ws_connected && wifi_is_connected()) {
      esp_err_t err = websocket_ensure_connected();
      if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
        ESP_LOGW(TAG, "background WebSocket connect failed: %s", esp_err_to_name(err));
      }
    }

    if (!startup_prompt_played &&
        !s_ws_connected &&
        esp_timer_get_time() >= startup_deadline) {
      ESP_LOGW(TAG, "startup WebSocket is not connected after %d ms",
               VOICE_UPLOAD_STARTUP_WS_TIMEOUT_MS);
      voice_play_wav_prompt(VOICE_UPLOAD_PROMPT_NET_ERROR, false);
      startup_prompt_played = true;
    }

    vTaskDelay(pdMS_TO_TICKS(VOICE_UPLOAD_RECONNECT_DELAY_MS));
  }
}

static int16_t convert_mic_sample(int32_t sample)
{
  int32_t scaled = sample >> VOICE_UPLOAD_MIC_SHIFT;
  if (scaled > INT16_MAX) {
    return INT16_MAX;
  }
  if (scaled < INT16_MIN) {
    return INT16_MIN;
  }
  return (int16_t)scaled;
}

static esp_err_t read_mic_pcm_chunk(int32_t *raw,
                                    int16_t *pcm,
                                    size_t *samples_out,
                                    TickType_t timeout_ticks)
{
  if (raw == NULL || pcm == NULL || samples_out == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  *samples_out = 0;

  size_t bytes_read = 0;
  esp_err_t ret = i2s_channel_read(s_i2s_rx_chan,
                                   raw,
                                   VOICE_UPLOAD_FRAMES_PER_CHUNK * sizeof(int32_t),
                                   &bytes_read,
                                   timeout_ticks);
  if (ret == ESP_ERR_TIMEOUT || bytes_read == 0) {
    return ESP_ERR_TIMEOUT;
  }
  ESP_RETURN_ON_ERROR(ret, TAG, "read INMP441 failed");

  size_t samples = bytes_read / sizeof(int32_t);
  for (size_t i = 0; i < samples; ++i) {
    pcm[i] = convert_mic_sample(raw[i]);
  }

#if VOICE_UPLOAD_AFE_DEBUG_ENABLE
  voice_afe_debug_feed(pcm, samples);
#endif

  *samples_out = samples;
  return ESP_OK;
}

static bool vad_detect_voice(const int16_t *pcm,
                             size_t samples,
                             uint32_t *avg_abs_out,
                             int16_t *peak_out,
                             int32_t *dc_out)
{
  if (pcm == NULL || samples == 0) {
    if (avg_abs_out != NULL) {
      *avg_abs_out = 0;
    }
    if (peak_out != NULL) {
      *peak_out = 0;
    }
    if (dc_out != NULL) {
      *dc_out = 0;
    }
    return false;
  }

  int64_t sum = 0;
  for (size_t i = 0; i < samples; ++i) {
    sum += pcm[i];
  }

  int32_t dc = (int32_t)(sum / (int64_t)samples);
  uint64_t sum_abs = 0;
  int16_t peak = 0;
  for (size_t i = 0; i < samples; ++i) {
    int32_t centered = (int32_t)pcm[i] - dc;
    int32_t abs_sample = centered < 0 ? -centered : centered;
    sum_abs += (uint32_t)abs_sample;
    if (abs_sample > peak) {
      peak = (int16_t)abs_sample;
    }
  }

  uint32_t avg_abs = (uint32_t)(sum_abs / samples);
  if (avg_abs_out != NULL) {
    *avg_abs_out = avg_abs;
  }
  if (peak_out != NULL) {
    *peak_out = peak;
  }
  if (dc_out != NULL) {
    *dc_out = dc;
  }

  return avg_abs >= VOICE_UPLOAD_VAD_AVG_THRESHOLD &&
         peak >= VOICE_UPLOAD_VAD_PEAK_THRESHOLD &&
         (uint32_t)peak <= avg_abs * VOICE_UPLOAD_VAD_PEAK_AVG_RATIO_MAX;
}

static esp_err_t send_heartbeat_if_due(int64_t *last_heartbeat_us)
{
  int64_t now = esp_timer_get_time();
  if (*last_heartbeat_us != 0 &&
      now - *last_heartbeat_us < (int64_t)VOICE_UPLOAD_HEARTBEAT_MS * 1000) {
    return ESP_OK;
  }

  ESP_RETURN_ON_ERROR(websocket_send_json_type("ping"), TAG, "send heartbeat failed");
  *last_heartbeat_us = now;
  return ESP_OK;
}

static esp_err_t record_and_upload(record_stop_mode_t stop_mode)
{
  s_continue_after_tts = false;
  voice_set_state(VOICE_STATE_RECORDING);
  lcd_show_user_speaking();
  s_accept_tts = false;
  esp_err_t ret = ESP_OK;
  bool start_sent = false;
  int32_t *raw = NULL;
  int16_t *pcm = NULL;
  int64_t last_heartbeat_us = 0;
  size_t total_pcm_bytes = 0;
  uint32_t recorded_chunks = 0;
  uint32_t silence_chunks = 0;
  bool speech_started = false;

  if (!wifi_is_connected()) {
    ESP_LOGW(TAG, "WiFi is not connected, ignore recording request");
    ret = ESP_ERR_INVALID_STATE;
    goto cleanup;
  }

  ESP_GOTO_ON_ERROR(websocket_ensure_connected(), cleanup, TAG, "WebSocket connect failed");
  ESP_GOTO_ON_ERROR(send_heartbeat_if_due(&(int64_t){0}), cleanup, TAG, "initial heartbeat failed");
  clear_pending_tts_audio();
  clear_pending_tts_texts();
  ESP_GOTO_ON_ERROR(websocket_send_json_type("start"), cleanup, TAG, "send start failed");
  start_sent = true;

  raw = (int32_t *)malloc(VOICE_UPLOAD_FRAMES_PER_CHUNK * sizeof(int32_t));
  pcm = (int16_t *)malloc(VOICE_UPLOAD_FRAMES_PER_CHUNK * sizeof(int16_t));
  if (raw == NULL || pcm == NULL) {
    ret = ESP_ERR_NO_MEM;
    goto cleanup;
  }

  ESP_LOGI(TAG, "recording started");
  last_heartbeat_us = esp_timer_get_time();

  while (s_ws_connected) {
    ESP_GOTO_ON_ERROR(send_heartbeat_if_due(&last_heartbeat_us), cleanup, TAG, "heartbeat failed");

    size_t samples = 0;
    ret = read_mic_pcm_chunk(raw, pcm, &samples, pdMS_TO_TICKS(100));
    if (ret == ESP_ERR_TIMEOUT) {
      ret = ESP_OK;
      continue;
    }
    ESP_GOTO_ON_ERROR(ret, cleanup, TAG, "read mic failed");

    if (stop_mode == RECORD_STOP_BY_SILENCE || stop_mode == RECORD_STOP_BY_KEY) {
      uint32_t avg_abs = 0;
      int16_t peak = 0;
      int32_t dc = 0;
      bool raw_has_voice = vad_detect_voice(pcm, samples, &avg_abs, &peak, &dc);
#if VOICE_UPLOAD_AFE_DEBUG_ENABLE
      vad_state_t afe_vad = voice_afe_last_vad_state();
      bool has_voice = voice_afe_vad_available() && afe_vad == VAD_SPEECH;
#else
      int afe_vad = -1;
      bool has_voice = false;
#endif
      bool playback_active = audio_is_playback_active();
      bool user_voice = has_voice && !playback_active;
      if (user_voice) {
        if (!speech_started) {
          ESP_LOGI(TAG, "record speech started: chunk=%lu", (unsigned long)recorded_chunks);
        }
        speech_started = true;
        silence_chunks = 0;
      } else if (speech_started) {
        ++silence_chunks;
      }

      if (recorded_chunks < 5 ||
          (!speech_started && recorded_chunks == VOICE_UPLOAD_VAD_FIRST_SPEECH_CHUNKS) ||
          silence_chunks == 1 ||
          (recorded_chunks % VOICE_UPLOAD_RECORD_VAD_LOG_INTERVAL) == 0U) {
        ESP_LOGI(TAG,
                 "record VAD: chunk=%lu afe_vad=%d voice=%d user_voice=%d playback=%d started=%d raw_voice=%d avg=%lu peak=%d ratio=%lu.%lu dc=%ld silence=%lu",
                 (unsigned long)recorded_chunks,
                 afe_vad,
                 has_voice,
                 user_voice,
                 playback_active,
                 speech_started,
                 raw_has_voice,
                 (unsigned long)avg_abs,
                 peak,
                 avg_abs > 0 ? (unsigned long)((uint32_t)peak / avg_abs) : 0UL,
                 avg_abs > 0 ? (unsigned long)(((uint32_t)peak % avg_abs) * 10U / avg_abs) : 0UL,
                 (long)dc,
                 (unsigned long)silence_chunks);
      }
    }

    ESP_GOTO_ON_ERROR(websocket_send_bin(pcm, samples), cleanup, TAG, "send PCM failed");
    total_pcm_bytes += samples * sizeof(int16_t);
    ++recorded_chunks;

    if (stop_mode == RECORD_STOP_BY_SILENCE || stop_mode == RECORD_STOP_BY_KEY) {
      if (!speech_started && recorded_chunks >= VOICE_UPLOAD_VAD_FIRST_SPEECH_CHUNKS) {
        ESP_LOGI(TAG,
                 "stop recording: no speech detected in %lu chunks",
                 (unsigned long)recorded_chunks);
        break;
      }
      if (speech_started && silence_chunks >= VOICE_UPLOAD_VAD_END_SILENCE_CHUNKS) {
        ESP_LOGI(TAG, "stop recording after silence: chunks=%lu", (unsigned long)silence_chunks);
        break;
      }
      if (recorded_chunks >= VOICE_UPLOAD_VAD_MAX_RECORD_CHUNKS) {
        ESP_LOGW(TAG, "stop recording at max chunks: %lu", (unsigned long)recorded_chunks);
        break;
      }
    }
  }

cleanup:
  if (start_sent) {
    esp_err_t end_ret = websocket_send_json_type("end");
    if (ret == ESP_OK && end_ret != ESP_OK) {
      ret = end_ret;
    }
  }
  clear_pending_tts_audio();
  clear_pending_tts_texts();
  s_accept_tts = true;

  ESP_LOGI(TAG,
           "recording ended: ret=%s pcm_bytes=%u chunks=%lu",
           esp_err_to_name(ret),
           (unsigned)total_pcm_bytes,
           (unsigned long)recorded_chunks);
  free(raw);
  free(pcm);
  if (ret == ESP_OK) {
    voice_set_state(VOICE_STATE_WAITING_RESPONSE);
    schedule_waiting_response_timeout();
  } else {
    voice_set_state(VOICE_STATE_IDLE);
  }
  return ret;
}

static esp_err_t listen_for_voice_start(int32_t *raw,
                                        int16_t *pcm,
                                        uint32_t *voice_chunks,
                                        uint32_t *idle_chunks,
                                        uint32_t *debug_cooldown_chunks,
                                        uint32_t *multinet_silence_chunks,
                                        uint32_t *human_voice_log_cooldown_chunks)
{
  if (raw == NULL || pcm == NULL || voice_chunks == NULL || idle_chunks == NULL ||
      debug_cooldown_chunks == NULL || multinet_silence_chunks == NULL ||
      human_voice_log_cooldown_chunks == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  size_t samples = 0;
  esp_err_t ret = read_mic_pcm_chunk(raw,
                                     pcm,
                                     &samples,
                                     pdMS_TO_TICKS(VOICE_UPLOAD_IDLE_READ_TIMEOUT_MS));
  if (ret == ESP_ERR_TIMEOUT) {
    return ESP_OK;
  }
  ESP_RETURN_ON_ERROR(ret, TAG, "read idle mic failed");

  uint32_t avg_abs = 0;
  int16_t peak = 0;
  int32_t dc = 0;
  bool raw_has_voice = vad_detect_voice(pcm, samples, &avg_abs, &peak, &dc);
#if VOICE_UPLOAD_AFE_DEBUG_ENABLE
  vad_state_t afe_vad = voice_afe_last_vad_state();
  bool has_voice = voice_afe_vad_available() && afe_vad == VAD_SPEECH;
#else
  int afe_vad = -1;
  bool has_voice = false;
#endif
  ++(*idle_chunks);
  if (*human_voice_log_cooldown_chunks > 0) {
    --(*human_voice_log_cooldown_chunks);
  }

  if (*debug_cooldown_chunks > 0) {
    --(*debug_cooldown_chunks);
    *voice_chunks = 0;
    *multinet_silence_chunks = 0;
    voice_multinet_reset();
    if (!has_voice && s_voice_state == VOICE_STATE_VAD_ACTIVE) {
      voice_set_state(VOICE_STATE_IDLE);
    }
    return ESP_OK;
  }

  bool wake_detected = false;
  if (has_voice) {
    ++(*voice_chunks);
    *multinet_silence_chunks = 0;
    voice_set_state(VOICE_STATE_VAD_ACTIVE);
    if (*human_voice_log_cooldown_chunks == 0) {
      ESP_LOGI(TAG,
               "human voice detected: afe_vad=%d raw_voice=%d avg=%lu peak=%d ratio=%lu.%lu dc=%ld",
               afe_vad,
               raw_has_voice,
               (unsigned long)avg_abs,
               peak,
               avg_abs > 0 ? (unsigned long)((uint32_t)peak / avg_abs) : 0UL,
               avg_abs > 0 ? (unsigned long)(((uint32_t)peak % avg_abs) * 10U / avg_abs) : 0UL,
               (long)dc);
      *human_voice_log_cooldown_chunks = VOICE_UPLOAD_HUMAN_VOICE_LOG_COOLDOWN_CHUNKS;
    }
    wake_detected = voice_multinet_feed(pcm, samples);
  } else {
    if (*voice_chunks > 0 || s_mn_pending_samples > 0) {
      ++(*multinet_silence_chunks);
    }
    if (*multinet_silence_chunks >= VOICE_UPLOAD_MULTINET_RESET_SILENCE_CHUNKS) {
      voice_multinet_reset();
      *voice_chunks = 0;
      *multinet_silence_chunks = 0;
      *human_voice_log_cooldown_chunks = 0;
    }
    if (*voice_chunks == 0 && s_voice_state == VOICE_STATE_VAD_ACTIVE) {
      voice_set_state(VOICE_STATE_IDLE);
    }
  }

  if (*voice_chunks < VOICE_UPLOAD_VAD_START_CHUNKS) {
    return ESP_OK;
  }

  if (!wake_detected) {
    return ESP_OK;
  }

#if VOICE_UPLOAD_VAD_DEBUG_ONLY
  *voice_chunks = 0;
  *multinet_silence_chunks = 0;
  *debug_cooldown_chunks = VOICE_UPLOAD_VAD_DEBUG_COOLDOWN_CHUNKS;
  voice_set_state(VOICE_STATE_IDLE);
  return ESP_OK;
#endif

  *voice_chunks = 0;
  *multinet_silence_chunks = 0;

  if (!s_ws_connected) {
    ESP_LOGW(TAG, "wake phrase ignored because WebSocket is not connected");
    audio_stop_playback();
    voice_play_wav_prompt(VOICE_UPLOAD_PROMPT_NET_ERROR_AND_TRY, false);
    voice_set_state(VOICE_STATE_IDLE);
    return ESP_OK;
  }

  ESP_LOGI(TAG, "begin auto record after wake phrase");
  clear_pending_tts_audio();
  clear_pending_tts_texts();
  audio_stop_playback();

  voice_set_state(VOICE_STATE_SPEAKING);
  esp_err_t prompt_ret = voice_play_wav_prompt(VOICE_UPLOAD_PROMPT_IM_READY, true);
  if (prompt_ret != ESP_OK) {
    ESP_LOGW(TAG, "ready prompt failed, continue recording: %s", esp_err_to_name(prompt_ret));
  }

  ret = record_and_upload(RECORD_STOP_BY_SILENCE);
  if (ret != ESP_OK) {
    ESP_LOGW(TAG, "auto record/upload failed: %s", esp_err_to_name(ret));
    voice_set_state(VOICE_STATE_IDLE);
  }
  return ret;
}

static void handle_button_record(void)
{
  s_idle_vad_paused = true;
  voice_multinet_reset();
  esp_err_t err = ESP_OK;

  ESP_LOGI(TAG,
           "button record requested: pressed=%d ws_connected=%d state=%d",
           key_is_pressed(),
           s_ws_connected,
           s_voice_state);

  while (key_is_pressed()) {
    vTaskDelay(pdMS_TO_TICKS(20));
  }

  key_event_t stale_event = 0;
  while (key_wait_event(&stale_event, 0)) {
  }

  s_accept_tts = false;
  clear_pending_tts_audio();
  clear_pending_tts_texts();
  audio_stop_playback();

  if (!s_ws_connected) {
    ESP_LOGW(TAG, "button record ignored because WebSocket is not connected");
    voice_play_wav_prompt(VOICE_UPLOAD_PROMPT_NET_ERROR_AND_TRY, false);
    voice_set_state(VOICE_STATE_IDLE);
    goto cleanup;
  }

  ESP_LOGI(TAG, "button released, start recording");
  err = record_and_upload(RECORD_STOP_BY_SILENCE);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "record/upload failed: %s", esp_err_to_name(err));
    voice_set_state(VOICE_STATE_IDLE);
    vTaskDelay(pdMS_TO_TICKS(VOICE_UPLOAD_RECONNECT_DELAY_MS));
  }

cleanup:
  s_accept_tts = true;
  s_idle_vad_paused = false;
}

static void voice_upload_task(void *arg)
{
  (void)arg;

  while (true) {
    if (key_is_pressed()) {
      handle_button_record();
      continue;
    }

    key_event_t event = 0;
    if (!key_wait_event(&event, pdMS_TO_TICKS(1000))) {
      continue;
    }

    if (event != KEY_EVENT_PRESSED) {
      continue;
    }

    handle_button_record();
  }
}

static void voice_idle_vad_task(void *arg)
{
  (void)arg;
  int64_t idle_last_heartbeat_us = 0;
  uint32_t idle_voice_chunks = 0;
  uint32_t idle_vad_chunks = 0;
  uint32_t idle_debug_cooldown_chunks = 0;
  uint32_t idle_multinet_silence_chunks = 0;
  uint32_t idle_human_voice_log_cooldown_chunks = 0;
  int32_t *idle_raw = (int32_t *)malloc(VOICE_UPLOAD_FRAMES_PER_CHUNK * sizeof(int32_t));
  int16_t *idle_pcm = (int16_t *)malloc(VOICE_UPLOAD_FRAMES_PER_CHUNK * sizeof(int16_t));
  if (idle_raw == NULL || idle_pcm == NULL) {
    ESP_LOGE(TAG, "allocate idle VAD buffers failed");
    free(idle_raw);
    free(idle_pcm);
    vTaskDelete(NULL);
    return;
  }

  while (true) {
    if (s_idle_vad_paused || key_is_pressed()) {
      idle_voice_chunks = 0;
      idle_vad_chunks = 0;
      idle_multinet_silence_chunks = 0;
      idle_human_voice_log_cooldown_chunks = 0;
      voice_multinet_reset();
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }

    if (s_ws_connected) {
      esp_err_t err = send_heartbeat_if_due(&idle_last_heartbeat_us);
      if (err != ESP_OK) {
        ESP_LOGW(TAG, "idle heartbeat failed: %s", esp_err_to_name(err));
      }
    }

    if (audio_is_playback_active()) {
      voice_set_state(VOICE_STATE_SPEAKING);
      idle_voice_chunks = 0;
      idle_vad_chunks = 0;
      idle_multinet_silence_chunks = 0;
      idle_human_voice_log_cooldown_chunks = 0;
      voice_multinet_reset();
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    } else if (s_voice_state == VOICE_STATE_SPEAKING) {
      voice_set_state(VOICE_STATE_IDLE);
    }

    if (s_continue_after_tts && s_voice_state == VOICE_STATE_IDLE && s_ws_connected) {
      s_continue_after_tts = false;
      idle_voice_chunks = 0;
      idle_vad_chunks = 0;
      idle_multinet_silence_chunks = 0;
      idle_human_voice_log_cooldown_chunks = 0;
      voice_multinet_reset();
      ESP_LOGI(TAG, "TTS playback finished, start continuation recording");
      esp_err_t err = record_and_upload(RECORD_STOP_BY_SILENCE);
      if (err != ESP_OK) {
        ESP_LOGW(TAG, "continuation record/upload failed: %s", esp_err_to_name(err));
        voice_set_state(VOICE_STATE_IDLE);
        vTaskDelay(pdMS_TO_TICKS(VOICE_UPLOAD_RECONNECT_DELAY_MS));
      }
      continue;
    }

    if (s_voice_state == VOICE_STATE_RECORDING) {
      idle_voice_chunks = 0;
      idle_vad_chunks = 0;
      idle_multinet_silence_chunks = 0;
      idle_human_voice_log_cooldown_chunks = 0;
      voice_multinet_reset();
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }

    if (s_voice_state != VOICE_STATE_IDLE && s_voice_state != VOICE_STATE_VAD_ACTIVE) {
      voice_set_state(VOICE_STATE_IDLE);
    }

    esp_err_t err = listen_for_voice_start(idle_raw,
                                           idle_pcm,
                                           &idle_voice_chunks,
                                           &idle_vad_chunks,
                                           &idle_debug_cooldown_chunks,
                                           &idle_multinet_silence_chunks,
                                           &idle_human_voice_log_cooldown_chunks);
    if (err != ESP_OK) {
      vTaskDelay(pdMS_TO_TICKS(VOICE_UPLOAD_RECONNECT_DELAY_MS));
    }
  }
}

esp_err_t voice_upload_init(void)
{
  if (s_initialized) {
    return ESP_OK;
  }

  ESP_RETURN_ON_ERROR(key_init(), TAG, "key init failed");
  ESP_RETURN_ON_ERROR(voice_i2s_init(), TAG, "voice I2S init failed");
  s_ws_mutex = xSemaphoreCreateMutex();
  ESP_RETURN_ON_FALSE(s_ws_mutex != NULL, ESP_ERR_NO_MEM, TAG, "create WebSocket mutex failed");
  voice_log_model_partition_probe();
  esp_err_t sr_ret = voice_srmodel_init();
  if (sr_ret != ESP_OK) {
    ESP_LOGW(TAG, "SR model list disabled: %s", esp_err_to_name(sr_ret));
  }
#if VOICE_UPLOAD_AFE_DEBUG_ENABLE
  esp_err_t afe_ret = voice_afe_debug_init();
  if (afe_ret != ESP_OK) {
    ESP_LOGW(TAG, "AFE debug disabled: %s", esp_err_to_name(afe_ret));
  }
#endif
  esp_err_t mn_ret = voice_multinet_init();
  if (mn_ret != ESP_OK) {
    s_mn_init_attempted = true;
    ESP_LOGW(TAG, "MultiNet wake disabled: %s", esp_err_to_name(mn_ret));
  }

  BaseType_t ret = xTaskCreate(voice_upload_task,
                               "voice_upload",
                               VOICE_UPLOAD_TASK_STACK,
                               NULL,
                               VOICE_UPLOAD_TASK_PRIORITY,
                               &s_upload_task_handle);
  ESP_RETURN_ON_FALSE(ret == pdPASS, ESP_ERR_NO_MEM, TAG, "create voice upload task failed");

  ret = xTaskCreate(voice_idle_vad_task,
                    "voice_idle_vad",
                    VOICE_UPLOAD_TASK_STACK,
                    NULL,
                    VOICE_UPLOAD_TASK_PRIORITY - 1,
                    &s_idle_vad_task_handle);
  ESP_RETURN_ON_FALSE(ret == pdPASS, ESP_ERR_NO_MEM, TAG, "create idle VAD task failed");

  ret = xTaskCreate(voice_ws_monitor_task,
                    "voice_ws_monitor",
                    VOICE_UPLOAD_WS_MONITOR_TASK_STACK,
                    NULL,
                    VOICE_UPLOAD_TASK_PRIORITY - 1,
                    &s_ws_monitor_task_handle);
  ESP_RETURN_ON_FALSE(ret == pdPASS, ESP_ERR_NO_MEM, TAG, "create WebSocket monitor task failed");

  s_initialized = true;
  ESP_LOGI(TAG, "voice upload ready: ws=%s", VOICE_UPLOAD_WS_URI);
  return ESP_OK;
}
