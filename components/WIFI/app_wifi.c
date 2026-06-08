#include "app_wifi.h"

#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "app_wifi";

#define APP_WIFI_NVS_NAMESPACE "wifi"
#define APP_WIFI_NVS_KEY_SSID "ssid"
#define APP_WIFI_NVS_KEY_PASSWORD "password"
#define APP_WIFI_PASSWORD_MAX_LEN 64
#define APP_WIFI_RETRY_DELAY_MS (60 * 1000)
#define APP_WIFI_MAX_RETRIES 5

static SemaphoreHandle_t s_status_mutex;
static esp_netif_t *s_sta_netif;
static bool s_initialized;
static app_wifi_status_t s_status;
static char s_saved_ssid[APP_WIFI_SSID_MAX_LEN + 1];
static char s_saved_password[APP_WIFI_PASSWORD_MAX_LEN + 1];
static char s_pending_ssid[APP_WIFI_SSID_MAX_LEN + 1];
static char s_pending_password[APP_WIFI_PASSWORD_MAX_LEN + 1];
static bool s_pending_save;
static bool s_suppress_next_disconnect_retry;
static uint8_t s_retry_count;
static TaskHandle_t s_retry_task_handle;
static app_wifi_status_changed_cb_t s_status_changed_cb;
static void *s_status_changed_user_ctx;

static void schedule_retry(void);
static void notify_status_changed(void);

static void status_lock(void)
{
  if (s_status_mutex != NULL) {
    xSemaphoreTake(s_status_mutex, portMAX_DELAY);
  }
}

static void status_unlock(void)
{
  if (s_status_mutex != NULL) {
    xSemaphoreGive(s_status_mutex);
  }
}

static void notify_status_changed(void)
{
  app_wifi_status_changed_cb_t cb = s_status_changed_cb;
  if (cb != NULL) {
    cb(s_status_changed_user_ctx);
  }
}

static esp_err_t init_nvs(void)
{
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "erase NVS failed");
    err = nvs_flash_init();
  }
  return err;
}

static esp_err_t load_saved_credentials(void)
{
  nvs_handle_t nvs_handle;
  esp_err_t err = nvs_open(APP_WIFI_NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
  if (err == ESP_ERR_NVS_NOT_FOUND) {
    return ESP_OK;
  }
  ESP_RETURN_ON_ERROR(err, TAG, "open WiFi NVS failed");

  size_t ssid_len = sizeof(s_saved_ssid);
  err = nvs_get_str(nvs_handle, APP_WIFI_NVS_KEY_SSID, s_saved_ssid, &ssid_len);
  if (err == ESP_ERR_NVS_NOT_FOUND) {
    s_saved_ssid[0] = '\0';
    nvs_close(nvs_handle);
    return ESP_OK;
  }
  if (err != ESP_OK) {
    nvs_close(nvs_handle);
    ESP_RETURN_ON_ERROR(err, TAG, "read saved SSID failed");
  }

  size_t password_len = sizeof(s_saved_password);
  err = nvs_get_str(nvs_handle, APP_WIFI_NVS_KEY_PASSWORD, s_saved_password, &password_len);
  if (err == ESP_ERR_NVS_NOT_FOUND) {
    s_saved_password[0] = '\0';
    err = ESP_OK;
  }
  nvs_close(nvs_handle);

  ESP_RETURN_ON_ERROR(err, TAG, "read saved password failed");
  ESP_LOGI(TAG, "loaded saved WiFi SSID: %s", s_saved_ssid);
  return ESP_OK;
}

static esp_err_t save_credentials(const char *ssid, const char *password)
{
  nvs_handle_t nvs_handle;
  ESP_RETURN_ON_ERROR(nvs_open(APP_WIFI_NVS_NAMESPACE, NVS_READWRITE, &nvs_handle),
                      TAG,
                      "open WiFi NVS failed");

  esp_err_t err = nvs_set_str(nvs_handle, APP_WIFI_NVS_KEY_SSID, ssid);
  if (err == ESP_OK) {
    err = nvs_set_str(nvs_handle, APP_WIFI_NVS_KEY_PASSWORD, password != NULL ? password : "");
  }
  if (err == ESP_OK) {
    err = nvs_commit(nvs_handle);
  }
  nvs_close(nvs_handle);

  ESP_RETURN_ON_ERROR(err, TAG, "save WiFi credentials failed");
  strlcpy(s_saved_ssid, ssid, sizeof(s_saved_ssid));
  strlcpy(s_saved_password, password != NULL ? password : "", sizeof(s_saved_password));
  ESP_LOGI(TAG, "saved WiFi credentials for SSID: %s", s_saved_ssid);
  return ESP_OK;
}

static esp_err_t connect_with_credentials(const char *ssid,
                                          const char *password,
                                          bool save_on_success,
                                          bool suppress_disconnect_retry)
{
  wifi_config_t wifi_config = {0};
  strlcpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid));
  if (password != NULL) {
    strlcpy((char *)wifi_config.sta.password, password, sizeof(wifi_config.sta.password));
  }
  wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
  wifi_config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;

  status_lock();
  s_status.connected = false;
  s_status.connecting = true;
  s_status.rssi = -127;
  strlcpy(s_status.ssid, ssid, sizeof(s_status.ssid));
  memset(&s_status.ip_info, 0, sizeof(s_status.ip_info));
  status_unlock();
  notify_status_changed();

  if (save_on_success) {
    strlcpy(s_pending_ssid, ssid, sizeof(s_pending_ssid));
    strlcpy(s_pending_password, password != NULL ? password : "", sizeof(s_pending_password));
    s_pending_save = true;
  } else {
    s_pending_save = false;
  }

  s_suppress_next_disconnect_retry = suppress_disconnect_retry;
  esp_err_t err = esp_wifi_disconnect();
  if (err != ESP_OK) {
    s_suppress_next_disconnect_retry = false;
  }
  if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_CONNECT) {
    ESP_LOGW(TAG, "disconnect before connect failed: %s", esp_err_to_name(err));
  }

  ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wifi_config), TAG, "set WiFi config failed");
  ESP_RETURN_ON_ERROR(esp_wifi_connect(), TAG, "connect WiFi failed");
  return ESP_OK;
}

