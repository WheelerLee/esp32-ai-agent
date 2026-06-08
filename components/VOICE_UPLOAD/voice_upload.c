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
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "key.h"
#include "lcd.h"

static const char *TAG = "voice_upload";

#define VOICE_UPLOAD_TASK_STACK 8192
#define VOICE_UPLOAD_TASK_PRIORITY 5
#define VOICE_UPLOAD_HEARTBEAT_MS 30000
#define VOICE_UPLOAD_CONNECT_TIMEOUT_MS 10000
#define VOICE_UPLOAD_SEND_TIMEOUT_TICKS pdMS_TO_TICKS(3000)
#define VOICE_UPLOAD_RECONNECT_DELAY_MS 1000
#define VOICE_UPLOAD_FRAMES_PER_CHUNK 512
#define VOICE_UPLOAD_MIC_SHIFT 14
#define VOICE_UPLOAD_WS_RX_BUFFER_BYTES 4096
#define VOICE_UPLOAD_TTS_TEXT_SLOTS 4

static i2s_chan_handle_t s_i2s_rx_chan;
static esp_websocket_client_handle_t s_ws_client;
static TaskHandle_t s_upload_task_handle;
static volatile bool s_ws_connected;
static bool s_ws_started;
static bool s_initialized;

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

static tts_audio_rx_t s_tts_audio_rx;
static tts_text_slot_t s_tts_text_slots[VOICE_UPLOAD_TTS_TEXT_SLOTS];

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
  } else if (strcmp(type->valuestring, "error") == 0) {
    const cJSON *stage = cJSON_GetObjectItem(root, "stage");
    const cJSON *message = cJSON_GetObjectItem(root, "message");
    ESP_LOGE(TAG,
             "server error: stage=%s message=%s",
             cJSON_IsString(stage) ? stage->valuestring : "?",
             cJSON_IsString(message) ? message->valuestring : "?");
  } else {
    ESP_LOGI(TAG, "server: %.*s", len, json);
  }

  cJSON_Delete(root);
  free(json_copy);
}

