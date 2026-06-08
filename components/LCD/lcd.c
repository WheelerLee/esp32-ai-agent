#include "lcd.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "app_wifi.h"
#include "audio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_lcd_ili9341.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch.h"
#include "esp_lcd_touch_xpt2046.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"

static const char *TAG = "lcd";

LV_FONT_DECLARE(lv_font_chinese_16);

static esp_lcd_panel_handle_t s_panel_handle;
static lv_disp_draw_buf_t s_disp_buf;
static lv_disp_drv_t s_disp_drv;
static lv_indev_drv_t s_indev_drv;
static lv_obj_t *s_wifi_status_label;
static lv_obj_t *s_wifi_list;
static lv_obj_t *s_password_ta;
static lv_obj_t *s_keyboard;
static esp_lcd_touch_handle_t s_touch_handle;
static SemaphoreHandle_t s_lvgl_mutex;
static TaskHandle_t s_lvgl_task_handle;
static lv_font_t *s_external_chinese_font;
static bool s_font_fs_ready;
static uint32_t s_font_fs_io_count;
static app_wifi_ap_record_t s_scan_results[APP_WIFI_MAX_APS];
static size_t s_scan_count;
static char s_selected_ssid[APP_WIFI_SSID_MAX_LEN + 1];

static void show_home_page(void);
static void show_wifi_page(void);
static void show_wifi_connect_page(const char *ssid);
static void start_wifi_scan(void);
static void update_wifi_status_text(void);
static void lvgl_lock(void);
static void lvgl_unlock(void);

#define LCD_PLAY_URL "http://192.168.1.254:5500/music.mp3"
#define LCD_FONT_MOUNT_POINT "/font"
#define LCD_FONT_PARTITION_LABEL "font"
#define LCD_FONT_FILE_NAME "llm_text_14.bin"
#define LCD_FONT_LVGL_PATH "F:/" LCD_FONT_FILE_NAME
#define LCD_FONT_FS_YIELD_INTERVAL 64

static const lv_font_t *ui_font(void)
{
  return s_external_chinese_font != NULL ? s_external_chinese_font : &lv_font_chinese_16;
}

static bool font_fs_ready_cb(lv_fs_drv_t *drv)
{
  (void)drv;
  return s_font_fs_ready;
}

static void *font_fs_open_cb(lv_fs_drv_t *drv, const char *path, lv_fs_mode_t mode)
{
  (void)drv;

  if ((mode & LV_FS_MODE_WR) != 0 || path == NULL) {
    return NULL;
  }

  char full_path[128];
  int written = snprintf(full_path,
                         sizeof(full_path),
                         "%s%s%s",
                         LCD_FONT_MOUNT_POINT,
                         path[0] == '/' ? "" : "/",
                         path);
  if (written < 0 || written >= (int)sizeof(full_path)) {
    return NULL;
  }

  return fopen(full_path, "rb");
}

static void font_fs_yield(void)
{
  if (++s_font_fs_io_count >= LCD_FONT_FS_YIELD_INTERVAL) {
    s_font_fs_io_count = 0;
    vTaskDelay(pdMS_TO_TICKS(1));
  }
}

static lv_fs_res_t font_fs_close_cb(lv_fs_drv_t *drv, void *file_p)
{
  (void)drv;
  return fclose((FILE *)file_p) == 0 ? LV_FS_RES_OK : LV_FS_RES_UNKNOWN;
}

static lv_fs_res_t font_fs_read_cb(lv_fs_drv_t *drv, void *file_p, void *buf, uint32_t btr, uint32_t *br)
{
  (void)drv;

  size_t read_count = fread(buf, 1, btr, (FILE *)file_p);
  if (br != NULL) {
    *br = read_count;
  }
  font_fs_yield();
  return ferror((FILE *)file_p) == 0 ? LV_FS_RES_OK : LV_FS_RES_UNKNOWN;
}

static lv_fs_res_t font_fs_seek_cb(lv_fs_drv_t *drv, void *file_p, uint32_t pos, lv_fs_whence_t whence)
{
  (void)drv;

  int origin = SEEK_SET;
  if (whence == LV_FS_SEEK_CUR) {
    origin = SEEK_CUR;
  } else if (whence == LV_FS_SEEK_END) {
    origin = SEEK_END;
  }

  int ret = fseek((FILE *)file_p, (long)pos, origin);
  font_fs_yield();
  return ret == 0 ? LV_FS_RES_OK : LV_FS_RES_UNKNOWN;
}

static lv_fs_res_t font_fs_tell_cb(lv_fs_drv_t *drv, void *file_p, uint32_t *pos_p)
{
  (void)drv;

  long pos = ftell((FILE *)file_p);
  if (pos < 0) {
    return LV_FS_RES_UNKNOWN;
  }
  if (pos_p != NULL) {
    *pos_p = (uint32_t)pos;
  }
  return LV_FS_RES_OK;
}

