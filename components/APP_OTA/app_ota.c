#include "app_ota.h"

#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_wifi.h"
#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "wear_levelling.h"

static const char *TAG = "app_ota";

#define APP_OTA_CHECK_URL "http://192.168.1.254:3001/update/check?version=%s"
#define APP_OTA_HTTP_TIMEOUT_MS 15000
#define APP_OTA_CHECK_MAX_BYTES (16 * 1024)
#define APP_OTA_DOWNLOAD_BUFFER_SIZE 4096
#define APP_OTA_TASK_STACK 12288
#define APP_OTA_TASK_PRIORITY 4
#define APP_OTA_VFS_BASE_PATH "/vfs"
#define APP_OTA_VFS_PARTITION_LABEL "vfs"
#define APP_OTA_NVS_NAMESPACE "app_ota"
#define APP_OTA_NVS_KEY_PENDING "pending"
#define APP_OTA_NVS_KEY_COUNT "count"
#define APP_OTA_MAX_FILES 4
#define APP_OTA_TASK_WDT_TIMEOUT_MS 30000
#define APP_OTA_DEFAULT_WDT_TIMEOUT_MS (CONFIG_ESP_TASK_WDT_TIMEOUT_S * 1000)
#ifndef CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0
#define CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0 0
#endif
#ifndef CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU1
#define CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU1 0
#endif
#ifdef CONFIG_ESP_TASK_WDT_PANIC
#define APP_OTA_TASK_WDT_TRIGGER_PANIC true
#else
#define APP_OTA_TASK_WDT_TRIGGER_PANIC false
#endif
#define APP_OTA_TASK_WDT_IDLE_MASK \
  ((CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0 ? (1 << 0) : 0) | \
   (CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU1 ? (1 << 1) : 0))

typedef struct {
  mbedtls_sha256_context ctx;
} app_ota_sha256_t;

typedef struct {
  char partition[16];
  char url[256];
  char sha256[65];
  size_t size;
} app_ota_file_t;

typedef struct {
  app_ota_file_t file;
  char path[64];
} app_ota_staged_file_t;

static bool s_initialized;
static bool s_task_running;
static bool s_checked_this_boot;
static bool s_vfs_mounted;
static wl_handle_t s_vfs_wl_handle = WL_INVALID_HANDLE;

static void app_ota_task(void *arg);

// OTA 写 flash/校验镜像时会长时间进入 cache/flash IPC 临界区，临时放宽 Task WDT。
static esp_err_t configure_task_wdt(uint32_t timeout_ms)
{
  esp_task_wdt_config_t config = {
    .timeout_ms = timeout_ms,
    .idle_core_mask = APP_OTA_TASK_WDT_IDLE_MASK,
    .trigger_panic = APP_OTA_TASK_WDT_TRIGGER_PANIC,
  };
  esp_err_t err = esp_task_wdt_reconfigure(&config);
  if (err == ESP_ERR_INVALID_STATE) {
    // Task WDT 未启用时不需要处理。
    return ESP_OK;
  }
  return err;
}

static void relax_task_wdt_for_ota(void)
{
  esp_err_t err = configure_task_wdt(APP_OTA_TASK_WDT_TIMEOUT_MS);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "relax task watchdog for OTA failed: %s", esp_err_to_name(err));
  }
}

static void restore_task_wdt_after_ota(void)
{
  esp_err_t err = configure_task_wdt(APP_OTA_DEFAULT_WDT_TIMEOUT_MS);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "restore task watchdog after OTA failed: %s", esp_err_to_name(err));
  }
}

static esp_err_t init_nvs_if_needed(void)
{
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "erase NVS failed");
    err = nvs_flash_init();
  }
  if (err == ESP_ERR_INVALID_STATE) {
    return ESP_OK;
  }
  return err;
}

static void sha256_begin(app_ota_sha256_t *sha)
{
  mbedtls_sha256_init(&sha->ctx);
  mbedtls_sha256_starts(&sha->ctx, 0);
}

static void sha256_update(app_ota_sha256_t *sha, const void *data, size_t len)
{
  if (len > 0) {
    mbedtls_sha256_update(&sha->ctx, (const unsigned char *)data, len);
  }
}

