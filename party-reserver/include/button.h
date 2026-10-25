#ifndef _BUTTON_H_
#define _BUTTON_H_

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/gpio.h>
#include <inttypes.h>
#include <string.h>

#include <esp_now.h>
#include <esp_wifi.h>

enum button_state {
    RELEASED = 0,
    PRESSED = 1
};

typedef void (*button_callback_t)(enum button_state _state);

int register_button(struct gpio_dt_spec* button_dt_spec, button_callback_t on_state_change_cb);

#endif