#ifndef __LED_H_
#define __LED_H_

#include <stdint.h>

#include "hal/gpio_types.h"

#define LED_GPIO_PIN GPIO_NUM_48

void led_init(void);
void led_set(uint32_t on);
void led_toggle(void);

#define LED(X) led_set(X)
#define LED_TOGGLE() led_toggle()

#endif