static void sha256_finish(app_ota_sha256_t *sha, char out_hex[65])
{
  unsigned char digest[32] = {0};
  mbedtls_sha256_finish(&sha->ctx, digest);
  mbedtls_sha256_free(&sha->ctx);

  for (size_t i = 0; i < sizeof(digest); ++i) {
    snprintf(out_hex + (i * 2), 3, "%02x", digest[i]);
  }
  out_hex[64] = '\0';
}

static bool str_equals_ignore_case(const char *a, const char *b)
{
  if (a == NULL || b == NULL) {
    return false;
  }
  while (*a != '\0' && *b != '\0') {
    if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) {
      return false;
    }
    ++a;
    ++b;
  }
  return *a == '\0' && *b == '\0';
}

static bool is_app_partition(const char *partition)
{
  return str_equals_ignore_case(partition, "ota") ||
         str_equals_ignore_case(partition, "app") ||
         str_equals_ignore_case(partition, "ota_0") ||
         str_equals_ignore_case(partition, "ota_1");
}

static esp_err_t mount_vfs(void)
{
  if (s_vfs_mounted) {
    return ESP_OK;
  }

  const esp_vfs_fat_mount_config_t mount_config = {
    .format_if_mount_failed = true,
    .max_files = 4,
    .allocation_unit_size = 4096,
  };

  esp_err_t err = esp_vfs_fat_spiflash_mount_rw_wl(APP_OTA_VFS_BASE_PATH,
                                                    APP_OTA_VFS_PARTITION_LABEL,
                                                    &mount_config,
                                                    &s_vfs_wl_handle);
  ESP_RETURN_ON_ERROR(err, TAG, "mount OTA staging VFS failed");
  s_vfs_mounted = true;
  ESP_LOGI(TAG, "mounted OTA staging VFS");
  return ESP_OK;
}

static esp_err_t http_open_get(const char *url, esp_http_client_handle_t *out_client, int *out_length)
{
  esp_http_client_config_t cfg = {
    .url = url,
    .timeout_ms = APP_OTA_HTTP_TIMEOUT_MS,
    .buffer_size = APP_OTA_DOWNLOAD_BUFFER_SIZE,
    .buffer_size_tx = 1024,
    .keep_alive_enable = false,
  };

  esp_http_client_handle_t client = esp_http_client_init(&cfg);
  ESP_RETURN_ON_FALSE(client != NULL, ESP_ERR_NO_MEM, TAG, "init HTTP client failed");

  esp_err_t err = esp_http_client_open(client, 0);
  if (err != ESP_OK) {
    esp_http_client_cleanup(client);
    ESP_RETURN_ON_ERROR(err, TAG, "open URL failed: %s", url);
  }

  int content_length = esp_http_client_fetch_headers(client);
  int status_code = esp_http_client_get_status_code(client);
  if (status_code < 200 || status_code >= 300) {
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    ESP_LOGW(TAG, "HTTP status %d for %s", status_code, url);
    return ESP_FAIL;
  }

  *out_client = client;
  if (out_length != NULL) {
    *out_length = content_length;
  }
  return ESP_OK;
}

static void http_close(esp_http_client_handle_t client)
{
  if (client != NULL) {
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
  }
}

static esp_err_t fetch_manifest(char **out_json)
{
  const esp_app_desc_t *app_desc = esp_app_get_description();
  const char *version = app_desc != NULL ? app_desc->version : "0.0.0";
  char url[192];
  snprintf(url, sizeof(url), APP_OTA_CHECK_URL, version);

  esp_http_client_handle_t client = NULL;
  int content_length = 0;
  ESP_RETURN_ON_ERROR(http_open_get(url, &client, &content_length), TAG, "open OTA check failed");

  if (content_length > APP_OTA_CHECK_MAX_BYTES) {
    http_close(client);
    ESP_LOGW(TAG, "OTA manifest too large: %d", content_length);
    return ESP_ERR_INVALID_SIZE;
  }

  size_t capacity = content_length > 0 ? (size_t)content_length + 1 : 1024;
  char *json = calloc(1, capacity);
  if (json == NULL) {
    http_close(client);
    return ESP_ERR_NO_MEM;
  }

  size_t total = 0;
  while (true) {
    if (total + APP_OTA_DOWNLOAD_BUFFER_SIZE + 1 > capacity) {
      size_t new_capacity = capacity * 2;
      if (new_capacity > APP_OTA_CHECK_MAX_BYTES + 1) {
        free(json);
        http_close(client);
        return ESP_ERR_INVALID_SIZE;
      }
      char *new_json = realloc(json, new_capacity);
      if (new_json == NULL) {
        free(json);
        http_close(client);
        return ESP_ERR_NO_MEM;
      }
      json = new_json;
      capacity = new_capacity;
    }

    int read_len = esp_http_client_read(client, json + total, (int)(capacity - total - 1));
    if (read_len < 0) {
      free(json);
      http_close(client);
      ESP_LOGW(TAG, "read OTA manifest failed");
      return ESP_FAIL;
    }
    if (read_len == 0) {
      break;
    }
    total += (size_t)read_len;
    if (total > APP_OTA_CHECK_MAX_BYTES) {
      free(json);
      http_close(client);
      return ESP_ERR_INVALID_SIZE;
    }
  }

  json[total] = '\0';
  http_close(client);
  *out_json = json;
  ESP_LOGI(TAG, "OTA manifest received: %u bytes", (unsigned)total);
  return ESP_OK;
}

