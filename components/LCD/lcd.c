#include "lcd.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "app_wifi.h"
#include "audio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_app_desc.h"
#include "esp_freertos_hooks.h"
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
#include "lazy_font.h"
#include "lvgl.h"

static const char *TAG = "lcd";

LV_FONT_DECLARE(lv_font_chinese_16);

static esp_lcd_panel_handle_t s_panel_handle;
static lv_disp_draw_buf_t s_disp_buf;
static lv_disp_drv_t s_disp_drv;
static lv_indev_drv_t s_indev_drv;
static lv_obj_t *s_home_wifi_button;
static lv_obj_t *s_home_volume_label;
static lv_obj_t *s_home_question_label;
static lv_obj_t *s_home_reply_label;
static lv_timer_t *s_home_wifi_timer;
static lv_obj_t *s_app_version_label;
static lv_obj_t *s_cpu_usage_label;
static lv_timer_t *s_cpu_usage_timer;
static lv_obj_t *s_wifi_status_label;
static lv_obj_t *s_wifi_status_icon;
static lv_obj_t *s_wifi_list;
static lv_obj_t *s_password_ta;
static lv_obj_t *s_keyboard;
static esp_lcd_touch_handle_t s_touch_handle;
static SemaphoreHandle_t s_lvgl_mutex;
static TaskHandle_t s_lvgl_task_handle;
static const lv_font_t *s_external_chinese_font;
static app_wifi_ap_record_t s_scan_results[APP_WIFI_MAX_APS];
static size_t s_scan_count;
static char s_selected_ssid[APP_WIFI_SSID_MAX_LEN + 1];
static char s_home_question_text[512];
static char s_home_reply_text[512];
static bool s_home_reply_thinking;
static bool s_lcd_ready;
static volatile uint32_t s_cpu_idle_count[2];
static uint32_t s_cpu_idle_last[2];
static uint32_t s_cpu_idle_max[2];

static void show_home_page(void);
static void show_wifi_page(void);
static void show_wifi_connect_page(const char *ssid);
static void start_wifi_scan(void);
static void update_wifi_status_text(void);
static void update_wifi_status_icon(void);
static void update_home_wifi_icon(void);
static void stop_home_wifi_timer(void);
static void create_cpu_usage_label(bool above_keyboard);
static void wifi_status_changed_cb(void *user_ctx);
static void close_keyboard(void);
static void lvgl_lock(void);
static void lvgl_unlock(void);

#define LCD_FONT_MOUNT_POINT "/font"
#define LCD_FONT_PARTITION_LABEL "font"
#define LCD_FONT_FILE_NAME "llm_text_14_lazy.bin"
#define LCD_FONT_PATH LCD_FONT_MOUNT_POINT "/" LCD_FONT_FILE_NAME
#define LCD_KB_BTN(width) (LV_BTNMATRIX_CTRL_POPOVER | (width))

static const char *s_kb_map_lower[] = {
  "1#", "q", "w", "e", "r", "t", "y", "u", "i", "o", "p", "Del", "\n",
  "ABC", "a", "s", "d", "f", "g", "h", "j", "k", "l", "Enter", "\n",
  "_", "-", "z", "x", "c", "v", "b", "n", "m", ".", ",", ":", "\n",
  "Hide", "Space", "Del", "OK", ""
};

static const char *s_kb_map_upper[] = {
  "1#", "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", "Del", "\n",
  "abc", "A", "S", "D", "F", "G", "H", "J", "K", "L", "Enter", "\n",
  "_", "-", "Z", "X", "C", "V", "B", "N", "M", ".", ",", ":", "\n",
  "Hide", "Space", "Del", "OK", ""
};

static const char *s_kb_map_special[] = {
  "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "Del", "\n",
  "abc", "+", "&", "/", "*", "=", "%", "!", "?", "#", "<", ">", "\n",
  "\\", "@", "$", "(", ")", "{", "}", "[", "]", ";", "\"", "'", "\n",
  "Hide", "Space", "Del", "OK", ""
};

static const lv_btnmatrix_ctrl_t s_kb_ctrl_text[] = {
  LV_KEYBOARD_CTRL_BTN_FLAGS | 5, LCD_KB_BTN(4), LCD_KB_BTN(4), LCD_KB_BTN(4), LCD_KB_BTN(4),
  LCD_KB_BTN(4), LCD_KB_BTN(4), LCD_KB_BTN(4), LCD_KB_BTN(4), LCD_KB_BTN(4), LCD_KB_BTN(4),
  LV_KEYBOARD_CTRL_BTN_FLAGS | 7,
  LV_KEYBOARD_CTRL_BTN_FLAGS | 6, LCD_KB_BTN(3), LCD_KB_BTN(3), LCD_KB_BTN(3), LCD_KB_BTN(3),
  LCD_KB_BTN(3), LCD_KB_BTN(3), LCD_KB_BTN(3), LCD_KB_BTN(3), LCD_KB_BTN(3),
  LV_KEYBOARD_CTRL_BTN_FLAGS | 7,
  LV_BTNMATRIX_CTRL_CHECKED | LCD_KB_BTN(1), LV_BTNMATRIX_CTRL_CHECKED | LCD_KB_BTN(1),
  LCD_KB_BTN(1), LCD_KB_BTN(1), LCD_KB_BTN(1), LCD_KB_BTN(1), LCD_KB_BTN(1), LCD_KB_BTN(1),
  LCD_KB_BTN(1), LV_BTNMATRIX_CTRL_CHECKED | LCD_KB_BTN(1), LV_BTNMATRIX_CTRL_CHECKED | LCD_KB_BTN(1),
  LV_BTNMATRIX_CTRL_CHECKED | LCD_KB_BTN(1),
  LV_KEYBOARD_CTRL_BTN_FLAGS | 3, LV_BTNMATRIX_CTRL_CHECKED | 8,
  LV_KEYBOARD_CTRL_BTN_FLAGS | 3, LV_KEYBOARD_CTRL_BTN_FLAGS | 3
};

