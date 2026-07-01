#ifndef APP_WIFI_H
#define APP_WIFI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_netif.h"
#include "esp_wifi_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define APP_WIFI_MAX_APS 12
#define APP_WIFI_SSID_MAX_LEN 32

typedef struct {
  char ssid[APP_WIFI_SSID_MAX_LEN + 1];
  int8_t rssi;
  uint8_t ap_count;
  wifi_auth_mode_t authmode;
} app_wifi_ap_record_t;

typedef struct {
  bool connected;
  bool connecting;
  int8_t rssi;
  char ssid[APP_WIFI_SSID_MAX_LEN + 1];
  esp_netif_ip_info_t ip_info;
} app_wifi_status_t;

typedef void (*app_wifi_status_changed_cb_t)(void *user_ctx);

// 初始化 NVS、WiFi STA 模式、事件处理器，并使用已保存凭据自动连接。
esp_err_t app_wifi_init(void);

// 执行阻塞式 WiFi 扫描，最多复制 max_aps 条结果。
esp_err_t app_wifi_scan(app_wifi_ap_record_t *aps, size_t max_aps, size_t *ap_count);

// 连接指定 SSID，并在连接成功后持久化凭据。
esp_err_t app_wifi_connect(const char *ssid, const char *password);

// 复制最新 WiFi 状态快照；已连接时包含 RSSI。
void app_wifi_get_status(app_wifi_status_t *status);

// 注册 WiFi 状态变化时调用的回调。
void app_wifi_set_status_changed_cb(app_wifi_status_changed_cb_t cb, void *user_ctx);

#ifdef __cplusplus
}
#endif

#endif