static bool is_lvgl_bin_font_file(void)
{
  char full_path[128];
  int written = snprintf(full_path, sizeof(full_path), "%s/%s", LCD_FONT_MOUNT_POINT, LCD_FONT_FILE_NAME);
  if (written < 0 || written >= (int)sizeof(full_path)) {
    return false;
  }

  FILE *file = fopen(full_path, "rb");
  if (file == NULL) {
    return false;
  }

  uint8_t header[8] = {0};
  size_t read_count = fread(header, 1, sizeof(header), file);
  fclose(file);

  return read_count == sizeof(header) && header[0] == 0x30 && memcmp(&header[4], "head", 4) == 0;
}

static void register_font_lvgl_fs(void)
{
  static bool s_registered;
  static lv_fs_drv_t s_font_drv;

  if (s_registered) {
    return;
  }

  lv_fs_drv_init(&s_font_drv);
  s_font_drv.letter = 'F';
  s_font_drv.ready_cb = font_fs_ready_cb;
  s_font_drv.open_cb = font_fs_open_cb;
  s_font_drv.close_cb = font_fs_close_cb;
  s_font_drv.read_cb = font_fs_read_cb;
  s_font_drv.seek_cb = font_fs_seek_cb;
  s_font_drv.tell_cb = font_fs_tell_cb;
  lv_fs_drv_register(&s_font_drv);
  s_registered = true;
}

static void load_external_font(void)
{
  esp_vfs_spiffs_conf_t conf = {
    .base_path = LCD_FONT_MOUNT_POINT,
    .partition_label = LCD_FONT_PARTITION_LABEL,
    .max_files = 2,
    .format_if_mount_failed = false,
  };

  esp_err_t err = esp_vfs_spiffs_register(&conf);
  if (err == ESP_ERR_INVALID_STATE) {
    err = ESP_OK;
  }

  if (err != ESP_OK) {
    ESP_LOGW(TAG, "mount font partition failed: %s", esp_err_to_name(err));
    return;
  }

  s_font_fs_ready = true;
  register_font_lvgl_fs();

  int64_t start_us = esp_timer_get_time();
  if (!is_lvgl_bin_font_file()) {
    ESP_LOGW(TAG,
             "unsupported LVGL font file format: %s, checked in %lld ms",
             LCD_FONT_LVGL_PATH,
             (long long)((esp_timer_get_time() - start_us) / 1000));
    return;
  }

  s_external_chinese_font = lv_font_load(LCD_FONT_LVGL_PATH);
  if (s_external_chinese_font == NULL) {
    ESP_LOGW(TAG, "load external font failed: %s", LCD_FONT_LVGL_PATH);
  } else {
    ESP_LOGI(TAG,
             "loaded external font: %s in %lld ms",
             LCD_FONT_LVGL_PATH,
             (long long)((esp_timer_get_time() - start_us) / 1000));
  }
}

static uint16_t clamp_u16(uint16_t value, uint16_t min_value, uint16_t max_value)
{
  if (value < min_value) {
    return min_value;
  }
  if (value > max_value) {
    return max_value;
  }
  return value;
}

static uint16_t map_touch_axis(uint16_t value,
                               uint16_t in_min,
                               uint16_t in_max,
                               uint16_t out_max)
{
  value = clamp_u16(value, in_min, in_max);
  return (uint32_t)(value - in_min) * out_max / (in_max - in_min);
}

static void touch_process_coordinates(esp_lcd_touch_handle_t tp,
                                      uint16_t *x,
                                      uint16_t *y,
                                      uint16_t *strength,
                                      uint8_t *point_num,
                                      uint8_t max_point_num)
{
  (void)tp;
  (void)strength;

  if (x == NULL || y == NULL || point_num == NULL) {
    return;
  }

  for (uint8_t i = 0; i < *point_num && i < max_point_num; ++i) {
    uint16_t raw_x = x[i];
    uint16_t raw_y = y[i];
    uint16_t mapped_x;
    uint16_t mapped_y;

    if (LCD_TOUCH_SWAP_XY) {
      mapped_x = map_touch_axis(raw_y, LCD_TOUCH_RAW_Y_MIN, LCD_TOUCH_RAW_Y_MAX, LCD_H_RES - 1);
      mapped_y = map_touch_axis(raw_x, LCD_TOUCH_RAW_X_MIN, LCD_TOUCH_RAW_X_MAX, LCD_V_RES - 1);
    } else {
      mapped_x = map_touch_axis(raw_x, LCD_TOUCH_RAW_X_MIN, LCD_TOUCH_RAW_X_MAX, LCD_H_RES - 1);
      mapped_y = map_touch_axis(raw_y, LCD_TOUCH_RAW_Y_MIN, LCD_TOUCH_RAW_Y_MAX, LCD_V_RES - 1);
    }

    if (LCD_TOUCH_MIRROR_X) {
      mapped_x = (LCD_H_RES - 1) - mapped_x;
    }
    if (LCD_TOUCH_MIRROR_Y) {
      mapped_y = (LCD_V_RES - 1) - mapped_y;
    }

    x[i] = mapped_x;
    y[i] = mapped_y;

    static int64_t s_last_log_us;
    int64_t now_us = esp_timer_get_time();
    if (now_us - s_last_log_us > 500000) {
      ESP_LOGI(TAG, "touch raw=(%u,%u) mapped=(%u,%u)", raw_x, raw_y, mapped_x, mapped_y);
      s_last_log_us = now_us;
    }
  }
}