static const lv_btnmatrix_ctrl_t s_kb_ctrl_special[] = {
  LCD_KB_BTN(1), LCD_KB_BTN(1), LCD_KB_BTN(1), LCD_KB_BTN(1), LCD_KB_BTN(1), LCD_KB_BTN(1),
  LCD_KB_BTN(1), LCD_KB_BTN(1), LCD_KB_BTN(1), LCD_KB_BTN(1), LV_KEYBOARD_CTRL_BTN_FLAGS | 2,
  LV_KEYBOARD_CTRL_BTN_FLAGS | 2, LCD_KB_BTN(1), LCD_KB_BTN(1), LCD_KB_BTN(1), LCD_KB_BTN(1),
  LCD_KB_BTN(1), LCD_KB_BTN(1), LCD_KB_BTN(1), LCD_KB_BTN(1), LCD_KB_BTN(1), LCD_KB_BTN(1),
  LCD_KB_BTN(1), LCD_KB_BTN(1),
  LCD_KB_BTN(1), LCD_KB_BTN(1), LCD_KB_BTN(1), LCD_KB_BTN(1), LCD_KB_BTN(1), LCD_KB_BTN(1),
  LCD_KB_BTN(1), LCD_KB_BTN(1), LCD_KB_BTN(1), LCD_KB_BTN(1), LCD_KB_BTN(1), LCD_KB_BTN(1),
  LV_KEYBOARD_CTRL_BTN_FLAGS | 3, LV_BTNMATRIX_CTRL_CHECKED | 8,
  LV_KEYBOARD_CTRL_BTN_FLAGS | 3, LV_KEYBOARD_CTRL_BTN_FLAGS | 3
};

// 返回当前 UI 使用的字体，外置字库加载失败时回退到内置中文字体。
static const lv_font_t *ui_font(void)
{
  return s_external_chinese_font != NULL ? s_external_chinese_font : &lv_font_chinese_16;
}

// 挂载字体分区并加载懒加载中文字库。
static void load_external_font(void)
{
  esp_vfs_spiffs_conf_t conf = {
    .base_path = LCD_FONT_MOUNT_POINT,
    .partition_label = LCD_FONT_PARTITION_LABEL,
    .max_files = 5,
    .format_if_mount_failed = false,
  };

  esp_err_t err = esp_vfs_spiffs_register(&conf);
  if (err == ESP_ERR_INVALID_STATE) {
    // 字体分区可能已由其他路径挂载，视为可继续使用。
    err = ESP_OK;
  }

  if (err != ESP_OK) {
    ESP_LOGW(TAG, "mount font partition failed: %s", esp_err_to_name(err));
    return;
  }

  int64_t start_us = esp_timer_get_time();
  s_external_chinese_font = lazy_font_load(LCD_FONT_PATH, &lv_font_chinese_16);
  if (s_external_chinese_font == NULL) {
    ESP_LOGW(TAG, "load external font failed: %s", LCD_FONT_PATH);
  } else {
    ESP_LOGI(TAG,
             "loaded external font: %s in %lld ms",
             LCD_FONT_PATH,
             (long long)((esp_timer_get_time() - start_us) / 1000));
  }
}

// 将 16 位无符号值限制在指定范围内。
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

// 把触摸原始坐标轴映射到屏幕逻辑坐标范围。
static uint16_t map_touch_axis(uint16_t value,
                               uint16_t in_min,
                               uint16_t in_max,
                               uint16_t out_max)
{
  value = clamp_u16(value, in_min, in_max);
  return (uint32_t)(value - in_min) * out_max / (in_max - in_min);
}

// 将 XPT2046 原始坐标按校准参数转换为 LVGL 坐标。
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
      // 触摸板安装方向和 LCD 横屏方向不一致时交换轴。
      mapped_x = map_touch_axis(raw_y, LCD_TOUCH_RAW_Y_MIN, LCD_TOUCH_RAW_Y_MAX, LCD_H_RES - 1);
      mapped_y = map_touch_axis(raw_x, LCD_TOUCH_RAW_X_MIN, LCD_TOUCH_RAW_X_MAX, LCD_V_RES - 1);
    } else {
      mapped_x = map_touch_axis(raw_x, LCD_TOUCH_RAW_X_MIN, LCD_TOUCH_RAW_X_MAX, LCD_H_RES - 1);
      mapped_y = map_touch_axis(raw_y, LCD_TOUCH_RAW_Y_MIN, LCD_TOUCH_RAW_Y_MAX, LCD_V_RES - 1);
    }

    if (LCD_TOUCH_MIRROR_X) {
      // 按实际贴合方向镜像 X 轴。
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

// LVGL 异步回调：切回首页。
static void async_show_home_page(void *arg)
{
  (void)arg;
  show_home_page();
}

// LVGL 异步回调：打开 WiFi 列表页。
static void async_show_wifi_page(void *arg)
{
  (void)arg;
  show_wifi_page();
}

// LVGL 异步回调：打开指定 SSID 的连接页。
static void async_show_wifi_connect_page(void *arg)
{
  (void)arg;
  show_wifi_connect_page(s_selected_ssid);
}

// LVGL 不是线程安全的，所有对象和定时器操作都必须共用这把锁。
static void lvgl_lock(void)
{
  if (s_lvgl_mutex != NULL) {
    xSemaphoreTake(s_lvgl_mutex, portMAX_DELAY);
  }
}

// 释放 LVGL 操作锁。
static void lvgl_unlock(void)
{
  if (s_lvgl_mutex != NULL) {
    xSemaphoreGive(s_lvgl_mutex);
  }
}

// esp_lcd 异步颜色传输完成后调用，用于通知 LVGL 刷新结束。
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

// LVGL 显示刷新回调：把脏矩形像素提交给 LCD 面板。
static void lcd_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map)
{
  esp_lcd_panel_handle_t panel_handle = (esp_lcd_panel_handle_t)drv->user_data;
  // LVGL 坐标右下角是闭区间，esp_lcd 绘制接口要求结束坐标为开区间。
  int x_start = area->x1;
  int x_end = area->x2 + 1;
  int y_start = area->y1;
  int y_end = area->y2 + 1;

  esp_lcd_panel_draw_bitmap(panel_handle, x_start, y_start, x_end, y_end, color_map);
}

// 周期性推进 LVGL 内部时基。
static void lvgl_tick_cb(void *arg)
{
  (void)arg;
  // 让 LVGL 时基独立于主任务运行。
  lv_tick_inc(2);
}

// FreeRTOS 空闲钩子：统计每个 CPU 核心的空闲次数。
static bool cpu_idle_hook(void)
{
  int core_id = xPortGetCoreID();
  if (core_id >= 0 && core_id < 2) {
    s_cpu_idle_count[core_id]++;
  }
  return true;
}

