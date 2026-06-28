#ifndef KEY_H
#define KEY_H

#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#define KEY_GPIO_PIN GPIO_NUM_2

typedef enum {
  KEY_EVENT_PRESSED = 1,
  KEY_EVENT_RELEASED,
} key_event_t;

// 配置低电平有效按键 GPIO、ISR、消抖任务和事件队列。
esp_err_t key_init(void);

// 返回最近一次消抖后的按键状态。
bool key_is_pressed(void);

// 等待下一个消抖后的按键事件。
bool key_wait_event(key_event_t *event, TickType_t ticks_to_wait);

#endif