static void async_show_home_page(void *arg)
{
  (void)arg;
  show_home_page();
}

static void async_show_wifi_page(void *arg)
{
  (void)arg;
  show_wifi_page();
}

static void async_show_wifi_connect_page(void *arg)
{
  (void)arg;
  show_wifi_connect_page(s_selected_ssid);
}

// LVGL is not thread-safe, so all LVGL object/timer calls share this mutex.
static void lvgl_lock(void)
{
  if (s_lvgl_mutex != NULL) {
    xSemaphoreTake(s_lvgl_mutex, portMAX_DELAY);
  }
}

static void lvgl_unlock(void)
{
  if (s_lvgl_mutex != NULL) {
    xSemaphoreGive(s_lvgl_mutex);
  }
}

// esp_lcd calls this after an asynchronous color transfer has finished.
static bool lcd_flush_ready_cb(esp_lcd_panel_io_handle_t panel_io,
                               esp_lcd_panel_io_event_data_t *edata,
                               void *user_ctx)
{
  (void)panel_io;
  (void)edata;

  lv_disp_drv_t *disp_drv = (lv_disp_drv_t *)user_ctx;
  lv_disp_flush_ready(disp_drv);

  return false;
}

static void lcd_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map)
{
  esp_lcd_panel_handle_t panel_handle = (esp_lcd_panel_handle_t)drv->user_data;
  // LVGL uses inclusive coordinates; esp_lcd expects the end coordinate to be exclusive.
  int x_start = area->x1;
  int x_end = area->x2 + 1;
  int y_start = area->y1;
  int y_end = area->y2 + 1;

  esp_lcd_panel_draw_bitmap(panel_handle, x_start, y_start, x_end, y_end, color_map);
}

static void lvgl_tick_cb(void *arg)
{
  (void)arg;
  // Keep LVGL's internal time base moving independently of the main task.
  lv_tick_inc(2);
}

static lv_obj_t *create_label(lv_obj_t *parent, const char *text, const lv_font_t *font)
{
  lv_obj_t *label = lv_label_create(parent);
  if (label != NULL) {
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(0x1f2937), 0);
    lv_label_set_text(label, text != NULL ? text : "");
  }
  return label;
}

static lv_obj_t *create_button(lv_obj_t *parent,
                               const char *text,
                               lv_coord_t width,
                               lv_coord_t height,
                               lv_event_cb_t cb,
                               void *user_data)
{
  lv_obj_t *button = lv_btn_create(parent);
  if (button == NULL) {
    return NULL;
  }

  lv_obj_set_size(button, width, height);
  lv_obj_set_style_radius(button, 6, 0);
  lv_obj_set_style_bg_color(button, lv_color_hex(0x2563eb), 0);
  lv_obj_set_style_bg_color(button, lv_color_hex(0x1d4ed8), LV_STATE_PRESSED);
  if (cb != NULL) {
    lv_obj_add_event_cb(button, cb, LV_EVENT_CLICKED, user_data);
  }

  lv_obj_t *label = create_label(button, text, ui_font());
  if (label != NULL) {
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_center(label);
  }

  return button;
}

static lv_obj_t *create_list_row(lv_obj_t *parent,
                                 const char *text,
                                 lv_coord_t width,
                                 lv_coord_t height,
                                 lv_event_cb_t cb,
                                 void *user_data)
{
  lv_obj_t *row = lv_obj_create(parent);
  if (row == NULL) {
    return NULL;
  }

  lv_obj_set_size(row, width, height);
  lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_EVENT_BUBBLE | LV_OBJ_FLAG_SCROLL_CHAIN_VER);
  lv_obj_set_style_radius(row, 6, 0);
  lv_obj_set_style_border_width(row, 1, 0);
  lv_obj_set_style_border_color(row, lv_color_hex(0xcbd5e1), 0);
  lv_obj_set_style_bg_color(row, lv_color_hex(0xffffff), 0);
  lv_obj_set_style_bg_color(row, lv_color_hex(0xe0f2fe), LV_STATE_PRESSED);
  lv_obj_set_style_pad_left(row, 8, 0);
  lv_obj_set_style_pad_right(row, 8, 0);
  if (cb != NULL) {
    lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, user_data);
  }

  lv_obj_t *label = create_label(row, text, ui_font());
  if (label != NULL) {
    lv_obj_set_width(label, width - 16);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 0, 0);
  }

  return row;
}

