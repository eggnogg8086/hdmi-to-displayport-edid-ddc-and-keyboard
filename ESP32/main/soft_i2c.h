#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_err.h"

typedef struct {
    uint8_t address;

    void (*begin)(bool read_direction);
    bool (*write_byte)(uint8_t byte);
    uint8_t (*read_byte)(void);
    void (*read_advance)(bool master_ack);
    void (*end)(void);
} soft_i2c_device_t;

typedef struct {
    uint32_t transactions;
    uint32_t starts;
    uint32_t repeated_starts;
    uint32_t stops;
    uint32_t address_matches;
    uint32_t rx_bytes;
    uint32_t tx_bytes;
    uint32_t timeouts;
} soft_i2c_stats_t;

esp_err_t soft_i2c_register_device(const soft_i2c_device_t *device);
esp_err_t soft_i2c_start(gpio_num_t sda_gpio, gpio_num_t scl_gpio);
void soft_i2c_get_stats(soft_i2c_stats_t *out);
