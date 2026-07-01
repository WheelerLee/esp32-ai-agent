#ifndef APP_OTA_H
#define APP_OTA_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Registers the WiFi/IP event listener that checks for OTA updates after STA gets an IP.
esp_err_t app_ota_init(void);

// Applies resource images staged in VFS before font/model partitions are mounted or loaded.
esp_err_t app_ota_apply_staged(void);

#ifdef __cplusplus
}
#endif

#endif
