#include "app_wifi.h"

#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "app_wifi";

#define APP_WIFI_NVS_NAMESPACE "app_wifi"
#define APP_WIFI_LEGACY_NVS_NAMESPACE "wifi"
#define APP_WIFI_NVS_KEY_SSID "ssid"
#define APP_WIFI_NVS_KEY_PASSWORD "password"
#define APP_WIFI_PASSWORD_MAX_LEN 64
#define APP_WIFI_RETRY_DELAY_MS (3 * 1000)
#define APP_WIFI_MAX_RETRIES 5
#define APP_WIFI_SCAN_RECORDS_MAX 32

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
static esp_timer_handle_t s_retry_timer;
static app_wifi_status_changed_cb_t s_status_changed_cb;
static void *s_status_changed_user_ctx;

static void schedule_retry(void);
static void notify_status_changed(void);
static void retry_timer_cb(void *arg);

// 加锁保护共享 WiFi 状态；初始化早期没有 mutex 时直接跳过。
static void status_lock(void)
{
  if (s_status_mutex != NULL) {
    xSemaphoreTake(s_status_mutex, portMAX_DELAY);
  }
}

// 释放共享 WiFi 状态锁。
static void status_unlock(void)
{
  if (s_status_mutex != NULL) {
    xSemaphoreGive(s_status_mutex);
  }
}

// 通知 UI 或其他订阅者 WiFi 状态已经变化。
static void notify_status_changed(void)
{
  app_wifi_status_changed_cb_t cb = s_status_changed_cb;
  if (cb != NULL) {
    cb(s_status_changed_user_ctx);
  }
}

// 初始化 NVS；遇到旧版本或空间不足时按 ESP-IDF 要求擦除后重试。
static esp_err_t init_nvs(void)
{
  esp_err_t err = nvs_flash_init();
  // NVS 分区格式不兼容时必须擦除，否则后续读写都会失败。
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "erase NVS failed");
    err = nvs_flash_init();
  }
  return err;
}

// 从指定 NVS 命名空间读取 WiFi 凭据，可选择忽略旧格式错误。
static esp_err_t load_credentials_from_namespace(const char *namespace_name, bool ignore_schema_error)
{
  nvs_handle_t nvs_handle;
  esp_err_t err = nvs_open(namespace_name, NVS_READONLY, &nvs_handle);
  if (err == ESP_ERR_NVS_NOT_FOUND) {
    return ESP_OK;
  }
  ESP_RETURN_ON_ERROR(err, TAG, "open WiFi NVS failed");

  size_t ssid_len = sizeof(s_saved_ssid);
  err = nvs_get_str(nvs_handle, APP_WIFI_NVS_KEY_SSID, s_saved_ssid, &ssid_len);
  if (err == ESP_ERR_NVS_NOT_FOUND) {
    // 没有 SSID 表示没有可自动连接的网络，不属于错误。
    s_saved_ssid[0] = '\0';
    nvs_close(nvs_handle);
    return ESP_OK;
  }
  if (err != ESP_OK) {
    if (ignore_schema_error) {
      // 旧命名空间可能存过实验数据，迁移时忽略这类结构错误。
      s_saved_ssid[0] = '\0';
      nvs_close(nvs_handle);
      return ESP_OK;
    }
    nvs_close(nvs_handle);
    ESP_RETURN_ON_ERROR(err, TAG, "read saved SSID failed");
  }

  size_t password_len = sizeof(s_saved_password);
  err = nvs_get_str(nvs_handle, APP_WIFI_NVS_KEY_PASSWORD, s_saved_password, &password_len);
  if (err == ESP_ERR_NVS_NOT_FOUND) {
    // 开放网络没有密码，按空密码处理。
    s_saved_password[0] = '\0';
    err = ESP_OK;
  } else if (err != ESP_OK && ignore_schema_error) {
    s_saved_ssid[0] = '\0';
    s_saved_password[0] = '\0';
    err = ESP_OK;
  }
  nvs_close(nvs_handle);

  ESP_RETURN_ON_ERROR(err, TAG, "read saved password failed");
  return ESP_OK;
}

// 保存 WiFi 凭据，并同步更新内存中的自动重连凭据。
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

