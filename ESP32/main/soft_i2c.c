#include "soft_i2c.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

#include "esp_attr.h"
#include "esp_intr_alloc.h"

#include "soc/gpio_struct.h"

#define MAX_I2C_DEVICES  4

/*
 * The DDC source can disappear mid-transaction when the laptop powers off.
 * Exit the tight ISR polling loop quickly if the bus stops moving, especially
 * if both lines are held low. This keeps the CPU from reaching the interrupt
 * watchdog while preserving the otherwise-working transaction engine.
 */
#define I2C_STUCK_BOTH_LOW_SPINS   250000U
#define I2C_NO_EDGE_TIMEOUT_SPINS 1000000U

typedef enum {
    ST_ADDRESS = 0,
    ST_ADDRESS_ACK,
    ST_RX,
    ST_RX_ACK,
    ST_TX,
    ST_TX_MASTER_ACK,
    ST_IGNORE
} bus_state_t;

/*
 * Copy registered device descriptors into internal RAM.
 * The ISR never dereferences descriptors stored in flash.
 */
static DRAM_ATTR soft_i2c_device_t s_devices[MAX_I2C_DEVICES];
static volatile size_t s_device_count = 0;

static volatile gpio_num_t s_sda_gpio;
static volatile gpio_num_t s_scl_gpio;
static volatile uint32_t s_sda_bit;
static volatile uint32_t s_scl_bit;

static volatile soft_i2c_stats_t s_stats;

/* -------------------------------------------------------------
 * Direct GPIO
 * ------------------------------------------------------------- */

static inline uint32_t IRAM_ATTR sda_read(void)
{
    return (GPIO.in.val & s_sda_bit) ? 1U : 0U;
}

static inline uint32_t IRAM_ATTR scl_read(void)
{
    return (GPIO.in.val & s_scl_bit) ? 1U : 0U;
}

/*
 * SDA is INPUT_OUTPUT_OD:
 *   output latch 0 -> actively pull LOW
 *   output latch 1 -> release line
 */
static inline void IRAM_ATTR sda_low(void)
{
    GPIO.out_w1tc.val = s_sda_bit;
}

static inline void IRAM_ATTR sda_release(void)
{
    GPIO.out_w1ts.val = s_sda_bit;
}

static inline void IRAM_ATTR sda_put(uint32_t bit)
{
    if (bit) {
        sda_release();
    } else {
        sda_low();
    }
}

static soft_i2c_device_t *IRAM_ATTR find_device(uint8_t address)
{
    size_t count = s_device_count;

    for (size_t i = 0; i < count; ++i) {
        if (s_devices[i].address == address) {
            return &s_devices[i];
        }
    }

    return NULL;
}

static inline void IRAM_ATTR end_device(soft_i2c_device_t **active)
{
    if (*active != NULL && (*active)->end != NULL) {
        (*active)->end();
    }

    *active = NULL;
}

/* -------------------------------------------------------------
 * Poll entire transaction inside one START ISR.
 * ------------------------------------------------------------- */