static esp_err_t parse_manifest(const char *json, app_ota_file_t *files, size_t max_files, size_t *file_count)
{
  *file_count = 0;

  cJSON *root = cJSON_Parse(json);
  ESP_RETURN_ON_FALSE(root != NULL, ESP_ERR_INVALID_RESPONSE, TAG, "parse OTA manifest failed");

  cJSON *new_version = cJSON_GetObjectItem(root, "newVersion");
  if (!cJSON_IsTrue(new_version)) {
    ESP_LOGI(TAG, "no OTA update available");
    cJSON_Delete(root);
    return ESP_ERR_NOT_FOUND;
  }

  cJSON *items = cJSON_GetObjectItem(root, "files");
  if (!cJSON_IsArray(items)) {
    cJSON_Delete(root);
    ESP_LOGW(TAG, "OTA manifest missing files[]");
    return ESP_ERR_INVALID_RESPONSE;
  }

  cJSON *item = NULL;
  cJSON_ArrayForEach(item, items) {
    if (*file_count >= max_files) {
      break;
    }

    cJSON *partition = cJSON_GetObjectItem(item, "partition");
    cJSON *url = cJSON_GetObjectItem(item, "url");
    cJSON *sha256 = cJSON_GetObjectItem(item, "sha256");
    cJSON *size = cJSON_GetObjectItem(item, "size");
    if (!cJSON_IsString(partition) || !cJSON_IsString(url) || !cJSON_IsString(sha256)) {
      ESP_LOGW(TAG, "skip invalid OTA file entry");
      continue;
    }

    app_ota_file_t *file = &files[*file_count];
    strlcpy(file->partition, partition->valuestring, sizeof(file->partition));
    strlcpy(file->url, url->valuestring, sizeof(file->url));
    strlcpy(file->sha256, sha256->valuestring, sizeof(file->sha256));
    file->size = cJSON_IsNumber(size) && size->valuedouble > 0 ? (size_t)size->valuedouble : 0;
    (*file_count)++;
  }

  cJSON_Delete(root);
  ESP_RETURN_ON_FALSE(*file_count > 0, ESP_ERR_INVALID_RESPONSE, TAG, "OTA manifest has no files");
  return ESP_OK;
}

