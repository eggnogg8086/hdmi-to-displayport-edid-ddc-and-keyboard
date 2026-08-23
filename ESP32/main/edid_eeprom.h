#pragma once

#include <stdint.h>

#include "soft_i2c.h"

#define EDID_EEPROM_I2C_ADDRESS 0x50

void edid_eeprom_init(void);
const soft_i2c_device_t *edid_eeprom_device(void);
uint8_t edid_eeprom_get_pointer(void);