static void handle_server_binary(const uint8_t *data, int len)
{
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
  ESP_LOGI(TAG,
           "TTS PCM complete: response=%d speech=%d chunk=%d bytes=%u receive=%lld ms",
           s_tts_audio_rx.response_id,
           s_tts_audio_rx.speech_index,
           s_tts_audio_rx.chunk_index,
           (unsigned)s_tts_audio_rx.expected_bytes,
           (long long)(receive_us / 1000));

  esp_err_t err = audio_queue_pcm_s16le_with_text(s_tts_audio_rx.data,
                                                  s_tts_audio_rx.expected_bytes,
                                                  s_tts_audio_rx.sample_rate_hz,
                                                  s_tts_audio_rx.channels,
                                                  s_tts_audio_rx.text);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "queue TTS PCM failed: %s", esp_err_to_name(err));
  }

  clear_pending_tts_audio();
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
    clear_pending_tts_audio();
    clear_pending_tts_texts();
    ESP_LOGW(TAG, "WebSocket disconnected");
    break;
  case WEBSOCKET_EVENT_DATA:
    if (data == NULL || data->data_len <= 0) {
      break;
    }
    if (data->op_code == WS_TRANSPORT_OPCODES_TEXT) {
      handle_server_json((const char *)data->data_ptr, data->data_len);
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

static bool wifi_is_connected(void)
{
  app_wifi_status_t status = {0};
  app_wifi_get_status(&status);
  return status.connected;
}

static esp_err_t websocket_ensure_connected(void)
{
  if (s_ws_client != NULL && s_ws_connected) {
    return ESP_OK;
  }

  if (s_ws_client == NULL) {
    esp_websocket_client_config_t ws_cfg = {
      .uri = VOICE_UPLOAD_WS_URI,
      .buffer_size = VOICE_UPLOAD_WS_RX_BUFFER_BYTES,
      .network_timeout_ms = 5000,
      .reconnect_timeout_ms = 3000,
    };

    s_ws_client = esp_websocket_client_init(&ws_cfg);
    ESP_RETURN_ON_FALSE(s_ws_client != NULL, ESP_ERR_NO_MEM, TAG, "create WebSocket client failed");
    ESP_RETURN_ON_ERROR(esp_websocket_register_events(s_ws_client,
                                                      WEBSOCKET_EVENT_ANY,
                                                      websocket_event_handler,
                                                      NULL),
                        TAG,
                        "register WebSocket events failed");
  }

  if (!s_ws_started) {
    s_ws_connected = false;
    ESP_LOGI(TAG, "connecting WebSocket: %s", VOICE_UPLOAD_WS_URI);
    ESP_RETURN_ON_ERROR(esp_websocket_client_start(s_ws_client), TAG, "start WebSocket client failed");
    s_ws_started = true;
  }

  int64_t deadline = esp_timer_get_time() + (int64_t)VOICE_UPLOAD_CONNECT_TIMEOUT_MS * 1000;
  while (!s_ws_connected && esp_timer_get_time() < deadline) {
    vTaskDelay(pdMS_TO_TICKS(50));
  }

  if (s_ws_connected) {
    return ESP_OK;
  }

  ESP_LOGW(TAG, "WebSocket connect timeout, restart client next time");
  esp_websocket_client_stop(s_ws_client);
  s_ws_started = false;
  return ESP_ERR_TIMEOUT;
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

static esp_err_t record_and_upload(void)
{
  lcd_show_user_speaking();

  if (!wifi_is_connected()) {
    ESP_LOGW(TAG, "WiFi is not connected, ignore recording request");
    return ESP_ERR_INVALID_STATE;
  }

  ESP_RETURN_ON_ERROR(websocket_ensure_connected(), TAG, "WebSocket connect failed");
  ESP_RETURN_ON_ERROR(send_heartbeat_if_due(&(int64_t){0}), TAG, "initial heartbeat failed");
  ESP_RETURN_ON_ERROR(websocket_send_json_type("start"), TAG, "send start failed");

  int32_t *raw = (int32_t *)malloc(VOICE_UPLOAD_FRAMES_PER_CHUNK * sizeof(int32_t));
  int16_t *pcm = (int16_t *)malloc(VOICE_UPLOAD_FRAMES_PER_CHUNK * sizeof(int16_t));
  if (raw == NULL || pcm == NULL) {
    free(raw);
    free(pcm);
    websocket_send_json_type("end");
    return ESP_ERR_NO_MEM;
  }

  ESP_LOGI(TAG, "recording started");
  esp_err_t ret = ESP_OK;
  int64_t last_heartbeat_us = esp_timer_get_time();
  size_t total_pcm_bytes = 0;

  while (key_is_pressed() && s_ws_connected) {
    ESP_GOTO_ON_ERROR(send_heartbeat_if_due(&last_heartbeat_us), cleanup, TAG, "heartbeat failed");

    size_t bytes_read = 0;
    ret = i2s_channel_read(s_i2s_rx_chan,
                           raw,
                           VOICE_UPLOAD_FRAMES_PER_CHUNK * sizeof(int32_t),
                           &bytes_read,
                           pdMS_TO_TICKS(100));
    if (ret == ESP_ERR_TIMEOUT || bytes_read == 0) {
      ret = ESP_OK;
      continue;
    }
    ESP_GOTO_ON_ERROR(ret, cleanup, TAG, "read INMP441 failed");

    size_t samples = bytes_read / sizeof(int32_t);
    for (size_t i = 0; i < samples; ++i) {
      pcm[i] = convert_mic_sample(raw[i]);
    }

    ESP_GOTO_ON_ERROR(websocket_send_bin(pcm, samples), cleanup, TAG, "send PCM failed");
    total_pcm_bytes += samples * sizeof(int16_t);
  }

cleanup:
  esp_err_t end_ret = websocket_send_json_type("end");
  if (ret == ESP_OK && end_ret != ESP_OK) {
    ret = end_ret;
  }

  ESP_LOGI(TAG,
           "recording ended: ret=%s pcm_bytes=%u",
           esp_err_to_name(ret),
           (unsigned)total_pcm_bytes);
  free(raw);
  free(pcm);
  return ret;
}

static void voice_upload_task(void *arg)
{
  (void)arg;
  int64_t idle_last_heartbeat_us = 0;

  while (true) {
    key_event_t event = 0;
    if (!key_wait_event(&event, pdMS_TO_TICKS(1000))) {
      if (s_ws_connected) {
        esp_err_t err = send_heartbeat_if_due(&idle_last_heartbeat_us);
        if (err != ESP_OK) {
          ESP_LOGW(TAG, "idle heartbeat failed: %s", esp_err_to_name(err));
        }
      }
      continue;
    }

    if (event != KEY_EVENT_PRESSED) {
      continue;
    }

    esp_err_t err = record_and_upload();
    if (err != ESP_OK) {
      ESP_LOGW(TAG, "record/upload failed: %s", esp_err_to_name(err));
      vTaskDelay(pdMS_TO_TICKS(VOICE_UPLOAD_RECONNECT_DELAY_MS));
    }

    while (key_is_pressed()) {
      vTaskDelay(pdMS_TO_TICKS(20));
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

  BaseType_t ret = xTaskCreate(voice_upload_task,
                               "voice_upload",
                               VOICE_UPLOAD_TASK_STACK,
                               NULL,
                               VOICE_UPLOAD_TASK_PRIORITY,
                               &s_upload_task_handle);
  ESP_RETURN_ON_FALSE(ret == pdPASS, ESP_ERR_NO_MEM, TAG, "create voice upload task failed");

  s_initialized = true;
  ESP_LOGI(TAG, "voice upload ready: ws=%s", VOICE_UPLOAD_WS_URI);
  return ESP_OK;
}