static esp_err_t download_app_to_inactive_ota(const app_ota_file_t *file,
                                              const esp_partition_t **out_update_partition)
{
  const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
  ESP_RETURN_ON_FALSE(update_partition != NULL, ESP_ERR_NOT_FOUND, TAG, "no inactive OTA partition");

  if (file->size > 0 && file->size > update_partition->size) {
    ESP_LOGW(TAG, "app image too large: %u > %u", (unsigned)file->size, (unsigned)update_partition->size);
    return ESP_ERR_INVALID_SIZE;
  }

  esp_http_client_handle_t client = NULL;
  int content_length = 0;
  ESP_RETURN_ON_ERROR(http_open_get(file->url, &client, &content_length), TAG, "open app OTA URL failed");
  if (content_length > 0 && (size_t)content_length > update_partition->size) {
    http_close(client);
    return ESP_ERR_INVALID_SIZE;
  }

  uint8_t *buffer = malloc(APP_OTA_DOWNLOAD_BUFFER_SIZE);
  if (buffer == NULL) {
    http_close(client);
    return ESP_ERR_NO_MEM;
  }

  esp_ota_handle_t ota_handle = 0;
  esp_err_t err = esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &ota_handle);
  if (err != ESP_OK) {
    free(buffer);
    http_close(client);
    ESP_RETURN_ON_ERROR(err, TAG, "begin app OTA failed");
  }

  app_ota_sha256_t sha;
  sha256_begin(&sha);
  size_t total = 0;

  while (true) {
    int read_len = esp_http_client_read(client, (char *)buffer, APP_OTA_DOWNLOAD_BUFFER_SIZE);
    if (read_len < 0) {
      err = ESP_FAIL;
      break;
    }
    if (read_len == 0) {
      break;
    }
    err = esp_ota_write(ota_handle, buffer, read_len);
    if (err != ESP_OK) {
      break;
    }
    sha256_update(&sha, buffer, (size_t)read_len);
    total += (size_t)read_len;
    vTaskDelay(1);
  }

  char digest[65];
  sha256_finish(&sha, digest);
  if (err == ESP_OK && file->size > 0 && total != file->size) {
    ESP_LOGW(TAG, "app OTA size mismatch: got=%u expected=%u", (unsigned)total, (unsigned)file->size);
    err = ESP_ERR_INVALID_SIZE;
  }
  if (err == ESP_OK && !str_equals_ignore_case(digest, file->sha256)) {
    ESP_LOGW(TAG, "app OTA sha256 mismatch");
    err = ESP_ERR_INVALID_CRC;
  }

  esp_err_t end_err = esp_ota_end(ota_handle);
  if (err == ESP_OK) {
    err = end_err;
  }

  free(buffer);
  http_close(client);
  ESP_RETURN_ON_ERROR(err, TAG, "app OTA download failed");

  *out_update_partition = update_partition;
  ESP_LOGI(TAG, "app OTA written to %s, %u bytes", update_partition->label, (unsigned)total);
  return ESP_OK;
}

static esp_err_t download_file_to_vfs(const app_ota_file_t *file, char *out_path, size_t out_path_size)
{
  ESP_RETURN_ON_ERROR(mount_vfs(), TAG, "mount VFS failed");
  snprintf(out_path,
           out_path_size,
           APP_OTA_VFS_BASE_PATH "/ota_%.*s.bin",
           (int)(sizeof(file->partition) - 1),
           file->partition);
  remove(out_path);

  esp_http_client_handle_t client = NULL;
  int content_length = 0;
  ESP_RETURN_ON_ERROR(http_open_get(file->url, &client, &content_length), TAG, "open data OTA URL failed");

  FILE *fp = fopen(out_path, "wb");
  if (fp == NULL) {
    int open_errno = errno;
    http_close(client);
    ESP_LOGW(TAG, "open staging file failed: %s errno=%d", out_path, open_errno);
    return ESP_FAIL;
  }

  uint8_t *buffer = malloc(APP_OTA_DOWNLOAD_BUFFER_SIZE);
  if (buffer == NULL) {
    fclose(fp);
    http_close(client);
    return ESP_ERR_NO_MEM;
  }

  app_ota_sha256_t sha;
  sha256_begin(&sha);
  size_t total = 0;
  esp_err_t err = ESP_OK;

  while (true) {
    int read_len = esp_http_client_read(client, (char *)buffer, APP_OTA_DOWNLOAD_BUFFER_SIZE);
    if (read_len < 0) {
      err = ESP_FAIL;
      break;
    }
    if (read_len == 0) {
      break;
    }
    if (fwrite(buffer, 1, (size_t)read_len, fp) != (size_t)read_len) {
      err = ESP_FAIL;
      break;
    }
    sha256_update(&sha, buffer, (size_t)read_len);
    total += (size_t)read_len;
    vTaskDelay(1);
  }

  char digest[65];
  sha256_finish(&sha, digest);
  free(buffer);
  fclose(fp);
  http_close(client);

  if (err == ESP_OK && file->size > 0 && total != file->size) {
    ESP_LOGW(TAG, "data OTA size mismatch: got=%u expected=%u", (unsigned)total, (unsigned)file->size);
    err = ESP_ERR_INVALID_SIZE;
  }
  if (err == ESP_OK && !str_equals_ignore_case(digest, file->sha256)) {
    ESP_LOGW(TAG, "data OTA sha256 mismatch: %s", file->partition);
    err = ESP_ERR_INVALID_CRC;
  }

  ESP_RETURN_ON_ERROR(err, TAG, "download data OTA failed");
  ESP_LOGI(TAG, "downloaded %s to %s, %u bytes", file->partition, out_path, (unsigned)total);
  return ESP_OK;
}