// 根据空闲计数增量估算 CPU 使用率。
static uint8_t cpu_usage_from_idle_delta(uint32_t core_id, uint32_t idle_count)
{
  uint32_t delta = idle_count - s_cpu_idle_last[core_id];
  s_cpu_idle_last[core_id] = idle_count;

  if (delta > s_cpu_idle_max[core_id]) {
    // 记录观测到的最大空闲增量，作为近似 0% 使用率基线。
    s_cpu_idle_max[core_id] = delta;
  }
  if (s_cpu_idle_max[core_id] == 0) {
    return 0;
  }

  uint32_t idle_percent = (delta * 100U) / s_cpu_idle_max[core_id];
  if (idle_percent > 100U) {
    idle_percent = 100U;
  }
  return (uint8_t)(100U - idle_percent);
}

// 刷新首页或 WiFi 页底部的 CPU 使用率标签。
static void update_cpu_usage_label(void)
{
  if (s_cpu_usage_label == NULL) {
    return;
  }

  uint8_t cpu0 = cpu_usage_from_idle_delta(0, s_cpu_idle_count[0]);
  uint8_t cpu1 = cpu_usage_from_idle_delta(1, s_cpu_idle_count[1]);

  char text[32];
  snprintf(text, sizeof(text), "CPU0 %u%%  CPU1 %u%%", cpu0, cpu1);
  lv_label_set_text(s_cpu_usage_label, text);
}

// LVGL 定时器回调：周期刷新 CPU 使用率。
static void cpu_usage_timer_cb(lv_timer_t *timer)
{
  (void)timer;
  update_cpu_usage_label();
}

// 创建带统一字体和颜色的 LVGL 标签。
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

// 创建 CPU 使用率标签；键盘打开时上移避免遮挡。
static void create_cpu_usage_label(bool above_keyboard)
{
  const esp_app_desc_t *app_desc = esp_app_get_description();
  char version_text[32];
  snprintf(version_text, sizeof(version_text), "版本 %s", app_desc != NULL ? app_desc->version : "--");

  s_app_version_label = create_label(lv_scr_act(), version_text, ui_font());
  if (s_app_version_label != NULL) {
    lv_obj_set_width(s_app_version_label, 150);
    lv_obj_set_style_text_color(s_app_version_label, lv_color_hex(0x64748b), 0);
    lv_obj_set_style_text_align(s_app_version_label, LV_TEXT_ALIGN_LEFT, 0);
    lv_label_set_long_mode(s_app_version_label, LV_LABEL_LONG_CLIP);
    lv_obj_align(s_app_version_label, LV_ALIGN_BOTTOM_LEFT, 6, above_keyboard ? -120 : -22);
    lv_obj_move_foreground(s_app_version_label);
  }

  s_cpu_usage_label = create_label(lv_scr_act(), "CPU0 --%  CPU1 --%", ui_font());
  if (s_cpu_usage_label == NULL) {
    return;
  }

  lv_obj_set_width(s_cpu_usage_label, 150);
  lv_obj_set_style_text_color(s_cpu_usage_label, lv_color_hex(0x475569), 0);
  lv_obj_set_style_text_align(s_cpu_usage_label, LV_TEXT_ALIGN_LEFT, 0);
  lv_label_set_long_mode(s_cpu_usage_label, LV_LABEL_LONG_CLIP);
  lv_obj_align(s_cpu_usage_label, LV_ALIGN_BOTTOM_LEFT, 6, above_keyboard ? -102 : -4);
  lv_obj_move_foreground(s_cpu_usage_label);
  update_cpu_usage_label();
}

// 创建统一样式的蓝色操作按钮。
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