static void clear_screen(void)
{
  lv_obj_clean(lv_scr_act());
  lv_obj_set_style_bg_color(lv_scr_act(), lv_color_hex(0xf8fafc), 0);
  lv_obj_set_style_bg_opa(lv_scr_act(), LV_OPA_COVER, 0);
}

static void home_wifi_event_cb(lv_event_t *event)
{
  if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
    lv_async_call(async_show_wifi_page, NULL);
  }
}

static void play_task(void *arg)
{
  const char *url = (const char *)arg;
  esp_err_t err = audio_play_test_tone();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "play I2S test tone failed: %s", esp_err_to_name(err));
  }
  vTaskDelay(pdMS_TO_TICKS(250));

  err = audio_play_mp3_url(url);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "play %s failed: %s", url, esp_err_to_name(err));
  }
  vTaskDelete(NULL);
}

static void home_play_event_cb(lv_event_t *event)
{
  if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
    return;
  }

  BaseType_t ret = xTaskCreate(play_task, "audio_play", 8192, (void *)LCD_PLAY_URL, 5, NULL);
  if (ret != pdPASS) {
    ESP_LOGE(TAG, "create audio play task failed");
  }
}

static void home_volume_down_event_cb(lv_event_t *event)
{
  if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
    audio_volume_down();
  }
}

static void home_volume_up_event_cb(lv_event_t *event)
{
  if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
    audio_volume_up();
  }
}

static void wifi_back_event_cb(lv_event_t *event)
{
  if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
    lv_async_call(async_show_home_page, NULL);
  }
}

static void wifi_refresh_event_cb(lv_event_t *event)
{
  if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
    lv_async_call(async_show_wifi_page, NULL);
  }
}

static void connect_back_event_cb(lv_event_t *event)
{
  if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
    lv_async_call(async_show_wifi_page, NULL);
  }
}

static void wifi_scan_event_cb(lv_event_t *event)
{
  if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
    start_wifi_scan();
  }
}

static void close_keyboard(void)
{
  if (s_keyboard != NULL) {
    lv_obj_del(s_keyboard);
    s_keyboard = NULL;
  }
}

static void textarea_event_cb(lv_event_t *event)
{
  if (lv_event_get_code(event) != LV_EVENT_FOCUSED || s_password_ta == NULL) {
    return;
  }

  close_keyboard();
  s_keyboard = lv_keyboard_create(lv_scr_act());
  if (s_keyboard != NULL) {
    lv_obj_set_size(s_keyboard, LCD_H_RES, 96);
    lv_obj_align(s_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(s_keyboard, s_password_ta);
  }
}

static void connect_task(void *arg)
{
  char password[65] = {0};
  if (arg != NULL) {
    strlcpy(password, (const char *)arg, sizeof(password));
  }

  esp_err_t err = app_wifi_connect(s_selected_ssid, password);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "connect to %s failed: %s", s_selected_ssid, esp_err_to_name(err));
  }

  for (int i = 0; i < 20; ++i) {
    lvgl_lock();
    update_wifi_status_text();
    lvgl_unlock();

    app_wifi_status_t status = {0};
    app_wifi_get_status(&status);
    if (status.connected || (!status.connecting && i > 1)) {
      break;
    }
    vTaskDelay(pdMS_TO_TICKS(500));
  }

  lvgl_lock();
  show_wifi_page();
  lvgl_unlock();
  vTaskDelete(NULL);
}

static void connect_event_cb(lv_event_t *event)
{
  if (lv_event_get_code(event) != LV_EVENT_CLICKED || s_password_ta == NULL) {
    return;
  }

  const char *password = lv_textarea_get_text(s_password_ta);
  static char s_password[65];
  strlcpy(s_password, password != NULL ? password : "", sizeof(s_password));
  close_keyboard();
  update_wifi_status_text();

  BaseType_t ret = xTaskCreate(connect_task, "wifi_connect", 4096, s_password, 4, NULL);
  if (ret != pdPASS) {
    ESP_LOGE(TAG, "create WiFi connect task failed");
  }
}

static void ssid_event_cb(lv_event_t *event)
{
  if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
    return;
  }

  const char *ssid = (const char *)lv_event_get_user_data(event);
  if (ssid == NULL || ssid[0] == '\0') {
    return;
  }

  strlcpy(s_selected_ssid, ssid, sizeof(s_selected_ssid));
  lv_async_call(async_show_wifi_connect_page, NULL);
}

