#include "key.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

void key_init(void)
{
  gpio_config_t io_conf = {0};
  io_conf.pin_bit_mask = (1ull << KEY_GPIO_PIN);
  io_conf.mode = GPIO_MODE_INPUT;
  io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
  io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
  io_conf.intr_type = GPIO_INTR_DISABLE;

  gpio_config(&io_conf);
}

uint8_t key_pressed(void)
{
  static uint8_t last_state = 1;
  uint8_t current = gpio_get_level(KEY_GPIO_PIN);

  uint8_t key_event = 0;

  // 下降沿检测（1 -> 0）
  if (last_state == 1 && current == 0)
  {

    vTaskDelay(pdMS_TO_TICKS(10)); // 消抖

    if (gpio_get_level(KEY_GPIO_PIN) == 0)
    {
      key_event = 1;
    }
  }

  last_state = current;

  return key_event;
}