// 创建顶部导航栏容器。
static lv_obj_t *create_nav_bar(void)
{
  lv_obj_t *nav = lv_obj_create(lv_scr_act());
  if (nav == NULL) {
    return NULL;
  }

  lv_obj_set_size(nav, LCD_H_RES, 42);
  lv_obj_align(nav, LV_ALIGN_TOP_MID, 0, 0);
  lv_obj_clear_flag(nav, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_radius(nav, 0, 0);
  lv_obj_set_style_border_side(nav, LV_BORDER_SIDE_BOTTOM, 0);
  lv_obj_set_style_border_width(nav, 1, 0);
  lv_obj_set_style_border_color(nav, lv_color_black(), 0);
  lv_obj_set_style_bg_color(nav, lv_color_white(), 0);
  lv_obj_set_style_pad_all(nav, 0, 0);

  return nav;
}

// 创建导航栏中的文字按钮。
static lv_obj_t *create_nav_label_button(lv_obj_t *parent,
                                         const char *text,
                                         lv_coord_t width,
                                         lv_coord_t height,
                                         lv_event_cb_t cb,
                                         void *user_data)
{
  lv_obj_t *button = lv_obj_create(parent);
  if (button == NULL) {
    return NULL;
  }

  lv_obj_set_size(button, width, height);
  lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(button, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_radius(button, 4, 0);
  lv_obj_set_style_border_width(button, 0, 0);
  lv_obj_set_style_bg_opa(button, LV_OPA_TRANSP, 0);
  lv_obj_set_style_bg_color(button, lv_color_hex(0xe5e7eb), LV_STATE_PRESSED);
  lv_obj_set_style_bg_opa(button, LV_OPA_COVER, LV_STATE_PRESSED);
  lv_obj_set_style_pad_all(button, 0, 0);
  if (cb != NULL) {
    lv_obj_add_event_cb(button, cb, LV_EVENT_CLICKED, user_data);
  }

  lv_obj_t *label = create_label(button, text, ui_font());
  if (label != NULL) {
    lv_obj_set_style_text_color(label, lv_color_black(), 0);
    lv_obj_center(label);
  }

  return button;
}

// 统一设置屏幕键盘的尺寸、颜色和按键样式。
static void style_keyboard(lv_obj_t *keyboard)
{
  if (keyboard == NULL) {
    return;
  }

  lv_obj_set_style_text_font(keyboard, &lv_font_chinese_16, LV_PART_ITEMS);
  lv_obj_set_style_text_color(keyboard, lv_color_hex(0x111827), LV_PART_ITEMS);
  lv_obj_set_style_bg_color(keyboard, lv_color_hex(0xf8fafc), 0);
  lv_obj_set_style_bg_opa(keyboard, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(keyboard, 0, 0);
  lv_obj_set_style_pad_all(keyboard, 3, 0);
  lv_obj_set_style_pad_row(keyboard, 3, 0);
  lv_obj_set_style_pad_column(keyboard, 3, 0);
  lv_obj_set_style_radius(keyboard, 0, 0);
  lv_obj_set_style_radius(keyboard, 4, LV_PART_ITEMS);
  lv_obj_set_style_bg_color(keyboard, lv_color_white(), LV_PART_ITEMS);
  lv_obj_set_style_bg_color(keyboard, lv_color_hex(0xe5e7eb), LV_PART_ITEMS | LV_STATE_CHECKED);
  lv_obj_set_style_bg_color(keyboard, lv_color_hex(0xdbeafe), LV_PART_ITEMS | LV_STATE_PRESSED);
  lv_obj_set_style_border_width(keyboard, 1, LV_PART_ITEMS);
  lv_obj_set_style_border_color(keyboard, lv_color_hex(0xcbd5e1), LV_PART_ITEMS);
}

// LVGL 异步回调：关闭屏幕键盘。
static void async_close_keyboard(void *arg)
{
  (void)arg;
  close_keyboard();
}

// 自定义键盘按键事件，处理大小写、删除、空格和提交。
static void keyboard_event_cb(lv_event_t *event)
{
  if (lv_event_get_code(event) != LV_EVENT_VALUE_CHANGED) {
    return;
  }

  lv_obj_t *keyboard = lv_event_get_target(event);
  uint16_t btn_id = lv_btnmatrix_get_selected_btn(keyboard);
  if (btn_id == LV_BTNMATRIX_BTN_NONE) {
    return;
  }

  const char *txt = lv_btnmatrix_get_btn_text(keyboard, btn_id);
  if (txt == NULL) {
    return;
  }

  if (strcmp(txt, "abc") == 0) {
    // 模式切换键不向文本框输入字符。
    lv_keyboard_set_mode(keyboard, LV_KEYBOARD_MODE_TEXT_LOWER);
    return;
  }
  if (strcmp(txt, "ABC") == 0) {
    lv_keyboard_set_mode(keyboard, LV_KEYBOARD_MODE_TEXT_UPPER);
    return;
  }
  if (strcmp(txt, "1#") == 0) {
    lv_keyboard_set_mode(keyboard, LV_KEYBOARD_MODE_SPECIAL);
    return;
  }
  if (strcmp(txt, "Hide") == 0) {
    lv_async_call(async_close_keyboard, NULL);
    return;
  }

  lv_obj_t *textarea = lv_keyboard_get_textarea(keyboard);
  if (textarea == NULL) {
    return;
  }

  if (strcmp(txt, "OK") == 0) {
    // OK 表示密码输入完成，触发文本框 ready 事件并收起键盘。
    lv_event_send(textarea, LV_EVENT_READY, NULL);
    lv_async_call(async_close_keyboard, NULL);
  } else if (strcmp(txt, "Enter") == 0) {
    lv_textarea_add_char(textarea, '\n');
    if (lv_textarea_get_one_line(textarea)) {
      lv_event_send(textarea, LV_EVENT_READY, NULL);
      lv_async_call(async_close_keyboard, NULL);
    }
  } else if (strcmp(txt, "Del") == 0) {
    lv_textarea_del_char(textarea);
  } else if (strcmp(txt, "Space") == 0) {
    lv_textarea_add_char(textarea, ' ');
  } else {
    lv_textarea_add_text(textarea, txt);
  }
}

// 替换 LVGL 默认键盘映射，接入本项目的紧凑键盘布局。
static void setup_keyboard(lv_obj_t *keyboard)
{
  if (keyboard == NULL) {
    return;
  }

  lv_obj_remove_event_cb(keyboard, lv_keyboard_def_event_cb);
  lv_obj_add_event_cb(keyboard, keyboard_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
  lv_keyboard_set_map(keyboard, LV_KEYBOARD_MODE_TEXT_LOWER, s_kb_map_lower, s_kb_ctrl_text);
  lv_keyboard_set_map(keyboard, LV_KEYBOARD_MODE_TEXT_UPPER, s_kb_map_upper, s_kb_ctrl_text);
  lv_keyboard_set_map(keyboard, LV_KEYBOARD_MODE_SPECIAL, s_kb_map_special, s_kb_ctrl_special);
  lv_keyboard_set_mode(keyboard, LV_KEYBOARD_MODE_TEXT_LOWER);
  style_keyboard(keyboard);
}

// 将 RSSI 粗略映射为 1-3 格信号强度。
static int wifi_signal_level_from_rssi(int8_t rssi)
{
  if (rssi >= -60) {
    return 3;
  }
  if (rssi >= -75) {
    return 2;
  }
  return 1;
}

// 绘制 WiFi 图标中的三格信号柱。
static void create_signal_bars(lv_obj_t *parent, int level, lv_color_t active_color)
{
  static const lv_coord_t bar_heights[] = {6, 11, 16};

  for (int i = 0; i < 3; ++i) {
    lv_obj_t *bar = lv_obj_create(parent);
    if (bar == NULL) {
      continue;
    }

    lv_obj_set_size(bar, 5, bar_heights[i]);
    lv_obj_align(bar, LV_ALIGN_BOTTOM_LEFT, 3 + (i * 8), -2);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(bar, 2, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(bar,
                              i < level ? active_color : lv_color_hex(0xcbd5e1),
                              0);
  }
}

// 根据连接状态绘制 WiFi 图标：连接中显示 spinner，未连接显示红点。
static lv_obj_t *create_wifi_icon(lv_obj_t *parent, int level, bool connecting)
{
  lv_obj_t *icon = lv_obj_create(parent);
  if (icon == NULL) {
    return NULL;
  }

  lv_obj_set_size(icon, 30, 22);
  lv_obj_clear_flag(icon, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_border_width(icon, 0, 0);
  lv_obj_set_style_radius(icon, 0, 0);
  lv_obj_set_style_pad_all(icon, 0, 0);
  lv_obj_set_style_bg_opa(icon, LV_OPA_TRANSP, 0);

  if (connecting) {
    // 连接中用 spinner 表示异步流程仍在进行。
    lv_obj_t *spinner = lv_spinner_create(icon, 1000, 70);
    if (spinner != NULL) {
      lv_obj_set_size(spinner, 20, 20);
      lv_obj_center(spinner);
      lv_obj_set_style_arc_color(spinner, lv_color_hex(0xcbd5e1), LV_PART_MAIN);
      lv_obj_set_style_arc_color(spinner, lv_color_hex(0x2563eb), LV_PART_INDICATOR);
      lv_obj_set_style_arc_width(spinner, 3, LV_PART_MAIN);
      lv_obj_set_style_arc_width(spinner, 3, LV_PART_INDICATOR);
    }
    return icon;
  }

  if (level <= 0) {
    // 无连接或无信号时显示灰色信号柱和红色标记。
    create_signal_bars(icon, 0, lv_color_hex(0x94a3b8));

    lv_obj_t *mark = lv_obj_create(icon);
    if (mark != NULL) {
      lv_obj_set_size(mark, 7, 7);
      lv_obj_clear_flag(mark, LV_OBJ_FLAG_SCROLLABLE);
      lv_obj_set_style_radius(mark, 4, 0);
      lv_obj_set_style_border_width(mark, 0, 0);
      lv_obj_set_style_bg_color(mark, lv_color_hex(0xdc2626), 0);
      lv_obj_set_style_bg_opa(mark, LV_OPA_COVER, 0);
      lv_obj_align(mark, LV_ALIGN_RIGHT_MID, 0, -1);
    }
    return icon;
  }

  create_signal_bars(icon, level, lv_color_hex(0x16a34a));
  return icon;
}

// 根据当前 WiFi 状态在指定父对象中创建图标。
static void create_current_wifi_icon(lv_obj_t *parent)
{
  app_wifi_status_t status = {0};
  app_wifi_get_status(&status);

  lv_obj_t *icon = NULL;
  if (status.connected) {
    icon = create_wifi_icon(parent, wifi_signal_level_from_rssi(status.rssi), false);
  } else if (status.connecting) {
    icon = create_wifi_icon(parent, 0, true);
  } else {
    icon = create_wifi_icon(parent, 0, false);
  }

  if (icon != NULL) {
    lv_obj_center(icon);
  }
}

// 刷新首页右上角 WiFi 状态图标。
static void update_home_wifi_icon(void)
{
  if (s_home_wifi_button == NULL) {
    return;
  }

  lv_obj_clean(s_home_wifi_button);
  create_current_wifi_icon(s_home_wifi_button);
}

// 首页定时器回调：周期刷新 WiFi 图标。
static void home_wifi_timer_cb(lv_timer_t *timer)
{
  (void)timer;
  update_home_wifi_icon();
}

// 停止首页 WiFi 图标刷新定时器，并清理按钮引用。
static void stop_home_wifi_timer(void)
{
  if (s_home_wifi_timer != NULL) {
    lv_timer_del(s_home_wifi_timer);
    s_home_wifi_timer = NULL;
  }
  s_home_wifi_button = NULL;
}

// WiFi 状态变化回调：同步刷新首页图标和 WiFi 页面状态。
static void wifi_status_changed_cb(void *user_ctx)
{
  (void)user_ctx;

  lvgl_lock();
  if (s_home_wifi_button != NULL) {
    update_home_wifi_icon();
  }
  if (s_wifi_status_label != NULL || s_wifi_status_icon != NULL) {
    update_wifi_status_text();
  }
  lvgl_unlock();
}

// 创建 WiFi 扫描结果列表行。
static lv_obj_t *create_list_row(lv_obj_t *parent,
                                 const char *text,
                                 int signal_level,
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

  lv_obj_t *icon = create_wifi_icon(row, signal_level, false);
  if (icon != NULL) {
    lv_obj_align(icon, LV_ALIGN_LEFT_MID, 0, 0);
  }

  lv_obj_t *label = create_label(row, text, ui_font());
  if (label != NULL) {
    lv_obj_set_width(label, width - 48);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 34, 0);
  }

  return row;
}

// 清空当前屏幕并恢复默认背景。
static void clear_screen(void)
{
  lv_obj_clean(lv_scr_act());
  lv_obj_set_style_bg_color(lv_scr_act(), lv_color_hex(0xf8fafc), 0);
  lv_obj_set_style_bg_opa(lv_scr_act(), LV_OPA_COVER, 0);
  s_app_version_label = NULL;
  s_cpu_usage_label = NULL;
}

// 设置当前屏幕背景色。
static void set_screen_bg(lv_color_t color)
{
  lv_obj_set_style_bg_color(lv_scr_act(), color, 0);
  lv_obj_set_style_bg_opa(lv_scr_act(), LV_OPA_COVER, 0);
}

// 首页 WiFi 图标点击后进入 WiFi 页面。
static void home_wifi_event_cb(lv_event_t *event)
{
  if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
    lv_async_call(async_show_wifi_page, NULL);
  }
}

// 刷新首页音量数字。
static void update_home_volume_label(void)
{
  if (s_home_volume_label == NULL) {
    return;
  }

  char text[8];
  snprintf(text, sizeof(text), "%d", audio_get_volume_level());
  lv_label_set_text(s_home_volume_label, text);
}

// 首页音量减按钮事件。
static void home_volume_down_event_cb(lv_event_t *event)
{
  if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
    audio_volume_down();
    update_home_volume_label();
  }
}

// 首页音量加按钮事件。
static void home_volume_up_event_cb(lv_event_t *event)
{
  if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
    audio_volume_up();
    update_home_volume_label();
  }
}

// WiFi 页返回按钮事件。
static void wifi_back_event_cb(lv_event_t *event)
{
  if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
    lv_async_call(async_show_home_page, NULL);
  }
}

// WiFi 页刷新按钮事件。
static void wifi_refresh_event_cb(lv_event_t *event)
{
  if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
    lv_async_call(async_show_wifi_page, NULL);
  }
}