// 读取当前命名空间凭据；如果没有，则尝试迁移旧命名空间。
static esp_err_t load_saved_credentials(void)
{
  ESP_RETURN_ON_ERROR(load_credentials_from_namespace(APP_WIFI_NVS_NAMESPACE, false),
                      TAG,
                      "load saved WiFi credentials failed");
  if (s_saved_ssid[0] != '\0') {
    ESP_LOGI(TAG, "loaded saved WiFi SSID: %s", s_saved_ssid);
    return ESP_OK;
  }

  ESP_RETURN_ON_ERROR(load_credentials_from_namespace(APP_WIFI_LEGACY_NVS_NAMESPACE, true),
                      TAG,
                      "load legacy WiFi credentials failed");
  if (s_saved_ssid[0] != '\0') {
    char migrated_ssid[APP_WIFI_SSID_MAX_LEN + 1];
    char migrated_password[APP_WIFI_PASSWORD_MAX_LEN + 1];
    // save_credentials 会改写全局缓存，所以先复制一份待迁移内容。
    strlcpy(migrated_ssid, s_saved_ssid, sizeof(migrated_ssid));
    strlcpy(migrated_password, s_saved_password, sizeof(migrated_password));
    ESP_LOGI(TAG, "loaded legacy saved WiFi SSID: %s", s_saved_ssid);
    ESP_RETURN_ON_ERROR(save_credentials(migrated_ssid, migrated_password),
                        TAG,
                        "migrate WiFi credentials failed");
  }
  return ESP_OK;
}

// 将凭据写入 WiFi STA 配置，并启动一次连接流程。
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
  wifi_config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
  wifi_config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
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
    // 新凭据只有在 GOT_IP 后才真正保存，避免错误密码覆盖可用配置。
    strlcpy(s_pending_ssid, ssid, sizeof(s_pending_ssid));
    strlcpy(s_pending_password, password != NULL ? password : "", sizeof(s_pending_password));
    s_pending_save = true;
  } else {
    s_pending_save = false;
  }

  s_suppress_next_disconnect_retry = suppress_disconnect_retry;
  esp_err_t err = esp_wifi_disconnect();
  if (err != ESP_OK) {
    // 如果没有真正触发断开流程，就不要吞掉后续真实断线事件。
    s_suppress_next_disconnect_retry = false;
  }
  if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_CONNECT) {
    ESP_LOGW(TAG, "disconnect before connect failed: %s", esp_err_to_name(err));
  }

  ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wifi_config), TAG, "set WiFi config failed");
  ESP_RETURN_ON_ERROR(esp_wifi_connect(), TAG, "connect WiFi failed");
  return ESP_OK;
}

// 延迟重连回调：避免断线瞬间立刻重试导致状态抖动。
static void retry_timer_cb(void *arg)
{
  (void)arg;

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
}

// 在有保存凭据且未超过次数限制时安排一次后台重连。
static void schedule_retry(void)
{
  if (s_saved_ssid[0] == '\0') {
    // 没有历史凭据时无法自动重连。
    return;
  }
  if (s_retry_count >= APP_WIFI_MAX_RETRIES) {
    ESP_LOGW(TAG, "WiFi retry limit reached, stop reconnecting");
    return;
  }
  if (s_retry_timer == NULL) {
    ESP_LOGE(TAG, "WiFi retry timer is not ready");
    return;
  }
  if (esp_timer_is_active(s_retry_timer)) {
    // 已有重连任务在等待，避免重复创建。
    return;
  }

  esp_err_t err = esp_timer_start_once(s_retry_timer, APP_WIFI_RETRY_DELAY_MS * 1000);
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    ESP_LOGE(TAG, "start WiFi retry timer failed: %s", esp_err_to_name(err));
  }
}

// 统一处理 WiFi/IP 事件，并维护对外可读的状态快照。
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
      // 主动切换网络时的断开是预期行为，不要立刻重连旧网络。
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
    if (s_retry_timer != NULL && esp_timer_is_active(s_retry_timer)) {
      esp_timer_stop(s_retry_timer);
    }

    if (s_pending_save) {
      // DHCP 成功后才说明凭据真的可用，此时再写入 NVS。
      esp_err_t err = save_credentials(s_pending_ssid, s_pending_password);
      if (err != ESP_OK) {
        ESP_LOGE(TAG, "save connected WiFi credentials failed: %s", esp_err_to_name(err));
      }
      s_pending_save = false;
    }
  }
}