static void update_wifi_status_text(void)
{
  if (s_wifi_status_label == NULL) {
    return;
  }

  app_wifi_status_t status = {0};
  app_wifi_get_status(&status);

  char text[160];
  if (status.connected) {
    snprintf(text,
             sizeof(text),
             "已连接\n网络: %s\nIP: " IPSTR "\n网关: " IPSTR,
             status.ssid,
             IP2STR(&status.ip_info.ip),
             IP2STR(&status.ip_info.gw));
  } else if (status.connecting) {
    snprintf(text, sizeof(text), "正在连接\n网络: %s", status.ssid);
  } else {
    snprintf(text, sizeof(text), "未连接\n请扫描并选择 WiFi");
  }

  lv_label_set_text(s_wifi_status_label, text);
}

static void show_scan_results_locked(void)
{
  if (s_wifi_list == NULL) {
    return;
  }

  lv_obj_clean(s_wifi_list);
  if (s_scan_count == 0) {
    create_label(s_wifi_list, "未发现 WiFi", ui_font());
    return;
  }

  for (size_t i = 0; i < s_scan_count; ++i) {
    char row_text[64];
    snprintf(row_text,
             sizeof(row_text),
             "%s  %d dBm",
             s_scan_results[i].ssid[0] != '\0' ? s_scan_results[i].ssid : "<隐藏>",
             s_scan_results[i].rssi);
    lv_obj_t *row = create_list_row(s_wifi_list,
                                    row_text,
                                    LCD_H_RES - 32,
                                    34,
                                    ssid_event_cb,
                                    s_scan_results[i].ssid);
    if (row != NULL) {
      lv_obj_set_scroll_dir(row, LV_DIR_VER);
    }
  }
}

static void scan_task(void *arg)
{
  (void)arg;

  size_t count = 0;
  esp_err_t err = app_wifi_scan(s_scan_results, APP_WIFI_MAX_APS, &count);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "scan WiFi failed: %s", esp_err_to_name(err));
    count = 0;
  }

  lvgl_lock();
  s_scan_count = count;
  update_wifi_status_text();
  show_scan_results_locked();
  lvgl_unlock();

  vTaskDelete(NULL);
}

static void start_wifi_scan(void)
{
  close_keyboard();
  s_password_ta = NULL;
  s_scan_count = 0;

  if (s_wifi_list != NULL) {
    lv_obj_clean(s_wifi_list);
    create_label(s_wifi_list, "正在扫描...", ui_font());
  }

  BaseType_t ret = xTaskCreate(scan_task, "wifi_scan", 4096, NULL, 4, NULL);
  if (ret != pdPASS) {
    ESP_LOGE(TAG, "create WiFi scan task failed");
  }
}

static void show_home_page(void)
{
  close_keyboard();
  s_wifi_status_label = NULL;
  s_wifi_list = NULL;
  s_password_ta = NULL;

  clear_screen();

  lv_obj_t *title = create_label(lv_scr_act(), "ESP32 智能助手", ui_font());
  if (title != NULL) {
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 36);
  }

  lv_obj_t *wifi_button = create_button(lv_scr_act(), "WiFi", 180, 58, home_wifi_event_cb, NULL);
  if (wifi_button != NULL) {
    lv_obj_align(wifi_button, LV_ALIGN_CENTER, 0, -18);
  }

  lv_obj_t *volume_down_button = create_button(lv_scr_act(), "音量-", 58, 58, home_volume_down_event_cb, NULL);
  if (volume_down_button != NULL) {
    lv_obj_align(volume_down_button, LV_ALIGN_CENTER, -96, 52);
  }

  lv_obj_t *play_button = create_button(lv_scr_act(), "播放", 104, 58, home_play_event_cb, NULL);
  if (play_button != NULL) {
    lv_obj_align(play_button, LV_ALIGN_CENTER, 0, 52);
  }

  lv_obj_t *volume_up_button = create_button(lv_scr_act(), "音量+", 58, 58, home_volume_up_event_cb, NULL);
  if (volume_up_button != NULL) {
    lv_obj_align(volume_up_button, LV_ALIGN_CENTER, 96, 52);
  }
}

