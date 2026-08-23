#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "soft_i2c.h"

#define DDCCI_I2C_ADDRESS       0x37

#define DDCCI_VCP_BRIGHTNESS    0x10
#define DDCCI_VCP_POWER_MODE    0xD6
#define DDCCI_BRIGHTNESS_MAX    100

/* MCCS VCP D6 power-mode values. */
#define DDCCI_POWER_ON          0x01
#define DDCCI_POWER_STANDBY     0x02
#define DDCCI_POWER_SUSPEND     0x03
#define DDCCI_POWER_OFF         0x04
#define DDCCI_POWER_OFF_WRITE   0x05

void ddcci_init(uint8_t initial_brightness);
const soft_i2c_device_t *ddcci_device(void);

uint8_t ddcci_get_brightness(void);
void ddcci_set_brightness(uint8_t brightness);
bool ddcci_take_brightness_update(uint8_t *brightness);

uint8_t ddcci_get_power_mode(void);
bool ddcci_take_power_mode_update(uint8_t *mode);