static void retry_task(void *arg)
{
  (void)arg;

  vTaskDelay(pdMS_TO_TICKS(APP_WIFI_RETRY_DELAY_MS));

  s_retry_task_handle = NULL;

  if (s_saved_ssid[0] != '\0' && s_retry_count < APP_WIFI_MAX_RETRIES) {
    s_retry_count++;
    ESP_LOGI(TAG,
             "retry WiFi connection %u/%u: %s",
             s_retry_count,
             APP_WIFI_MAX_RETRIES,
             s_saved_ssid);
    esp_err_t err = connect_with_credentials(s_saved_ssid, s_saved_password, false, true);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "retry connect failed: %s", esp_err_to_name(err));
      schedule_retry();
    }
  }

  vTaskDelete(NULL);
}

static void schedule_retry(void)
{
  if (s_saved_ssid[0] == '\0') {
    return;
  }
  if (s_retry_count >= APP_WIFI_MAX_RETRIES) {
    ESP_LOGW(TAG, "WiFi retry limit reached, stop reconnecting");
    return;
  }
  if (s_retry_task_handle != NULL) {
    return;
  }

  BaseType_t ret = xTaskCreate(retry_task, "wifi_retry", 4096, NULL, 4, &s_retry_task_handle);
  if (ret != pdPASS) {
    s_retry_task_handle = NULL;
    ESP_LOGE(TAG, "create WiFi retry task failed");
  }
}

static void wifi_event_handler(void *arg,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data)
{
  (void)arg;

  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
    status_lock();
    s_status.connected = false;
    s_status.connecting = false;
    s_status.rssi = -127;
    memset(&s_status.ip_info, 0, sizeof(s_status.ip_info));
    status_unlock();
    notify_status_changed();

    wifi_event_sta_disconnected_t *event = (wifi_event_sta_disconnected_t *)event_data;
    ESP_LOGW(TAG, "WiFi disconnected, reason=%d", event != NULL ? event->reason : -1);

    if (s_suppress_next_disconnect_retry) {
      s_suppress_next_disconnect_retry = false;
    } else {
      schedule_retry();
    }
  } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;

    status_lock();
    s_status.connected = true;
    s_status.connecting = false;
    s_status.rssi = -127;
    if (event != NULL) {
      s_status.ip_info = event->ip_info;
    }
    status_unlock();
    notify_status_changed();

    ESP_LOGI(TAG, "got IP: " IPSTR, IP2STR(&event->ip_info.ip));
    s_suppress_next_disconnect_retry = false;
    s_retry_count = 0;

    if (s_pending_save) {
      esp_err_t err = save_credentials(s_pending_ssid, s_pending_password);
      if (err != ESP_OK) {
        ESP_LOGE(TAG, "save connected WiFi credentials failed: %s", esp_err_to_name(err));
      }
      s_pending_save = false;
    }
  }
}