static esp_err_t write_staged_file_to_partition(const app_ota_file_t *file, const char *path)
{
  const esp_partition_t *partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                                              ESP_PARTITION_SUBTYPE_ANY,
                                                              file->partition);
  ESP_RETURN_ON_FALSE(partition != NULL, ESP_ERR_NOT_FOUND, TAG, "partition not found: %s", file->partition);

  FILE *fp = fopen(path, "rb");
  ESP_RETURN_ON_FALSE(fp != NULL, ESP_FAIL, TAG, "open staging file for read failed: %s", path);

  if (file->size > 0 && file->size > partition->size) {
    fclose(fp);
    ESP_LOGW(TAG, "image too large for partition %s: %u > %u",
             file->partition,
             (unsigned)file->size,
             (unsigned)partition->size);
    return ESP_ERR_INVALID_SIZE;
  }

  uint8_t *buffer = malloc(APP_OTA_DOWNLOAD_BUFFER_SIZE);
  if (buffer == NULL) {
    fclose(fp);
    return ESP_ERR_NO_MEM;
  }

  esp_err_t err = esp_partition_erase_range(partition, 0, partition->size);
  size_t offset = 0;
  while (err == ESP_OK) {
    size_t read_len = fread(buffer, 1, APP_OTA_DOWNLOAD_BUFFER_SIZE, fp);
    if (read_len > 0) {
      err = esp_partition_write(partition, offset, buffer, read_len);
      if (err != ESP_OK) {
        break;
      }
      offset += read_len;
      vTaskDelay(1);
    }
    if (read_len < APP_OTA_DOWNLOAD_BUFFER_SIZE) {
      if (ferror(fp)) {
        err = ESP_FAIL;
      }
      break;
    }
  }
  fclose(fp);

  if (err == ESP_OK && file->size > 0 && offset != file->size) {
    err = ESP_ERR_INVALID_SIZE;
  }

  if (err == ESP_OK) {
    app_ota_sha256_t sha;
    sha256_begin(&sha);
    size_t verify_offset = 0;
    while (verify_offset < offset) {
      size_t to_read = offset - verify_offset;
      if (to_read > APP_OTA_DOWNLOAD_BUFFER_SIZE) {
        to_read = APP_OTA_DOWNLOAD_BUFFER_SIZE;
      }
      err = esp_partition_read(partition, verify_offset, buffer, to_read);
      if (err != ESP_OK) {
        break;
      }
      sha256_update(&sha, buffer, to_read);
      verify_offset += to_read;
      vTaskDelay(1);
    }
    char digest[65];
    sha256_finish(&sha, digest);
    if (err == ESP_OK && !str_equals_ignore_case(digest, file->sha256)) {
      ESP_LOGW(TAG, "written partition sha256 mismatch: %s", file->partition);
      err = ESP_ERR_INVALID_CRC;
    }
  }

  free(buffer);
  ESP_RETURN_ON_ERROR(err, TAG, "write partition failed: %s", file->partition);
  ESP_LOGI(TAG, "updated partition %s, %u bytes", file->partition, (unsigned)offset);
  return ESP_OK;
}

static esp_err_t partition_matches_sha256(const app_ota_file_t *file, bool *matches)
{
  *matches = false;
  if (file->size == 0) {
    return ESP_OK;
  }

  const esp_partition_t *partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                                              ESP_PARTITION_SUBTYPE_ANY,
                                                              file->partition);
  ESP_RETURN_ON_FALSE(partition != NULL, ESP_ERR_NOT_FOUND, TAG, "partition not found: %s", file->partition);
  if (file->size > partition->size) {
    return ESP_OK;
  }

  uint8_t *buffer = malloc(APP_OTA_DOWNLOAD_BUFFER_SIZE);
  ESP_RETURN_ON_FALSE(buffer != NULL, ESP_ERR_NO_MEM, TAG, "allocate hash buffer failed");

  app_ota_sha256_t sha;
  sha256_begin(&sha);
  esp_err_t err = ESP_OK;
  size_t offset = 0;
  while (offset < file->size) {
    size_t to_read = file->size - offset;
    if (to_read > APP_OTA_DOWNLOAD_BUFFER_SIZE) {
      to_read = APP_OTA_DOWNLOAD_BUFFER_SIZE;
    }
    err = esp_partition_read(partition, offset, buffer, to_read);
    if (err != ESP_OK) {
      break;
    }
    sha256_update(&sha, buffer, to_read);
    offset += to_read;
    vTaskDelay(1);
  }

  char digest[65];
  sha256_finish(&sha, digest);
  free(buffer);

  ESP_RETURN_ON_ERROR(err, TAG, "hash partition failed: %s", file->partition);
  *matches = str_equals_ignore_case(digest, file->sha256);
  return ESP_OK;
}

