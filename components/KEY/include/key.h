#ifndef KEY_H
#define KEY_H

#include "driver/gpio.h"

#define KEY_GPIO_PIN GPIO_NUM_0

void key_init(void);

uint8_t key_pressed(void);

#endif