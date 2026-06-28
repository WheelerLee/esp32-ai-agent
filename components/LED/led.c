#include "led.h"

// 将 LED 引脚配置为 GPIO 输出，并设置初始熄灭状态。
void led_init(void)
{
  gpio_config_t io_conf = {0};
  io_conf.pin_bit_mask = (1ull << LED_GPIO_PIN);
  io_conf.mode = GPIO_MODE_INPUT_OUTPUT;
  io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
  io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
  io_conf.intr_type = GPIO_INTR_DISABLE;

  gpio_config(&io_conf);

  // 默认熄灭，后续只由应用逻辑显式改变 LED 状态。
  LED(0);
}