static esp_err_t running_app_matches_sha256(const app_ota_file_t *file, bool *matches)
{
  *matches = false;
  if (file->size == 0) {
    return ESP_OK;
  }

  const esp_partition_t *partition = esp_ota_get_running_partition();
  ESP_RETURN_ON_FALSE(partition != NULL, ESP_ERR_NOT_FOUND, TAG, "running app partition not found");
  if (file->size > partition->size) {
    return ESP_OK;
  }

  uint8_t *buffer = malloc(APP_OTA_DOWNLOAD_BUFFER_SIZE);
  ESP_RETURN_ON_FALSE(buffer != NULL, ESP_ERR_NO_MEM, TAG, "allocate app hash buffer failed");

  app_ota_sha256_t sha;
  sha256_begin(&sha);
  esp_err_t err = ESP_OK;
  size_t offset = 0;
  while (offset < file->size) {
    size_t to_read = file->size - offset;
    if (to_read > APP_OTA_DOWNLOAD_BUFFER_SIZE) {
      to_read = APP_OTA_DOWNLOAD_BUFFER_SIZE;
    }
    err = esp_partition_read(partition, offset, buffer, to_read);
    if (err != ESP_OK) {
      break;
    }
    sha256_update(&sha, buffer, to_read);
    offset += to_read;
    vTaskDelay(1);
  }

  char digest[65];
  sha256_finish(&sha, digest);
  free(buffer);

  ESP_RETURN_ON_ERROR(err, TAG, "hash running app failed");
  *matches = str_equals_ignore_case(digest, file->sha256);
  return ESP_OK;
}

static esp_err_t save_staged_files(const app_ota_staged_file_t *staged_files, size_t count)
{
  ESP_RETURN_ON_ERROR(init_nvs_if_needed(), TAG, "init NVS failed");

  nvs_handle_t nvs;
  ESP_RETURN_ON_ERROR(nvs_open(APP_OTA_NVS_NAMESPACE, NVS_READWRITE, &nvs),
                      TAG,
                      "open OTA NVS failed");

  esp_err_t err = nvs_set_u8(nvs, APP_OTA_NVS_KEY_PENDING, count > 0 ? 1 : 0);
  if (err == ESP_OK) {
    err = nvs_set_u8(nvs, APP_OTA_NVS_KEY_COUNT, (uint8_t)count);
  }

  for (size_t i = 0; err == ESP_OK && i < count; ++i) {
    char key[16];
    snprintf(key, sizeof(key), "part%u", (unsigned)i);
    err = nvs_set_str(nvs, key, staged_files[i].file.partition);
    if (err != ESP_OK) {
      break;
    }
    snprintf(key, sizeof(key), "path%u", (unsigned)i);
    err = nvs_set_str(nvs, key, staged_files[i].path);
    if (err != ESP_OK) {
      break;
    }
    snprintf(key, sizeof(key), "sha%u", (unsigned)i);
    err = nvs_set_str(nvs, key, staged_files[i].file.sha256);
    if (err != ESP_OK) {
      break;
    }
    snprintf(key, sizeof(key), "size%u", (unsigned)i);
    err = nvs_set_u32(nvs, key, (uint32_t)staged_files[i].file.size);
  }

  if (err == ESP_OK) {
    err = nvs_commit(nvs);
  }
  nvs_close(nvs);
  ESP_RETURN_ON_ERROR(err, TAG, "save staged OTA files failed");
  return ESP_OK;
}