esp_err_t app_wifi_init(void)
{
  if (s_initialized) {
    return ESP_OK;
  }

  s_status_mutex = xSemaphoreCreateMutex();
  ESP_RETURN_ON_FALSE(s_status_mutex != NULL, ESP_ERR_NO_MEM, TAG, "create status mutex failed");

  ESP_RETURN_ON_ERROR(init_nvs(), TAG, "NVS init failed");
  ESP_RETURN_ON_ERROR(load_saved_credentials(), TAG, "load saved WiFi credentials failed");

  esp_err_t err = esp_netif_init();
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    ESP_RETURN_ON_ERROR(err, TAG, "esp_netif init failed");
  }

  err = esp_event_loop_create_default();
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    ESP_RETURN_ON_ERROR(err, TAG, "event loop init failed");
  }

  s_sta_netif = esp_netif_create_default_wifi_sta();
  ESP_RETURN_ON_FALSE(s_sta_netif != NULL, ESP_FAIL, TAG, "create default STA netif failed");

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "WiFi init failed");
  ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(WIFI_EVENT,
                                                          ESP_EVENT_ANY_ID,
                                                          wifi_event_handler,
                                                          NULL,
                                                          NULL),
                      TAG,
                      "register WiFi event handler failed");
  ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(IP_EVENT,
                                                          IP_EVENT_STA_GOT_IP,
                                                          wifi_event_handler,
                                                          NULL,
                                                          NULL),
                      TAG,
                      "register IP event handler failed");

  ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "set WiFi STA mode failed");
  ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_FLASH), TAG, "set WiFi storage failed");
  ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "start WiFi failed");

  s_initialized = true;
  if (s_saved_ssid[0] != '\0') {
    ESP_LOGI(TAG, "auto connect saved WiFi: %s", s_saved_ssid);
    ESP_RETURN_ON_ERROR(connect_with_credentials(s_saved_ssid, s_saved_password, false, false),
                        TAG,
                        "auto connect saved WiFi failed");
  }
  return ESP_OK;
}

esp_err_t app_wifi_scan(app_wifi_ap_record_t *aps, size_t max_aps, size_t *ap_count)
{
  ESP_RETURN_ON_FALSE(aps != NULL && ap_count != NULL, ESP_ERR_INVALID_ARG, TAG, "bad scan args");
  ESP_RETURN_ON_ERROR(app_wifi_init(), TAG, "WiFi init failed");

  *ap_count = 0;
  wifi_scan_config_t scan_config = {
    .ssid = NULL,
    .bssid = NULL,
    .channel = 0,
    .show_hidden = false,
  };

  ESP_RETURN_ON_ERROR(esp_wifi_scan_start(&scan_config, true), TAG, "scan failed");

  uint16_t found = 0;
  ESP_RETURN_ON_ERROR(esp_wifi_scan_get_ap_num(&found), TAG, "get AP count failed");
  if (found == 0 || max_aps == 0) {
    return ESP_OK;
  }

  uint16_t read_count = found > max_aps ? (uint16_t)max_aps : found;
  wifi_ap_record_t records[APP_WIFI_MAX_APS] = {0};
  if (read_count > APP_WIFI_MAX_APS) {
    read_count = APP_WIFI_MAX_APS;
  }
  ESP_RETURN_ON_ERROR(esp_wifi_scan_get_ap_records(&read_count, records), TAG, "get AP records failed");

  for (uint16_t i = 0; i < read_count; ++i) {
    strlcpy(aps[i].ssid, (const char *)records[i].ssid, sizeof(aps[i].ssid));
    aps[i].rssi = records[i].rssi;
    aps[i].authmode = records[i].authmode;
  }
  *ap_count = read_count;
  return ESP_OK;
}

esp_err_t app_wifi_connect(const char *ssid, const char *password)
{
  ESP_RETURN_ON_FALSE(ssid != NULL && ssid[0] != '\0', ESP_ERR_INVALID_ARG, TAG, "SSID is empty");
  ESP_RETURN_ON_ERROR(app_wifi_init(), TAG, "WiFi init failed");

  s_retry_count = 0;
  return connect_with_credentials(ssid, password, true, true);
}

void app_wifi_get_status(app_wifi_status_t *status)
{
  if (status == NULL) {
    return;
  }

  status_lock();
  *status = s_status;
  status_unlock();

  if (status->connected) {
    wifi_ap_record_t ap_info = {0};
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
      status->rssi = ap_info.rssi;
    } else {
      status->rssi = -127;
    }
  }
}

void app_wifi_set_status_changed_cb(app_wifi_status_changed_cb_t cb, void *user_ctx)
{
  s_status_changed_cb = cb;
  s_status_changed_user_ctx = user_ctx;
}