// WiFi 连接页返回按钮事件。
static void connect_back_event_cb(lv_event_t *event)
{
  if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
    lv_async_call(async_show_wifi_page, NULL);
  }
}

// 扫描按钮事件：启动一次新的 WiFi 扫描。
static void wifi_scan_event_cb(lv_event_t *event)
{
  if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
    start_wifi_scan();
  }
}

// 删除屏幕键盘并清空全局引用。
static void close_keyboard(void)
{
  if (s_keyboard != NULL) {
    lv_obj_del(s_keyboard);
    s_keyboard = NULL;
  }
}

// 密码输入框获得焦点时创建键盘。
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
    setup_keyboard(s_keyboard);
    lv_keyboard_set_textarea(s_keyboard, s_password_ta);
  }
}

// 后台连接任务：避免 WiFi 连接阻塞 LVGL 线程。
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
    // 最多轮询约 10 秒，期间持续刷新页面上的连接状态。
    lvgl_lock();
    update_wifi_status_text();
    lvgl_unlock();

    app_wifi_status_t status = {0};
    app_wifi_get_status(&status);
    if (status.connected || (!status.connecting && i > 1)) {
      // 成功连接或已经明确失败后提前结束等待。
      break;
    }
    vTaskDelay(pdMS_TO_TICKS(500));
  }

  lvgl_lock();
  show_wifi_page();
  lvgl_unlock();
  vTaskDelete(NULL);
}

