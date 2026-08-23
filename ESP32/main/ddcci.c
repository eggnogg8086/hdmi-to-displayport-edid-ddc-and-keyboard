#include "ddcci.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_attr.h"

/*
 * DDC/CI packet constants.
 *
 * I2C device:
 *   7-bit 0x37
 *   write address byte 0x6E
 *   read  address byte 0x6F
 *
 * Host source:             0x51
 * Display response source: 0x6E
 * Request checksum seed:   0x6E
 * Reply checksum seed:     0x50
 */
#define DDCCI_HOST_SOURCE            0x51
#define DDCCI_DISPLAY_SOURCE         0x6E
#define DDCCI_REQUEST_CHECKSUM_SEED  0x6E
#define DDCCI_REPLY_CHECKSUM_SEED    0x50

#define DDCCI_CMD_GET_VCP            0x01
#define DDCCI_CMD_GET_VCP_REPLY      0x02
#define DDCCI_CMD_SET_VCP            0x03

#define DDCCI_RX_MAX                 32
#define DDCCI_TX_MAX                 16

static DRAM_ATTR uint8_t s_rx[DDCCI_RX_MAX];
static volatile size_t s_rx_len = 0;

static DRAM_ATTR uint8_t s_tx[DDCCI_TX_MAX];
static volatile size_t s_tx_len = 0;
static volatile size_t s_tx_index = 0;
static volatile bool s_response_ready = false;
static volatile bool s_read_direction = false;

static volatile uint8_t s_brightness = 50;
static volatile bool s_brightness_update_pending = false;

static volatile uint8_t s_power_mode = DDCCI_POWER_ON;
static volatile bool s_power_mode_update_pending = false;

static uint8_t IRAM_ATTR xor_checksum(
    uint8_t seed,
    const uint8_t *data,
    size_t len)
{
    uint8_t value = seed;

    for (size_t i = 0; i < len; ++i) {
        value ^= data[i];
    }

    return value;
}

static bool IRAM_ATTR request_packet_valid(void)
{
    if (s_rx_len < 4) {
        return false;
    }

    if (s_rx[0] != DDCCI_HOST_SOURCE) {
        return false;
    }

    if ((s_rx[1] & 0x80U) == 0) {
        return false;
    }

    size_t payload_len = (size_t)(s_rx[1] & 0x7FU);

    if (s_rx_len != payload_len + 3U) {
        return false;
    }

    uint8_t expected = xor_checksum(
        DDCCI_REQUEST_CHECKSUM_SEED,
        s_rx,
        s_rx_len - 1U
    );

    return expected == s_rx[s_rx_len - 1U];
}

static void IRAM_ATTR build_null_response(void)
{
    s_tx[0] = DDCCI_DISPLAY_SOURCE;
    s_tx[1] = 0x80;
    s_tx[2] = xor_checksum(
        DDCCI_REPLY_CHECKSUM_SEED,
        s_tx,
        2
    );

    s_tx_len = 3;
    s_tx_index = 0;
    s_response_ready = true;
}

static void IRAM_ATTR build_vcp_response(
    uint8_t vcp,
    uint16_t max_value,
    uint16_t cur_value)
{
    /*
     * Get VCP Feature Reply:
     *   6E 88 02 00 <vcp> 00 <max_hi> <max_lo> <cur_hi> <cur_lo> checksum
     */
    s_tx[0]  = DDCCI_DISPLAY_SOURCE;
    s_tx[1]  = 0x88;
    s_tx[2]  = DDCCI_CMD_GET_VCP_REPLY;
    s_tx[3]  = 0x00;
    s_tx[4]  = vcp;
    s_tx[5]  = 0x00;
    s_tx[6]  = (uint8_t)(max_value >> 8);
    s_tx[7]  = (uint8_t)(max_value & 0xFFU);
    s_tx[8]  = (uint8_t)(cur_value >> 8);
    s_tx[9]  = (uint8_t)(cur_value & 0xFFU);
    s_tx[10] = xor_checksum(
        DDCCI_REPLY_CHECKSUM_SEED,
        s_tx,
        10
    );

    s_tx_len = 11;
    s_tx_index = 0;
    s_response_ready = true;
}