static void show_wifi_connect_page(const char *ssid)
{
  close_keyboard();
  s_wifi_status_label = NULL;
  s_wifi_list = NULL;
  s_password_ta = NULL;

  clear_screen();

  lv_obj_t *header = lv_obj_create(lv_scr_act());
  if (header != NULL) {
    lv_obj_set_size(header, LCD_H_RES, 42);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_set_style_radius(header, 0, 0);
    lv_obj_set_style_bg_color(header, lv_color_hex(0xe2e8f0), 0);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *back = create_button(header, "返回", 72, 30, connect_back_event_cb, NULL);
    if (back != NULL) {
      lv_obj_align(back, LV_ALIGN_LEFT_MID, 4, 0);
    }

    lv_obj_t *title = create_label(header, "连接 WiFi", ui_font());
    if (title != NULL) {
      lv_obj_align(title, LV_ALIGN_CENTER, 0, 0);
    }
  }

  char ssid_text[64];
  snprintf(ssid_text, sizeof(ssid_text), "SSID: %s", ssid != NULL ? ssid : "");
  lv_obj_t *ssid_label = create_label(lv_scr_act(), ssid_text, ui_font());
  if (ssid_label != NULL) {
    lv_obj_set_width(ssid_label, LCD_H_RES - 24);
    lv_label_set_long_mode(ssid_label, LV_LABEL_LONG_DOT);
    lv_obj_align(ssid_label, LV_ALIGN_TOP_LEFT, 12, 52);
  }

  s_password_ta = lv_textarea_create(lv_scr_act());
  if (s_password_ta != NULL) {
    lv_obj_set_size(s_password_ta, LCD_H_RES - 24, 38);
    lv_obj_align(s_password_ta, LV_ALIGN_TOP_MID, 0, 78);
    lv_textarea_set_one_line(s_password_ta, true);
    lv_textarea_set_password_mode(s_password_ta, false);
    lv_textarea_set_placeholder_text(s_password_ta, "请输入密码");
    lv_obj_add_event_cb(s_password_ta, textarea_event_cb, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_state(s_password_ta, LV_STATE_FOCUSED);
  }

  lv_obj_t *connect = create_button(lv_scr_act(), "连接", LCD_H_RES - 24, 34, connect_event_cb, NULL);
  if (connect != NULL) {
    lv_obj_align(connect, LV_ALIGN_TOP_MID, 0, 122);
  }

  if (s_password_ta != NULL) {
    s_keyboard = lv_keyboard_create(lv_scr_act());
    if (s_keyboard != NULL) {
      lv_obj_set_size(s_keyboard, LCD_H_RES, 78);
      lv_obj_align(s_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
      lv_keyboard_set_textarea(s_keyboard, s_password_ta);
    }
  }
}

static void show_wifi_page(void)
{
  close_keyboard();
  s_password_ta = NULL;
  clear_screen();

  lv_obj_t *header = lv_obj_create(lv_scr_act());
  if (header != NULL) {
    lv_obj_set_size(header, LCD_H_RES, 42);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_set_style_radius(header, 0, 0);
    lv_obj_set_style_bg_color(header, lv_color_hex(0xe2e8f0), 0);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *back = create_button(header, "返回", 72, 30, wifi_back_event_cb, NULL);
    if (back != NULL) {
      lv_obj_align(back, LV_ALIGN_LEFT_MID, 4, 0);
    }

    lv_obj_t *title = create_label(header, "WiFi", ui_font());
    if (title != NULL) {
      lv_obj_align(title, LV_ALIGN_CENTER, 0, 0);
    }

    lv_obj_t *refresh = create_button(header, "刷新", 86, 30, wifi_refresh_event_cb, NULL);
    if (refresh != NULL) {
      lv_obj_align(refresh, LV_ALIGN_RIGHT_MID, -4, 0);
    }
  }

  s_wifi_status_label = create_label(lv_scr_act(), "", ui_font());
  if (s_wifi_status_label != NULL) {
    lv_obj_set_width(s_wifi_status_label, LCD_H_RES - 24);
    lv_label_set_long_mode(s_wifi_status_label, LV_LABEL_LONG_WRAP);
    lv_obj_align(s_wifi_status_label, LV_ALIGN_TOP_LEFT, 12, 52);
  }
  update_wifi_status_text();

  s_wifi_list = lv_obj_create(lv_scr_act());
  if (s_wifi_list != NULL) {
    lv_obj_set_size(s_wifi_list, LCD_H_RES - 16, 108);
    lv_obj_align(s_wifi_list, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_set_flex_flow(s_wifi_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(s_wifi_list, 6, 0);
    lv_obj_set_style_pad_row(s_wifi_list, 6, 0);
    lv_obj_set_style_radius(s_wifi_list, 6, 0);
    lv_obj_set_style_bg_color(s_wifi_list, lv_color_white(), 0);
    lv_obj_set_style_border_color(s_wifi_list, lv_color_hex(0xcbd5e1), 0);
    lv_obj_set_scroll_dir(s_wifi_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_wifi_list, LV_SCROLLBAR_MODE_AUTO);
  }

  app_wifi_status_t status = {0};
  app_wifi_get_status(&status);
  if (status.connected || status.connecting) {
    if (s_wifi_list != NULL) {
      create_button(s_wifi_list, "重新扫描", LCD_H_RES - 32, 36, wifi_scan_event_cb, NULL);
    }
  } else {
    start_wifi_scan();
  }
}

static void touch_read_cb(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
  esp_lcd_touch_handle_t touch_handle = (esp_lcd_touch_handle_t)drv->user_data;
  uint16_t x[1];
  uint16_t y[1];
  uint16_t strength[1];
  uint8_t count = 0;

  data->state = LV_INDEV_STATE_REL;
  data->continue_reading = false;

  if (touch_handle == NULL || esp_lcd_touch_read_data(touch_handle) != ESP_OK) {
    return;
  }

  if (esp_lcd_touch_get_coordinates(touch_handle, x, y, strength, &count, 1) && count > 0) {
    data->point.x = x[0];
    data->point.y = y[0];
    data->state = LV_INDEV_STATE_PR;
  }
}

static void lvgl_task(void *arg)
{
  (void)arg;

  while (1) {
    lvgl_lock();
    uint32_t task_delay_ms = lv_timer_handler();
    lvgl_unlock();
    if (task_delay_ms > 20) {
      task_delay_ms = 20;
    }
    if (task_delay_ms < 5) {
      task_delay_ms = 5;
    }
    vTaskDelay(pdMS_TO_TICKS(task_delay_ms));
  }
}

static esp_err_t lcd_backlight_init(void)
{
  // Keep the backlight off until the panel is initialized to avoid visible noise.
  gpio_config_t bk_gpio_config = {
    .mode = GPIO_MODE_OUTPUT,
    .pin_bit_mask = 1ULL << LCD_PIN_NUM_BK_LIGHT,
  };
  ESP_RETURN_ON_ERROR(gpio_config(&bk_gpio_config), TAG, "configure backlight GPIO failed");
  ESP_RETURN_ON_ERROR(gpio_set_level(LCD_PIN_NUM_BK_LIGHT, LCD_BK_LIGHT_OFF_LEVEL),
                      TAG,
                      "turn backlight off failed");
  return ESP_OK;
}

static esp_err_t lcd_panel_init(void)
{
  // Configure SPI2 on ESP32-S3 native IO_MUX pins for the display bus.
  spi_bus_config_t bus_config = {
    .sclk_io_num = LCD_PIN_NUM_SCLK,
    .mosi_io_num = LCD_PIN_NUM_MOSI,
    .miso_io_num = LCD_PIN_NUM_MISO,
    .quadwp_io_num = GPIO_NUM_NC,
    .quadhd_io_num = GPIO_NUM_NC,
    .max_transfer_sz = LCD_H_RES * LCD_DRAW_BUF_LINES * sizeof(lv_color_t),
  };
  ESP_RETURN_ON_ERROR(spi_bus_initialize(LCD_SPI_HOST, &bus_config, SPI_DMA_CH_AUTO),
                      TAG,
                      "initialize SPI bus failed");

  // Panel IO translates esp_lcd commands and pixel transfers to SPI transactions.
  esp_lcd_panel_io_handle_t io_handle = NULL;
  esp_lcd_panel_io_spi_config_t io_config = {
    .dc_gpio_num = LCD_PIN_NUM_DC,
    .cs_gpio_num = LCD_PIN_NUM_CS,
    .pclk_hz = LCD_PIXEL_CLOCK_HZ,
    .lcd_cmd_bits = 8,
    .lcd_param_bits = 8,
    .spi_mode = 0,
    .trans_queue_depth = 10,
    .on_color_trans_done = lcd_flush_ready_cb,
    .user_ctx = &s_disp_drv,
  };
  ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_SPI_HOST,
                                               &io_config,
                                               &io_handle),
                      TAG,
                      "create LCD panel IO failed");

  // Most 2.8 inch ILI9341 modules use BGR order; switch here if colors are wrong.
  esp_lcd_panel_dev_config_t panel_config = {
    .reset_gpio_num = LCD_PIN_NUM_RST,
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 2, 0)
    .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
#else
    .color_space = ESP_LCD_COLOR_SPACE_BGR,
#endif
    .bits_per_pixel = 16,
  };
  ESP_RETURN_ON_ERROR(esp_lcd_new_panel_ili9341(io_handle, &panel_config, &s_panel_handle),
                      TAG,
                      "create ILI9341 panel failed");

  ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel_handle), TAG, "reset panel failed");
  ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel_handle), TAG, "init panel failed");
  // Rotate the physical 240x320 panel into landscape 320x240.
  ESP_RETURN_ON_ERROR(esp_lcd_panel_swap_xy(s_panel_handle, LCD_SWAP_XY), TAG, "swap xy failed");
  ESP_RETURN_ON_ERROR(esp_lcd_panel_mirror(s_panel_handle, LCD_MIRROR_X, LCD_MIRROR_Y),
                      TAG,
                      "mirror panel failed");
  ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel_handle, true), TAG, "turn display on failed");
  ESP_RETURN_ON_ERROR(gpio_set_level(LCD_PIN_NUM_BK_LIGHT, LCD_BK_LIGHT_ON_LEVEL),
                      TAG,
                      "turn backlight on failed");

  return ESP_OK;
}