// 连接按钮事件：读取密码并创建连接任务。
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

// 扫描列表行点击事件：记录 SSID 并跳转到密码输入页。
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

// 根据当前 WiFi 状态刷新状态文案和图标。
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
  update_wifi_status_icon();
}

// 刷新 WiFi 页面状态图标容器。
static void update_wifi_status_icon(void)
{
  if (s_wifi_status_icon == NULL) {
    return;
  }

  lv_obj_clean(s_wifi_status_icon);

  create_current_wifi_icon(s_wifi_status_icon);
}

// 在已持有 LVGL 锁的前提下刷新扫描结果列表。
static void show_scan_results_locked(void)
{
  if (s_wifi_list == NULL) {
    return;
  }

  lv_obj_clean(s_wifi_list);
  if (s_scan_count == 0) {
    // 扫描成功但没有 AP 时给用户明确反馈。
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
                                    wifi_signal_level_from_rssi(s_scan_results[i].rssi),
                                    LCD_H_RES - 32,
                                    34,
                                    ssid_event_cb,
                                    s_scan_results[i].ssid);
    if (row != NULL) {
      lv_obj_set_scroll_dir(row, LV_DIR_VER);
    }
  }
}

// 后台扫描任务：执行阻塞扫描并回到 LVGL 线程刷新列表。
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

// 启动 WiFi 扫描，并先在列表区域显示加载状态。
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