static void IRAM_ATTR process_write_packet(void)
{
    if (!request_packet_valid()) {
        build_null_response();
        return;
    }

    const uint8_t command = s_rx[2];

    if (command == DDCCI_CMD_GET_VCP) {
        if (s_rx_len != 5) {
            build_null_response();
            return;
        }

        const uint8_t vcp = s_rx[3];

        if (vcp == DDCCI_VCP_BRIGHTNESS) {
            build_vcp_response(
                DDCCI_VCP_BRIGHTNESS,
                DDCCI_BRIGHTNESS_MAX,
                s_brightness
            );
            return;
        }

        if (vcp == DDCCI_VCP_POWER_MODE) {
            build_vcp_response(
                DDCCI_VCP_POWER_MODE,
                DDCCI_POWER_OFF_WRITE,
                s_power_mode
            );
            return;
        }

        build_null_response();
        return;
    }

    if (command == DDCCI_CMD_SET_VCP) {
        if (s_rx_len != 7) {
            build_null_response();
            return;
        }

        const uint8_t vcp = s_rx[3];
        uint16_t requested =
            ((uint16_t)s_rx[4] << 8) |
            (uint16_t)s_rx[5];

        if (vcp == DDCCI_VCP_BRIGHTNESS) {
            if (requested > DDCCI_BRIGHTNESS_MAX) {
                requested = DDCCI_BRIGHTNESS_MAX;
            }

            s_brightness = (uint8_t)requested;
            s_brightness_update_pending = true;
            build_null_response();
            return;
        }

        if (vcp == DDCCI_VCP_POWER_MODE) {
            const uint8_t mode = (uint8_t)(requested & 0xFFU);

            if (mode >= DDCCI_POWER_ON &&
                mode <= DDCCI_POWER_OFF_WRITE) {

                s_power_mode = mode;
                s_power_mode_update_pending = true;
            }

            build_null_response();
            return;
        }

        build_null_response();
        return;
    }

    build_null_response();
}

static void IRAM_ATTR ddcci_begin(bool read_direction)
{
    s_read_direction = read_direction;

    if (read_direction) {
        if (!s_response_ready) {
            build_null_response();
        }

        s_tx_index = 0;
    } else {
        s_rx_len = 0;
    }
}

static bool IRAM_ATTR ddcci_write_byte(uint8_t byte)
{
    if (s_rx_len >= DDCCI_RX_MAX) {
        return false;
    }

    s_rx[s_rx_len++] = byte;
    return true;
}

static uint8_t IRAM_ATTR ddcci_read_byte(void)
{
    if (s_tx_index < s_tx_len) {
        return s_tx[s_tx_index];
    }

    return 0x00;
}

static void IRAM_ATTR ddcci_read_advance(bool master_ack)
{
    (void)master_ack;

    if (s_tx_index < s_tx_len) {
        s_tx_index++;
    }
}

static void IRAM_ATTR ddcci_end(void)
{
    if (s_read_direction) {
        s_response_ready = false;
        s_tx_index = 0;
    } else if (s_rx_len > 0) {
        process_write_packet();
    }
}

static DRAM_ATTR const soft_i2c_device_t s_device = {
    .address = DDCCI_I2C_ADDRESS,
    .begin = ddcci_begin,
    .write_byte = ddcci_write_byte,
    .read_byte = ddcci_read_byte,
    .read_advance = ddcci_read_advance,
    .end = ddcci_end,
};

void ddcci_init(uint8_t initial_brightness)
{
    if (initial_brightness > DDCCI_BRIGHTNESS_MAX) {
        initial_brightness = DDCCI_BRIGHTNESS_MAX;
    }

    s_brightness = initial_brightness;
    s_brightness_update_pending = false;

    s_power_mode = DDCCI_POWER_ON;
    s_power_mode_update_pending = false;

    s_rx_len = 0;
    s_tx_len = 0;
    s_tx_index = 0;
    s_response_ready = false;
    s_read_direction = false;
}

const soft_i2c_device_t *ddcci_device(void)
{
    return &s_device;
}

uint8_t ddcci_get_brightness(void)
{
    return s_brightness;
}

void ddcci_set_brightness(uint8_t brightness)
{
    if (brightness > DDCCI_BRIGHTNESS_MAX) {
        brightness = DDCCI_BRIGHTNESS_MAX;
    }

    s_brightness = brightness;
}

bool ddcci_take_brightness_update(uint8_t *brightness)
{
    if (!s_brightness_update_pending) {
        return false;
    }

    const uint8_t value = s_brightness;
    s_brightness_update_pending = false;

    if (brightness != NULL) {
        *brightness = value;
    }

    return true;
}

uint8_t ddcci_get_power_mode(void)
{
    return s_power_mode;
}

bool ddcci_take_power_mode_update(uint8_t *mode)
{
    if (!s_power_mode_update_pending) {
        return false;
    }

    const uint8_t value = s_power_mode;
    s_power_mode_update_pending = false;

    if (mode != NULL) {
        *mode = value;
    }

    return true;
}
