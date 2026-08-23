#pragma once

#include <stdint.h>

#include "esp_err.h"

/*
 * Laptop/eDP panel backlight PWM.
 *
 * DDC/CI brightness is 0..100.
 */
esp_err_t backlight_pwm_init(uint8_t initial_percent);
esp_err_t backlight_pwm_set_percent(uint8_t percent);
uint8_t backlight_pwm_get_percent(void);