static void IRAM_ATTR run_transaction(void)
{
    UBaseType_t old_mask = portSET_INTERRUPT_MASK_FROM_ISR();

    bus_state_t state = ST_ADDRESS;

    uint8_t shift = 0;
    uint8_t bits = 0;

    bool addressed = false;
    bool read_direction = false;
    bool rx_ack = true;

    uint8_t ack_phase = 0;

    uint8_t tx_byte = 0;
    uint8_t tx_bits = 0;
    bool master_ack = false;

    soft_i2c_device_t *active = NULL;

    uint32_t pins = GPIO.in.val;

    /* Confirm initial START: SDA low while SCL high. */
    if ((pins & s_scl_bit) == 0 || (pins & s_sda_bit) != 0) {
        GPIO.status_w1tc.val = s_sda_bit;
        portCLEAR_INTERRUPT_MASK_FROM_ISR(old_mask);
        return;
    }

    s_stats.transactions++;
    s_stats.starts++;

    uint32_t previous = pins;
    uint32_t idle_spins = 0;

    for (;;) {
        uint32_t current = GPIO.in.val;

        if (current == previous) {
            idle_spins++;

            const bool both_low =
                ((current & s_sda_bit) == 0U) &&
                ((current & s_scl_bit) == 0U);

            if ((both_low && idle_spins > I2C_STUCK_BOTH_LOW_SPINS) ||
                idle_spins > I2C_NO_EDGE_TIMEOUT_SPINS) {

                end_device(&active);
                sda_release();
                s_stats.timeouts++;
                break;
            }

            continue;
        }

        idle_spins = 0;

        uint32_t old_sda = (previous & s_sda_bit) ? 1U : 0U;
        uint32_t old_scl = (previous & s_scl_bit) ? 1U : 0U;

        uint32_t new_sda = (current & s_sda_bit) ? 1U : 0U;
        uint32_t new_scl = (current & s_scl_bit) ? 1U : 0U;

        /*
         * START / repeated START / STOP are SDA transitions while SCL high.
         */
        if (new_scl) {
            if (old_sda == 1U && new_sda == 0U) {
                end_device(&active);

                state = ST_ADDRESS;
                shift = 0;
                bits = 0;
                addressed = false;
                read_direction = false;
                rx_ack = true;
                ack_phase = 0;

                sda_release();

                s_stats.repeated_starts++;

                previous = GPIO.in.val;
                continue;
            }

            if (old_sda == 0U && new_sda == 1U) {
                end_device(&active);
                sda_release();
                s_stats.stops++;
                break;
            }
        }

        /* ---------------------------------------------------------
         * Rising SCL: receiver samples the bus.
         * --------------------------------------------------------- */
        if (old_scl == 0U && new_scl == 1U) {
            switch (state) {

                case ST_ADDRESS:
                    shift = (uint8_t)((shift << 1) | new_sda);
                    bits++;

                    if (bits == 8) {
                        uint8_t address = (uint8_t)(shift >> 1);

                        read_direction = (shift & 1U) != 0;
                        active = find_device(address);
                        addressed = (active != NULL);

                        if (addressed) {
                            s_stats.address_matches++;

                            if (active->begin != NULL) {
                                active->begin(read_direction);
                            }
                        }

                        ack_phase = 0;
                        state = ST_ADDRESS_ACK;
                    }
                    break;

                case ST_ADDRESS_ACK:
                    if (ack_phase == 1) {
                        ack_phase = 2;
                    }
                    break;

                case ST_RX:
                    shift = (uint8_t)((shift << 1) | new_sda);
                    bits++;

                    if (bits == 8) {
                        s_stats.rx_bytes++;

                        rx_ack = true;

                        if (active != NULL && active->write_byte != NULL) {
                            rx_ack = active->write_byte(shift);
                        }

                        ack_phase = 0;
                        state = ST_RX_ACK;
                    }
                    break;

                case ST_RX_ACK:
                    if (ack_phase == 1) {
                        ack_phase = 2;
                    }
                    break;

                case ST_TX:
                    tx_bits++;

                    if (tx_bits == 8) {
                        ack_phase = 0;
                        state = ST_TX_MASTER_ACK;
                    }
                    break;

                case ST_TX_MASTER_ACK:
                    if (ack_phase == 1) {
                        master_ack = (new_sda == 0U);
                        ack_phase = 2;

                        if (active != NULL && active->read_advance != NULL) {
                            active->read_advance(master_ack);
                        }

                        s_stats.tx_bytes++;
                    }
                    break;

                case ST_IGNORE:
                default:
                    break;
            }
        }

        /* ---------------------------------------------------------
         * Falling SCL: transmitter may change SDA.
         * --------------------------------------------------------- */
        if (old_scl == 1U && new_scl == 0U) {
            switch (state) {

                case ST_ADDRESS_ACK:
                    if (ack_phase == 0) {
                        if (addressed) {
                            sda_low();
                        } else {
                            sda_release();
                        }

                        ack_phase = 1;

                    } else if (ack_phase == 2) {
                        sda_release();
                        ack_phase = 0;

                        if (!addressed) {
                            state = ST_IGNORE;

                        } else if (read_direction) {
                            tx_byte =
                                (active != NULL && active->read_byte != NULL)
                                    ? active->read_byte()
                                    : 0xFF;

                            tx_bits = 0;
                            sda_put((tx_byte >> 7) & 1U);
                            state = ST_TX;

                        } else {
                            shift = 0;
                            bits = 0;
                            state = ST_RX;
                        }
                    }
                    break;

                case ST_RX_ACK:
                    if (ack_phase == 0) {
                        if (rx_ack) {
                            sda_low();
                        } else {
                            sda_release();
                        }

                        ack_phase = 1;

                    } else if (ack_phase == 2) {
                        sda_release();

                        ack_phase = 0;
                        shift = 0;
                        bits = 0;

                        state = rx_ack ? ST_RX : ST_IGNORE;
                    }
                    break;

                case ST_TX:
                    if (tx_bits < 8) {
                        uint8_t bit_index =
                            (uint8_t)(7U - tx_bits);

                        sda_put(
                            (tx_byte >> bit_index) & 1U
                        );
                    }
                    break;

                case ST_TX_MASTER_ACK:
                    if (ack_phase == 0) {
                        /* Master owns SDA for ACK/NACK. */
                        sda_release();
                        ack_phase = 1;

                    } else if (ack_phase == 2) {
                        ack_phase = 0;

                        if (master_ack) {
                            tx_byte =
                                (active != NULL && active->read_byte != NULL)
                                    ? active->read_byte()
                                    : 0xFF;

                            tx_bits = 0;
                            sda_put((tx_byte >> 7) & 1U);
                            state = ST_TX;

                        } else {
                            sda_release();
                            state = ST_IGNORE;
                        }
                    }
                    break;

                case ST_ADDRESS:
                case ST_RX:
                case ST_IGNORE:
                default:
                    break;
            }
        }

        /*
         * If our own SDA write creates another transition while SCL is low,
         * the next polling iteration simply observes and synchronizes it.
         */
        previous = current;
    }

    /*
     * ACK/data transitions made by us can leave a pending GPIO interrupt.
     * Clear it before leaving the ISR.
     */
    GPIO.status_w1tc.val = s_sda_bit;
    sda_release();

    portCLEAR_INTERRUPT_MASK_FROM_ISR(old_mask);
}