// 构建首页：标题、WiFi 入口、对话文本、音量控制和 CPU 标签。
static void show_home_page(void)
{
  stop_home_wifi_timer();
  close_keyboard();
  // 切页前清空页面对象引用，避免回调访问已删除控件。
  s_wifi_status_label = NULL;
  s_wifi_status_icon = NULL;
  s_wifi_list = NULL;
  s_password_ta = NULL;
  s_home_volume_label = NULL;
  s_home_question_label = NULL;
  s_home_reply_label = NULL;

  clear_screen();
  set_screen_bg(lv_color_white());

  lv_obj_t *nav = create_nav_bar();
  if (nav != NULL) {
    lv_obj_t *title = create_label(nav, "ESP32 智能助手", ui_font());
    if (title != NULL) {
      lv_obj_align(title, LV_ALIGN_LEFT_MID, 12, 0);
    }

    s_home_wifi_button = lv_obj_create(nav);
    if (s_home_wifi_button != NULL) {
      lv_obj_set_size(s_home_wifi_button, 44, 34);
      lv_obj_align(s_home_wifi_button, LV_ALIGN_RIGHT_MID, -8, 0);
      lv_obj_add_flag(s_home_wifi_button, LV_OBJ_FLAG_CLICKABLE);
      lv_obj_clear_flag(s_home_wifi_button, LV_OBJ_FLAG_SCROLLABLE);
      lv_obj_set_style_radius(s_home_wifi_button, 6, 0);
      lv_obj_set_style_border_width(s_home_wifi_button, 0, 0);
      lv_obj_set_style_bg_opa(s_home_wifi_button, LV_OPA_TRANSP, 0);
      lv_obj_set_style_bg_color(s_home_wifi_button, lv_color_hex(0xe0f2fe), LV_STATE_PRESSED);
      lv_obj_add_event_cb(s_home_wifi_button, home_wifi_event_cb, LV_EVENT_CLICKED, NULL);
      update_home_wifi_icon();
      s_home_wifi_timer = lv_timer_create(home_wifi_timer_cb, 30000, NULL);
    }
  }

  s_home_question_label = create_label(lv_scr_act(), s_home_question_text, ui_font());
  if (s_home_question_label != NULL) {
    lv_obj_set_width(s_home_question_label, LCD_H_RES - 32);
    lv_obj_set_style_text_align(s_home_question_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_home_question_label, lv_color_hex(0x2563eb), 0);
    lv_label_set_long_mode(s_home_question_label, LV_LABEL_LONG_WRAP);
    lv_obj_align(s_home_question_label, LV_ALIGN_CENTER, 0, -32);
  }

  s_home_reply_label = create_label(lv_scr_act(), s_home_reply_text, ui_font());
  if (s_home_reply_label != NULL) {
    lv_obj_set_width(s_home_reply_label, LCD_H_RES - 32);
    lv_obj_set_style_text_align(s_home_reply_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_home_reply_label,
                                s_home_reply_thinking ? lv_color_hex(0x64748b) : lv_color_hex(0x16a34a),
                                0);
    lv_label_set_long_mode(s_home_reply_label, LV_LABEL_LONG_WRAP);
    lv_obj_align(s_home_reply_label, LV_ALIGN_CENTER, 0, 22);
  }

  lv_obj_t *volume_panel = lv_obj_create(lv_scr_act());
  if (volume_panel != NULL) {
    lv_obj_set_size(volume_panel, 128, 38);
    lv_obj_align(volume_panel, LV_ALIGN_BOTTOM_RIGHT, -8, -8);
    lv_obj_clear_flag(volume_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(volume_panel, 0, 0);
    lv_obj_set_style_border_width(volume_panel, 0, 0);
    lv_obj_set_style_bg_opa(volume_panel, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(volume_panel, 0, 0);

    lv_obj_t *volume_down_button = create_button(volume_panel, "-", 38, 38, home_volume_down_event_cb, NULL);
    if (volume_down_button != NULL) {
      lv_obj_align(volume_down_button, LV_ALIGN_LEFT_MID, 0, 0);
    }

    s_home_volume_label = create_label(volume_panel, "", ui_font());
    if (s_home_volume_label != NULL) {
      lv_obj_set_width(s_home_volume_label, 44);
      lv_obj_set_style_text_align(s_home_volume_label, LV_TEXT_ALIGN_CENTER, 0);
      lv_obj_align(s_home_volume_label, LV_ALIGN_CENTER, 0, 0);
      update_home_volume_label();
    }

    lv_obj_t *volume_up_button = create_button(volume_panel, "+", 38, 38, home_volume_up_event_cb, NULL);
    if (volume_up_button != NULL) {
      lv_obj_align(volume_up_button, LV_ALIGN_RIGHT_MID, 0, 0);
    }
  }

  create_cpu_usage_label(false);
}

// 构建 WiFi 密码输入页。
static void show_wifi_connect_page(const char *ssid)
{
  stop_home_wifi_timer();
  close_keyboard();
  s_wifi_status_label = NULL;
  s_wifi_status_icon = NULL;
  s_wifi_list = NULL;
  s_password_ta = NULL;

  clear_screen();

  lv_obj_t *header = create_nav_bar();
  if (header != NULL) {
    lv_obj_t *back = create_nav_label_button(header, "返回", 72, 30, connect_back_event_cb, NULL);
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
    lv_obj_set_size(s_password_ta, LCD_H_RES - 24, 34);
    lv_obj_align(s_password_ta, LV_ALIGN_TOP_MID, 0, 76);
    lv_obj_set_style_text_font(s_password_ta, ui_font(), 0);
    lv_obj_set_style_text_font(s_password_ta, ui_font(), LV_PART_TEXTAREA_PLACEHOLDER);
    lv_obj_set_style_text_color(s_password_ta, lv_color_hex(0x64748b), LV_PART_TEXTAREA_PLACEHOLDER);
    lv_textarea_set_one_line(s_password_ta, true);
    lv_textarea_set_password_mode(s_password_ta, false);
    lv_textarea_set_placeholder_text(s_password_ta, "请输入密码");
    lv_obj_add_event_cb(s_password_ta, textarea_event_cb, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_state(s_password_ta, LV_STATE_FOCUSED);
  }

  lv_obj_t *connect = create_button(lv_scr_act(), "连接", LCD_H_RES - 24, 28, connect_event_cb, NULL);
  if (connect != NULL) {
    lv_obj_align(connect, LV_ALIGN_TOP_MID, 0, 114);
  }

  if (s_password_ta != NULL) {
    s_keyboard = lv_keyboard_create(lv_scr_act());
    if (s_keyboard != NULL) {
      lv_obj_set_size(s_keyboard, LCD_H_RES, 96);
      lv_obj_align(s_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
      setup_keyboard(s_keyboard);
      lv_keyboard_set_textarea(s_keyboard, s_password_ta);
    }
  }

  create_cpu_usage_label(true);
}

// 构建 WiFi 状态和扫描列表页。
static void show_wifi_page(void)
{
  stop_home_wifi_timer();
  close_keyboard();
  s_password_ta = NULL;
  clear_screen();

  lv_obj_t *header = create_nav_bar();
  if (header != NULL) {
    lv_obj_t *back = create_nav_label_button(header, "返回", 72, 30, wifi_back_event_cb, NULL);
    if (back != NULL) {
      lv_obj_align(back, LV_ALIGN_LEFT_MID, 4, 0);
    }

    lv_obj_t *title = create_label(header, "WiFi", ui_font());
    if (title != NULL) {
      lv_obj_align(title, LV_ALIGN_CENTER, 0, 0);
    }

    lv_obj_t *refresh = create_nav_label_button(header, "刷新", 86, 30, wifi_refresh_event_cb, NULL);
    if (refresh != NULL) {
      lv_obj_align(refresh, LV_ALIGN_RIGHT_MID, -4, 0);
    }
  }

  s_wifi_status_label = create_label(lv_scr_act(), "", ui_font());
  s_wifi_status_icon = lv_obj_create(lv_scr_act());
  if (s_wifi_status_icon != NULL) {
    lv_obj_set_size(s_wifi_status_icon, 36, 28);
    lv_obj_align(s_wifi_status_icon, LV_ALIGN_TOP_LEFT, 10, 54);
    lv_obj_clear_flag(s_wifi_status_icon, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_border_width(s_wifi_status_icon, 0, 0);
    lv_obj_set_style_radius(s_wifi_status_icon, 0, 0);
    lv_obj_set_style_pad_all(s_wifi_status_icon, 0, 0);
    lv_obj_set_style_bg_opa(s_wifi_status_icon, LV_OPA_TRANSP, 0);
  }
  if (s_wifi_status_label != NULL) {
    lv_obj_set_width(s_wifi_status_label, LCD_H_RES - 58);
    lv_label_set_long_mode(s_wifi_status_label, LV_LABEL_LONG_WRAP);
    lv_obj_align(s_wifi_status_label, LV_ALIGN_TOP_LEFT, 50, 52);
  }
  update_wifi_status_text();

  s_wifi_list = lv_obj_create(lv_scr_act());
  if (s_wifi_list != NULL) {
    lv_obj_set_size(s_wifi_list, LCD_H_RES - 16, 88);
    lv_obj_align(s_wifi_list, LV_ALIGN_BOTTOM_MID, 0, -28);
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
    // 已连接或连接中时不自动扫描，只提供手动重新扫描入口。
    if (s_wifi_list != NULL) {
      create_button(s_wifi_list, "重新扫描", LCD_H_RES - 32, 36, wifi_scan_event_cb, NULL);
    }
  } else {
    start_wifi_scan();
  }

  create_cpu_usage_label(false);
}

// LVGL 触摸输入回调：读取 XPT2046 坐标并转换成按下/松开状态。
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
    // 读取失败时保持松开状态，避免产生幽灵触摸。
    return;
  }

  if (esp_lcd_touch_get_coordinates(touch_handle, x, y, strength, &count, 1) && count > 0) {
    data->point.x = x[0];
    data->point.y = y[0];
    data->state = LV_INDEV_STATE_PR;
  }
}

// LVGL 主循环任务：周期执行 timer handler。
static void lvgl_task(void *arg)
{
  (void)arg;

  while (1) {
    lvgl_lock();
    uint32_t task_delay_ms = lv_timer_handler();
    lvgl_unlock();
    if (task_delay_ms > 20) {
      // 限制最大休眠时间，保证 UI 交互响应。
      task_delay_ms = 20;
    }
    if (task_delay_ms < 5) {
      task_delay_ms = 5;
    }
    vTaskDelay(pdMS_TO_TICKS(task_delay_ms));
  }
}

// 初始化背光 GPIO，面板准备好之前保持熄灭。
static esp_err_t lcd_backlight_init(void)
{
  // 面板初始化完成前关闭背光，避免用户看到上电噪声。
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

// 初始化 SPI 总线、ILI9341 面板并打开背光。
static esp_err_t lcd_panel_init(void)
{
  // 使用 ESP32-S3 原生 IO_MUX 引脚配置 SPI2 显示总线。
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

  // 面板 IO 负责把 esp_lcd 命令和像素传输转换成 SPI 事务。
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

  // 多数 2.8 寸 ILI9341 模块使用 BGR 顺序；颜色异常时可从这里调整。
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
  // 将物理 240x320 屏旋转为横屏 320x240 使用。
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

// 初始化 XPT2046 触摸控制器并注册到 LVGL 输入系统。
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

// 初始化 LVGL、显示缓冲、触摸输入、tick 定时器和 UI 任务。
static esp_err_t lvgl_port_init(void)
{
  lv_init();
  load_external_font();

  s_lvgl_mutex = xSemaphoreCreateMutex();
  ESP_RETURN_ON_FALSE(s_lvgl_mutex, ESP_ERR_NO_MEM, TAG, "create LVGL mutex failed");

  // 两个 DMA 可访问的局部绘制缓冲，让 LVGL 渲染和 SPI 发送可以流水执行。
  size_t draw_buffer_size = LCD_H_RES * LCD_DRAW_BUF_LINES;
  lv_color_t *buf1 = heap_caps_malloc(draw_buffer_size * sizeof(lv_color_t),
                                      MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  lv_color_t *buf2 = heap_caps_malloc(draw_buffer_size * sizeof(lv_color_t),
                                      MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  ESP_RETURN_ON_FALSE(buf1 && buf2, ESP_ERR_NO_MEM, TAG, "allocate LVGL draw buffers failed");

  lv_disp_draw_buf_init(&s_disp_buf, buf1, buf2, draw_buffer_size);

  // 将 ILI9341 面板注册为 LVGL 默认显示器。
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

  esp_err_t err = esp_register_freertos_idle_hook_for_cpu(cpu_idle_hook, 0);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "register CPU0 idle hook failed: %s", esp_err_to_name(err));
  }
#if portNUM_PROCESSORS > 1
  err = esp_register_freertos_idle_hook_for_cpu(cpu_idle_hook, 1);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "register CPU1 idle hook failed: %s", esp_err_to_name(err));
  }
#endif

  s_cpu_usage_timer = lv_timer_create(cpu_usage_timer_cb, 1000, NULL);
  ESP_RETURN_ON_FALSE(s_cpu_usage_timer != NULL, ESP_ERR_NO_MEM, TAG, "create CPU usage timer failed");

  // LVGL 任务负责运行定时器处理器；其他任务改 UI 前必须先加锁。
  BaseType_t ret = xTaskCreate(lvgl_task, "lvgl", 8192, NULL, 5, &s_lvgl_task_handle);
  ESP_RETURN_ON_FALSE(ret == pdPASS, ESP_ERR_NO_MEM, TAG, "create LVGL task failed");

  return ESP_OK;
}

// 初始化 LCD、LVGL、WiFi 状态回调，并显示首页。
esp_err_t lcd_init(void)
{
  ESP_LOGI(TAG, "initialize ILI9341 on SPI2");
  ESP_RETURN_ON_ERROR(lcd_backlight_init(), TAG, "backlight init failed");
  ESP_RETURN_ON_ERROR(lcd_panel_init(), TAG, "panel init failed");
  ESP_RETURN_ON_ERROR(lvgl_port_init(), TAG, "LVGL init failed");
  audio_set_pcm_playback_text_cb(lcd_show_text);
  app_wifi_set_status_changed_cb(wifi_status_changed_cb, NULL);
  ESP_RETURN_ON_ERROR(app_wifi_init(), TAG, "WiFi init failed");

  s_lcd_ready = true;
  lvgl_lock();
  show_home_page();
  lv_obj_invalidate(lv_scr_act());
  lvgl_unlock();

  return ESP_OK;
}

// 显示助手回复文本，并结束“思考中”状态。
void lcd_show_text(const char *text)
{
  if (!s_lcd_ready) {
    // LCD 尚未初始化时忽略外部显示请求。
    return;
  }

  strlcpy(s_home_reply_text, text != NULL ? text : "", sizeof(s_home_reply_text));
  s_home_reply_thinking = false;

  lvgl_lock();
  show_home_page();
  if (s_home_reply_label != NULL) {
    lv_label_set_text(s_home_reply_label, s_home_reply_text);
    lv_obj_set_style_text_color(s_home_reply_label, lv_color_hex(0x16a34a), 0);
  }
  lvgl_unlock();
}

// 显示“正在说话”状态，提示正在采集用户语音。
void lcd_show_user_speaking(void)
{
  if (!s_lcd_ready) {
    return;
  }

  strlcpy(s_home_question_text, "正在说话", sizeof(s_home_question_text));
  s_home_reply_text[0] = '\0';
  s_home_reply_thinking = false;

  lvgl_lock();
  show_home_page();
  if (s_home_question_label != NULL) {
    lv_label_set_text(s_home_question_label, s_home_question_text);
    lv_obj_set_style_text_color(s_home_question_label, lv_color_hex(0x2563eb), 0);
  }
  if (s_home_reply_label != NULL) {
    lv_label_set_text(s_home_reply_label, s_home_reply_text);
  }
  lvgl_unlock();
}

// 清除“正在说话”状态；如果当前不是该状态则不改 UI。
void lcd_clear_user_speaking(void)
{
  if (!s_lcd_ready || strcmp(s_home_question_text, "正在说话") != 0) {
    // 避免把已经识别出的用户问题误清空。
    return;
  }

  s_home_question_text[0] = '\0';
  s_home_reply_text[0] = '\0';
  s_home_reply_thinking = false;

  lvgl_lock();
  show_home_page();
  if (s_home_question_label != NULL) {
    lv_label_set_text(s_home_question_label, s_home_question_text);
  }
  if (s_home_reply_label != NULL) {
    lv_label_set_text(s_home_reply_label, s_home_reply_text);
  }
  lvgl_unlock();
}

// 显示识别出的用户问题，并把回复区域切换到“思考中”。
void lcd_show_user_question(const char *text)
{
  if (!s_lcd_ready) {
    return;
  }

  strlcpy(s_home_question_text, text != NULL ? text : "", sizeof(s_home_question_text));
  strlcpy(s_home_reply_text, "思考中", sizeof(s_home_reply_text));
  s_home_reply_thinking = true;

  lvgl_lock();
  show_home_page();
  if (s_home_question_label != NULL) {
    lv_label_set_text(s_home_question_label, s_home_question_text);
    lv_obj_set_style_text_color(s_home_question_label, lv_color_hex(0x2563eb), 0);
  }
  if (s_home_reply_label != NULL) {
    lv_label_set_text(s_home_reply_label, s_home_reply_text);
    lv_obj_set_style_text_color(s_home_reply_label, lv_color_hex(0x64748b), 0);
  }
  lvgl_unlock();
}
