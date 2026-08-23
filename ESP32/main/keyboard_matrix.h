#pragma once

#include <stdbool.h>
#include "esp_err.h"

/*
 * Passive laptop-keyboard matrix monitor.
 *
 * IMPORTANT:
 *   soft_i2c_start() must be called before keyboard_matrix_init(), because
 *   soft_i2c_start() installs the shared ESP-IDF GPIO ISR service.
 */
esp_err_t keyboard_matrix_init(void);

/* Call regularly from normal task context. Non-blocking. */
void keyboard_matrix_service(void);

/* One-shot events. Holding a combo repeats after the configured delay/rate. */
bool keyboard_matrix_take_brightness_down(void);
bool keyboard_matrix_take_brightness_up(void);