static esp_err_t clear_staged_files(void)
{
  ESP_RETURN_ON_ERROR(init_nvs_if_needed(), TAG, "init NVS failed");

  nvs_handle_t nvs;
  esp_err_t err = nvs_open(APP_OTA_NVS_NAMESPACE, NVS_READWRITE, &nvs);
  if (err == ESP_ERR_NVS_NOT_FOUND) {
    return ESP_OK;
  }
  ESP_RETURN_ON_ERROR(err, TAG, "open OTA NVS failed");

  err = nvs_erase_all(nvs);
  if (err == ESP_OK) {
    err = nvs_commit(nvs);
  }
  nvs_close(nvs);
  return err;
}

static esp_err_t load_staged_files(app_ota_staged_file_t *staged_files, size_t max_count, size_t *out_count)
{
  *out_count = 0;
  ESP_RETURN_ON_ERROR(init_nvs_if_needed(), TAG, "init NVS failed");

  nvs_handle_t nvs;
  esp_err_t err = nvs_open(APP_OTA_NVS_NAMESPACE, NVS_READONLY, &nvs);
  if (err == ESP_ERR_NVS_NOT_FOUND) {
    return ESP_OK;
  }
  ESP_RETURN_ON_ERROR(err, TAG, "open OTA NVS failed");

  uint8_t pending = 0;
  err = nvs_get_u8(nvs, APP_OTA_NVS_KEY_PENDING, &pending);
  if (err == ESP_ERR_NVS_NOT_FOUND || pending == 0) {
    nvs_close(nvs);
    return ESP_OK;
  }
  if (err != ESP_OK) {
    nvs_close(nvs);
    return err;
  }

  uint8_t count = 0;
  err = nvs_get_u8(nvs, APP_OTA_NVS_KEY_COUNT, &count);
  if (err != ESP_OK) {
    nvs_close(nvs);
    return err;
  }
  if (count > max_count) {
    count = (uint8_t)max_count;
  }

  for (uint8_t i = 0; i < count; ++i) {
    char key[16];
    size_t len = 0;

    snprintf(key, sizeof(key), "part%u", (unsigned)i);
    len = sizeof(staged_files[i].file.partition);
    err = nvs_get_str(nvs, key, staged_files[i].file.partition, &len);
    if (err != ESP_OK) {
      break;
    }

    snprintf(key, sizeof(key), "path%u", (unsigned)i);
    len = sizeof(staged_files[i].path);
    err = nvs_get_str(nvs, key, staged_files[i].path, &len);
    if (err != ESP_OK) {
      break;
    }

    snprintf(key, sizeof(key), "sha%u", (unsigned)i);
    len = sizeof(staged_files[i].file.sha256);
    err = nvs_get_str(nvs, key, staged_files[i].file.sha256, &len);
    if (err != ESP_OK) {
      break;
    }

    snprintf(key, sizeof(key), "size%u", (unsigned)i);
    uint32_t size = 0;
    err = nvs_get_u32(nvs, key, &size);
    if (err != ESP_OK) {
      break;
    }
    staged_files[i].file.size = size;
    staged_files[i].file.url[0] = '\0';
  }

  nvs_close(nvs);
  ESP_RETURN_ON_ERROR(err, TAG, "load staged OTA files failed");
  *out_count = count;
  return ESP_OK;
}

static esp_err_t perform_update(const app_ota_file_t *files, size_t file_count)
{
  const esp_partition_t *app_update_partition = NULL;
  bool resource_updated = false;

  for (size_t i = 0; i < file_count; ++i) {
    if (!is_app_partition(files[i].partition)) {
      bool matches = false;
      ESP_RETURN_ON_ERROR(partition_matches_sha256(&files[i], &matches),
                          TAG,
                          "check data partition hash failed");
      if (matches) {
        ESP_LOGI(TAG, "partition %s already matches OTA image", files[i].partition);
        continue;
      }

      char staged_path[64];
      ESP_RETURN_ON_ERROR(download_file_to_vfs(&files[i], staged_path, sizeof(staged_path)),
                          TAG,
                          "download data file failed");

      ESP_RETURN_ON_ERROR(write_staged_file_to_partition(&files[i], staged_path),
                          TAG,
                          "write data file failed");
      remove(staged_path);
      resource_updated = true;
      vTaskDelay(1);
    }
  }

  for (size_t i = 0; i < file_count; ++i) {
    if (is_app_partition(files[i].partition)) {
      bool matches = false;
      ESP_RETURN_ON_ERROR(running_app_matches_sha256(&files[i], &matches),
                          TAG,
                          "check running app hash failed");
      if (matches) {
        ESP_LOGI(TAG, "running app already matches OTA image");
        continue;
      }

      ESP_RETURN_ON_ERROR(download_app_to_inactive_ota(&files[i], &app_update_partition),
                          TAG,
                          "update app failed");
      break;
    }
  }

  if (app_update_partition != NULL) {
    esp_err_t err = esp_ota_set_boot_partition(app_update_partition);
    if (err != ESP_OK) {
      ESP_RETURN_ON_ERROR(err, TAG, "set boot partition failed");
    }
  }

  if (resource_updated || app_update_partition != NULL) {
    ESP_LOGI(TAG, "OTA update complete, restarting");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
  }

  ESP_LOGI(TAG, "OTA manifest has no pending changes");
  return ESP_OK;
}

