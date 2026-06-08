#include "key.h"

#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "key";

#define KEY_DEBOUNCE_MS 20
#define KEY_EVENT_QUEUE_LEN 8

static QueueHandle_t s_gpio_evt_queue;
static QueueHandle_t s_key_evt_queue;
static TaskHandle_t s_key_task_handle;
static volatile bool s_pressed;
static bool s_initialized;

static void IRAM_ATTR key_gpio_isr_handler(void *arg)
{
  uint32_t gpio_num = (uint32_t)arg;
  BaseType_t higher_priority_task_woken = pdFALSE;

  if (s_gpio_evt_queue != NULL) {
    xQueueSendFromISR(s_gpio_evt_queue, &gpio_num, &higher_priority_task_woken);
  }

  if (higher_priority_task_woken == pdTRUE) {
    portYIELD_FROM_ISR();
  }
}

static void key_task(void *arg)
{
  (void)arg;

  bool last_pressed = gpio_get_level(KEY_GPIO_PIN) == 0;
  s_pressed = last_pressed;

  while (true) {
    uint32_t gpio_num = 0;
    if (xQueueReceive(s_gpio_evt_queue, &gpio_num, portMAX_DELAY) != pdTRUE) {
      continue;
    }

    vTaskDelay(pdMS_TO_TICKS(KEY_DEBOUNCE_MS));

    bool current_pressed = gpio_get_level(KEY_GPIO_PIN) == 0;
    if (current_pressed == last_pressed) {
      continue;
    }

    last_pressed = current_pressed;
    s_pressed = current_pressed;

    key_event_t event = current_pressed ? KEY_EVENT_PRESSED : KEY_EVENT_RELEASED;
    if (xQueueSend(s_key_evt_queue, &event, 0) != pdTRUE) {
      ESP_LOGW(TAG, "key event queue full, drop event");
    }
  }
}

esp_err_t key_init(void)
{
  if (s_initialized) {
    return ESP_OK;
  }

  s_gpio_evt_queue = xQueueCreate(KEY_EVENT_QUEUE_LEN, sizeof(uint32_t));
  ESP_RETURN_ON_FALSE(s_gpio_evt_queue != NULL, ESP_ERR_NO_MEM, TAG, "create GPIO event queue failed");

  s_key_evt_queue = xQueueCreate(KEY_EVENT_QUEUE_LEN, sizeof(key_event_t));
  ESP_RETURN_ON_FALSE(s_key_evt_queue != NULL, ESP_ERR_NO_MEM, TAG, "create key event queue failed");

  gpio_config_t io_conf = {
    .pin_bit_mask = (1ull << KEY_GPIO_PIN),
    .mode = GPIO_MODE_INPUT,
    .pull_up_en = GPIO_PULLUP_ENABLE,
    .pull_down_en = GPIO_PULLDOWN_DISABLE,
    .intr_type = GPIO_INTR_ANYEDGE,
  };
  ESP_RETURN_ON_ERROR(gpio_config(&io_conf), TAG, "configure key GPIO failed");

  esp_err_t err = gpio_install_isr_service(0);
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    ESP_RETURN_ON_ERROR(err, TAG, "install GPIO ISR service failed");
  }

  ESP_RETURN_ON_ERROR(gpio_isr_handler_add(KEY_GPIO_PIN, key_gpio_isr_handler, (void *)KEY_GPIO_PIN),
                      TAG,
                      "add key ISR handler failed");

  BaseType_t task_ret = xTaskCreate(key_task, "key_evt", 2048, NULL, 10, &s_key_task_handle);
  ESP_RETURN_ON_FALSE(task_ret == pdPASS, ESP_ERR_NO_MEM, TAG, "create key task failed");

  s_initialized = true;
  ESP_LOGI(TAG, "key ready: GPIO=%d active-low interrupt input", KEY_GPIO_PIN);
  return ESP_OK;
}

bool key_is_pressed(void)
{
  return s_pressed;
}

bool key_wait_event(key_event_t *event, TickType_t ticks_to_wait)
{
  if (event == NULL || s_key_evt_queue == NULL) {
    return false;
  }

  return xQueueReceive(s_key_evt_queue, event, ticks_to_wait) == pdTRUE;
}
