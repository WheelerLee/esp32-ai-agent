#include "lcd.h"

#include <stdbool.h>
#include <stddef.h>

#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_lcd_ili9341.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lcd_text_bitmap.h"
#include "lvgl.h"

static const char *TAG = "lcd";

static esp_lcd_panel_handle_t s_panel_handle;
static lv_disp_draw_buf_t s_disp_buf;
static lv_disp_drv_t s_disp_drv;
static lv_obj_t *s_text_canvas;
static lv_color_t s_text_canvas_buf[LCD_TEXT_BITMAP_WIDTH * LCD_TEXT_BITMAP_HEIGHT];
static SemaphoreHandle_t s_lvgl_mutex;
static TaskHandle_t s_lvgl_task_handle;

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

static esp_err_t lvgl_port_init(void)
{
  lv_init();

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

  const esp_timer_create_args_t tick_timer_args = {
    .callback = lvgl_tick_cb,
    .name = "lvgl_tick",
  };
  esp_timer_handle_t tick_timer = NULL;
  ESP_RETURN_ON_ERROR(esp_timer_create(&tick_timer_args, &tick_timer), TAG, "create LVGL tick failed");
  ESP_RETURN_ON_ERROR(esp_timer_start_periodic(tick_timer, 2 * 1000), TAG, "start LVGL tick failed");

  // The LVGL task runs the timer handler; UI changes from other tasks must lock first.
  BaseType_t ret = xTaskCreate(lvgl_task, "lvgl", 4096, NULL, 5, &s_lvgl_task_handle);
  ESP_RETURN_ON_FALSE(ret == pdPASS, ESP_ERR_NO_MEM, TAG, "create LVGL task failed");

  return ESP_OK;
}

static lv_color_t blend_rgb(uint8_t bg_r,
                            uint8_t bg_g,
                            uint8_t bg_b,
                            uint8_t fg_r,
                            uint8_t fg_g,
                            uint8_t fg_b,
                            uint8_t opa)
{
  uint8_t inv = 255 - opa;
  uint8_t r = (bg_r * inv + fg_r * opa) / 255;
  uint8_t g = (bg_g * inv + fg_g * opa) / 255;
  uint8_t b = (bg_b * inv + fg_b * opa) / 255;

  return lv_color_make(r, g, b);
}

static void draw_chinese_text_bitmap(void)
{
  for (int y = 0; y < LCD_TEXT_BITMAP_HEIGHT; y++) {
    for (int x = 0; x < LCD_TEXT_BITMAP_WIDTH; x++) {
      uint32_t index = y * LCD_TEXT_BITMAP_WIDTH + x;
      uint8_t alpha = lcd_text_bitmap_alpha[index];
      s_text_canvas_buf[index] = alpha ? blend_rgb(0xff, 0xff, 0xff, 0x00, 0x00, 0x00, alpha)
                                       : lv_color_white();
    }
  }
}

esp_err_t lcd_init(void)
{
  ESP_LOGI(TAG, "initialize ILI9341 on SPI2");
  ESP_RETURN_ON_ERROR(lcd_backlight_init(), TAG, "backlight init failed");
  ESP_RETURN_ON_ERROR(lcd_panel_init(), TAG, "panel init failed");
  ESP_RETURN_ON_ERROR(lvgl_port_init(), TAG, "LVGL init failed");

  lvgl_lock();
  lv_obj_set_style_bg_color(lv_scr_act(), lv_color_white(), 0);
  // Draw pre-rendered Chinese text to avoid missing glyphs in LVGL's small CJK demo font.
  draw_chinese_text_bitmap();
  s_text_canvas = lv_canvas_create(lv_scr_act());
  lv_canvas_set_buffer(s_text_canvas,
                       s_text_canvas_buf,
                       LCD_TEXT_BITMAP_WIDTH,
                       LCD_TEXT_BITMAP_HEIGHT,
                       LV_IMG_CF_TRUE_COLOR);
  lv_obj_align(s_text_canvas, LV_ALIGN_CENTER, 0, 0);
  lv_obj_invalidate(lv_scr_act());
  lvgl_unlock();

  return ESP_OK;
}

void lcd_show_text(const char *text)
{
  (void)text;
  lvgl_lock();
  draw_chinese_text_bitmap();
  if (s_text_canvas != NULL) {
    lv_obj_align(s_text_canvas, LV_ALIGN_CENTER, 0, 0);
  }
  lv_obj_invalidate(lv_scr_act());
  lvgl_unlock();
}