static esp_err_t touch_init(void)
{
  esp_lcd_panel_io_handle_t touch_io_handle = NULL;
  esp_lcd_panel_io_spi_config_t touch_io_config =
    ESP_LCD_TOUCH_IO_SPI_XPT2046_CONFIG(LCD_PIN_NUM_TOUCH_CS);

  ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_SPI_HOST,
                                               &touch_io_config,
                                               &touch_io_handle),
                      TAG,
                      "create touch panel IO failed");

  esp_lcd_touch_config_t touch_config = {
    .x_max = LCD_H_RES,
    .y_max = LCD_V_RES,
    .rst_gpio_num = GPIO_NUM_NC,
    .int_gpio_num = LCD_PIN_NUM_TOUCH_IRQ,
    .flags = {
      .swap_xy = false,
      .mirror_x = false,
      .mirror_y = false,
    },
    .process_coordinates = touch_process_coordinates,
  };

  ESP_LOGI(TAG, "initialize XPT2046 touch controller");
  ESP_RETURN_ON_ERROR(esp_lcd_touch_new_spi_xpt2046(touch_io_handle,
                                                    &touch_config,
                                                    &s_touch_handle),
                      TAG,
                      "initialize XPT2046 failed");

  lv_indev_drv_init(&s_indev_drv);
  s_indev_drv.type = LV_INDEV_TYPE_POINTER;
  s_indev_drv.read_cb = touch_read_cb;
  s_indev_drv.user_data = s_touch_handle;
  lv_indev_drv_register(&s_indev_drv);

  return ESP_OK;
}