static void IRAM_ATTR start_isr(void *arg)
{
    (void)arg;

    uint32_t pins = GPIO.in.val;

    if ((pins & s_sda_bit) == 0 &&
        (pins & s_scl_bit) != 0) {

        run_transaction();
    }

    GPIO.status_w1tc.val = s_sda_bit;
}

/* -------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------- */

esp_err_t soft_i2c_register_device(const soft_i2c_device_t *device)
{
    if (device == NULL || device->address > 0x7F) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_device_count >= MAX_I2C_DEVICES) {
        return ESP_ERR_NO_MEM;
    }

    for (size_t i = 0; i < s_device_count; ++i) {
        if (s_devices[i].address == device->address) {
            return ESP_ERR_INVALID_STATE;
        }
    }

    s_devices[s_device_count] = *device;
    s_device_count++;

    return ESP_OK;
}

esp_err_t soft_i2c_start(gpio_num_t sda_gpio, gpio_num_t scl_gpio)
{
    if (sda_gpio < 0 || scl_gpio < 0 ||
        sda_gpio >= 32 || scl_gpio >= 32 ||
        sda_gpio == scl_gpio) {

        return ESP_ERR_INVALID_ARG;
    }

    if (s_device_count == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    s_sda_gpio = sda_gpio;
    s_scl_gpio = scl_gpio;

    s_sda_bit = (1U << (uint32_t)sda_gpio);
    s_scl_bit = (1U << (uint32_t)scl_gpio);

    gpio_config_t sda_cfg = {
        .pin_bit_mask = (1ULL << (uint32_t)sda_gpio),
        .mode = GPIO_MODE_INPUT_OUTPUT_OD,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };

    esp_err_t err = gpio_config(&sda_cfg);
    if (err != ESP_OK) {
        return err;
    }

    gpio_config_t scl_cfg = {
        .pin_bit_mask = (1ULL << (uint32_t)scl_gpio),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    err = gpio_config(&scl_cfg);
    if (err != ESP_OK) {
        return err;
    }

    err = gpio_set_level(sda_gpio, 1);
    if (err != ESP_OK) {
        return err;
    }

    err = gpio_install_isr_service(
        ESP_INTR_FLAG_IRAM | ESP_INTR_FLAG_LEVEL3
    );

    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    err = gpio_isr_handler_add(
        sda_gpio,
        start_isr,
        NULL
    );

    if (err != ESP_OK) {
        return err;
    }

    return gpio_intr_enable(sda_gpio);
}

void soft_i2c_get_stats(soft_i2c_stats_t *out)
{
    if (out == NULL) {
        return;
    }

    out->transactions    = s_stats.transactions;
    out->starts          = s_stats.starts;
    out->repeated_starts = s_stats.repeated_starts;
    out->stops           = s_stats.stops;
    out->address_matches = s_stats.address_matches;
    out->rx_bytes        = s_stats.rx_bytes;
    out->tx_bytes        = s_stats.tx_bytes;
    out->timeouts        = s_stats.timeouts;
}