static void app_ota_task(void *arg)
{
  (void)arg;

  char *manifest_json = NULL;
  app_ota_file_t files[APP_OTA_MAX_FILES] = {0};
  size_t file_count = 0;

  esp_err_t err = fetch_manifest(&manifest_json);
  if (err == ESP_OK) {
    err = parse_manifest(manifest_json, files, APP_OTA_MAX_FILES, &file_count);
  }
  free(manifest_json);

  if (err == ESP_OK) {
    relax_task_wdt_for_ota();
    err = perform_update(files, file_count);
    restore_task_wdt_after_ota();
  }

  if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) {
    ESP_LOGW(TAG, "OTA check/update failed: %s", esp_err_to_name(err));
  }

  s_checked_this_boot = true;
  s_task_running = false;
  vTaskDelete(NULL);
}

static void start_ota_task_if_needed(void)
{
  if (s_task_running || s_checked_this_boot) {
    return;
  }

  s_task_running = true;
  BaseType_t ret = xTaskCreatePinnedToCore(app_ota_task,
                                           "app_ota",
                                           APP_OTA_TASK_STACK,
                                           NULL,
                                           APP_OTA_TASK_PRIORITY,
                                           NULL,
                                           1);
  if (ret != pdPASS) {
    s_task_running = false;
    ESP_LOGE(TAG, "create OTA task failed");
  }
}

static void ip_event_handler(void *arg,
                             esp_event_base_t event_base,
                             int32_t event_id,
                             void *event_data)
{
  (void)arg;
  (void)event_data;

  if (event_base != IP_EVENT || event_id != IP_EVENT_STA_GOT_IP) {
    return;
  }
  start_ota_task_if_needed();
}

esp_err_t app_ota_init(void)
{
  if (s_initialized) {
    return ESP_OK;
  }

  ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(IP_EVENT,
                                                          IP_EVENT_STA_GOT_IP,
                                                          ip_event_handler,
                                                          NULL,
                                                          NULL),
                      TAG,
                      "register OTA IP event handler failed");
  s_initialized = true;

  app_wifi_status_t status = {0};
  app_wifi_get_status(&status);
  if (status.connected) {
    start_ota_task_if_needed();
  }

  return ESP_OK;
}

esp_err_t app_ota_apply_staged(void)
{
  app_ota_staged_file_t staged_files[APP_OTA_MAX_FILES] = {0};
  size_t staged_count = 0;
  esp_err_t ret = ESP_OK;

  ESP_RETURN_ON_ERROR(load_staged_files(staged_files, APP_OTA_MAX_FILES, &staged_count),
                      TAG,
                      "load staged files failed");
  if (staged_count == 0) {
    return ESP_OK;
  }

  ESP_LOGI(TAG, "applying %u staged OTA resource file(s)", (unsigned)staged_count);
  relax_task_wdt_for_ota();
  ret = mount_vfs();
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "mount VFS failed: %s", esp_err_to_name(ret));
    goto cleanup;
  }

  for (size_t i = 0; i < staged_count; ++i) {
    esp_err_t err = write_staged_file_to_partition(&staged_files[i].file, staged_files[i].path);
    if (err != ESP_OK) {
      ESP_LOGE(TAG,
               "apply staged OTA resource failed: partition=%s err=%s",
               staged_files[i].file.partition,
               esp_err_to_name(err));
      ret = err;
      goto cleanup;
    }
    remove(staged_files[i].path);
  }

  ret = clear_staged_files();
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "clear staged files failed: %s", esp_err_to_name(ret));
    goto cleanup;
  }
  ESP_LOGI(TAG, "staged OTA resources applied");

cleanup:
  restore_task_wdt_after_ota();
  return ret;
}