static esp_err_t lvgl_port_init(void)
{
  lv_init();
  load_external_font();

  s_lvgl_mutex = xSemaphoreCreateMutex();
  ESP_RETURN_ON_FALSE(s_lvgl_mutex, ESP_ERR_NO_MEM, TAG, "create LVGL mutex failed");

  // Two DMA-capable partial draw buffers let LVGL render while SPI sends pixels.
  size_t draw_buffer_size = LCD_H_RES * LCD_DRAW_BUF_LINES;
  lv_color_t *buf1 = heap_caps_malloc(draw_buffer_size * sizeof(lv_color_t),
                                      MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  lv_color_t *buf2 = heap_caps_malloc(draw_buffer_size * sizeof(lv_color_t),
                                      MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  ESP_RETURN_ON_FALSE(buf1 && buf2, ESP_ERR_NO_MEM, TAG, "allocate LVGL draw buffers failed");

  lv_disp_draw_buf_init(&s_disp_buf, buf1, buf2, draw_buffer_size);

  // Register this ILI9341 panel as LVGL's default display.
  lv_disp_drv_init(&s_disp_drv);
  s_disp_drv.hor_res = LCD_H_RES;
  s_disp_drv.ver_res = LCD_V_RES;
  s_disp_drv.flush_cb = lcd_flush_cb;
  s_disp_drv.draw_buf = &s_disp_buf;
  s_disp_drv.user_data = s_panel_handle;
  lv_disp_drv_register(&s_disp_drv);
  ESP_RETURN_ON_ERROR(touch_init(), TAG, "touch init failed");

  const esp_timer_create_args_t tick_timer_args = {
    .callback = lvgl_tick_cb,
    .name = "lvgl_tick",
  };
  esp_timer_handle_t tick_timer = NULL;
  ESP_RETURN_ON_ERROR(esp_timer_create(&tick_timer_args, &tick_timer), TAG, "create LVGL tick failed");
  ESP_RETURN_ON_ERROR(esp_timer_start_periodic(tick_timer, 2 * 1000), TAG, "start LVGL tick failed");

  // The LVGL task runs the timer handler; UI changes from other tasks must lock first.
  BaseType_t ret = xTaskCreate(lvgl_task, "lvgl", 8192, NULL, 5, &s_lvgl_task_handle);
  ESP_RETURN_ON_FALSE(ret == pdPASS, ESP_ERR_NO_MEM, TAG, "create LVGL task failed");

  return ESP_OK;
}

esp_err_t lcd_init(void)
{
  ESP_LOGI(TAG, "initialize ILI9341 on SPI2");
  ESP_RETURN_ON_ERROR(lcd_backlight_init(), TAG, "backlight init failed");
  ESP_RETURN_ON_ERROR(lcd_panel_init(), TAG, "panel init failed");
  ESP_RETURN_ON_ERROR(lvgl_port_init(), TAG, "LVGL init failed");
  ESP_RETURN_ON_ERROR(app_wifi_init(), TAG, "WiFi init failed");

  lvgl_lock();
  show_home_page();
  lv_obj_invalidate(lv_scr_act());
  lvgl_unlock();

  return ESP_OK;
}

void lcd_show_text(const char *text)
{
  (void)text;

  lvgl_lock();
  show_home_page();
  lvgl_unlock();
}