// 初始化 WiFi STA 栈，并在存在保存 SSID 时自动连接。
esp_err_t app_wifi_init(void)
{
  // 多个界面入口都可能调用 WiFi 初始化，因此保持幂等。
  if (s_initialized) {
    return ESP_OK;
  }

  s_status_mutex = xSemaphoreCreateMutex();
  ESP_RETURN_ON_FALSE(s_status_mutex != NULL, ESP_ERR_NO_MEM, TAG, "create status mutex failed");

  esp_timer_create_args_t retry_timer_args = {
    .callback = retry_timer_cb,
    .name = "wifi_retry",
  };
  ESP_RETURN_ON_ERROR(esp_timer_create(&retry_timer_args, &s_retry_timer),
                      TAG,
                      "create WiFi retry timer failed");

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
  // 凭据由本组件保存到 NVS，WiFi 驱动只使用 RAM 配置。
  ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "set WiFi storage failed");
  ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "start WiFi failed");
  ESP_RETURN_ON_ERROR(esp_wifi_set_ps(WIFI_PS_NONE), TAG, "disable WiFi power save failed");

  s_initialized = true;
  if (s_saved_ssid[0] != '\0') {
    ESP_LOGI(TAG, "auto connect saved WiFi: %s", s_saved_ssid);
    ESP_RETURN_ON_ERROR(connect_with_credentials(s_saved_ssid, s_saved_password, false, false),
                        TAG,
                        "auto connect saved WiFi failed");
  }
  return ESP_OK;
}

// 执行一次阻塞式扫描，并把 AP 信息复制到调用者缓冲区。
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
    // 扫描为空是正常结果，保持 ap_count 为 0。
    return ESP_OK;
  }

  uint16_t read_count = found > APP_WIFI_SCAN_RECORDS_MAX ? APP_WIFI_SCAN_RECORDS_MAX : found;
  wifi_ap_record_t records[APP_WIFI_SCAN_RECORDS_MAX] = {0};
  if (read_count > APP_WIFI_SCAN_RECORDS_MAX) {
    // 本地临时数组固定为 APP_WIFI_SCAN_RECORDS_MAX，避免调用者传入过大导致越界。
    read_count = APP_WIFI_SCAN_RECORDS_MAX;
  }
  ESP_RETURN_ON_ERROR(esp_wifi_scan_get_ap_records(&read_count, records), TAG, "get AP records failed");

  for (uint16_t i = 0; i < read_count; ++i) {
    const char *ssid = (const char *)records[i].ssid;
    if (ssid[0] == '\0') {
      // 隐藏 SSID 无法从当前列表直接连接，避免占用可选网络名额。
      continue;
    }

    bool has_existing = false;
    size_t existing = 0;
    for (size_t j = 0; j < *ap_count; ++j) {
      if (strcmp(aps[j].ssid, ssid) == 0 && aps[j].authmode == records[i].authmode) {
        has_existing = true;
        existing = j;
        break;
      }
    }

    if (has_existing) {
      // 同名同加密类型 AP 合并展示，保留信号最强的 BSSID 信息。
      if (aps[existing].ap_count < UINT8_MAX) {
        ++aps[existing].ap_count;
      }
      if (records[i].rssi > aps[existing].rssi) {
        aps[existing].rssi = records[i].rssi;
      }
      continue;
    }

    if (*ap_count >= max_aps) {
      continue;
    }

    strlcpy(aps[*ap_count].ssid, ssid, sizeof(aps[*ap_count].ssid));
    aps[*ap_count].rssi = records[i].rssi;
    aps[*ap_count].ap_count = 1;
    aps[*ap_count].authmode = records[i].authmode;
    ++(*ap_count);
  }
  return ESP_OK;
}

// 启动用户发起的连接，并在成功后保存凭据。
esp_err_t app_wifi_connect(const char *ssid, const char *password)
{
  ESP_RETURN_ON_FALSE(ssid != NULL && ssid[0] != '\0', ESP_ERR_INVALID_ARG, TAG, "SSID is empty");
  ESP_RETURN_ON_ERROR(app_wifi_init(), TAG, "WiFi init failed");

  s_retry_count = 0;
  return connect_with_credentials(ssid, password, true, true);
}

// 复制当前 WiFi 状态；已连接时额外刷新实时 RSSI。
void app_wifi_get_status(app_wifi_status_t *status)
{
  if (status == NULL) {
    return;
  }

  status_lock();
  *status = s_status;
  status_unlock();

  if (status->connected) {
    // RSSI 会随时间变化，状态快照复制后再向驱动读取最新值。
    wifi_ap_record_t ap_info = {0};
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
      status->rssi = ap_info.rssi;
    } else {
      status->rssi = -127;
    }
  }
}

// 设置 WiFi 状态变化回调，LCD 使用它来刷新状态图标和文案。
void app_wifi_set_status_changed_cb(app_wifi_status_changed_cb_t cb, void *user_ctx)
{
  s_status_changed_cb = cb;
  s_status_changed_user_ctx = user_ctx;
}
