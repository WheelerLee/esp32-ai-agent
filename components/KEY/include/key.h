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

esp_err_t key_init(void);
bool key_is_pressed(void);
bool key_wait_event(key_event_t *event, TickType_t ticks_to_wait);

#endif
