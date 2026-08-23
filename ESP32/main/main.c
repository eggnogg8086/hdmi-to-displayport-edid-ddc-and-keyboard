/*
 * ESP32-C3 EDID + DDC/CI + keyboard brightness + smooth backlight control
 *
 * ESP-IDF 6.0.2
 *
 * DDC:
 *   GPIO0 SDA
 *   GPIO1 SCL
 *   0x50 EDID
 *   0x37 DDC/CI
 *
 * PWM:
 *   GPIO5, 5 kHz
 *
 * Keyboard matrix, passive sensing:
 *   GPIO6  = Fn KSO
 *   GPIO8  = Fn KSI
 *   GPIO10 = F9 KSO
 *   GPIO9  = F10 KSO
 *   GPIO7  = F9/F10 shared KSI
 *
 */

#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "driver/gpio.h"

#include "soft_i2c.h"
#include "edid_eeprom.h"
#include "ddcci.h"
#include "backlight_pwm.h"
#include "keyboard_matrix.h"

#define TAG       "DDC"

#define SDA_GPIO  GPIO_NUM_0
#define SCL_GPIO  GPIO_NUM_1

#define INITIAL_BRIGHTNESS  50
/*
 * DDC/CI keeps the original faster slew.
 * Keyboard changes use a slower, clearly visible fade.
 *
 * At 50 %/s, one normal 10% keyboard step takes about 200 ms.
 * If a brightness key is held, repeated 10% targets merge into one
 * continuous smooth ramp instead of jumping between levels.
 */
#define DDC_BRIGHTNESS_SLEW_PERCENT_PER_SEC       200U
#define KEYBOARD_BRIGHTNESS_SLEW_PERCENT_PER_SEC   50U

#define KEY_BRIGHTNESS_STEP  10
#define KEY_BRIGHTNESS_MAX  100

/* If the powered-off laptop holds both DDC lines low, force PWM off. */
#define DDC_BOTH_LOW_OFF_US  20000LL

static uint8_t s_requested_brightness = INITIAL_BRIGHTNESS;
static uint8_t s_target_brightness = INITIAL_BRIGHTNESS;
static bool s_ddcci_power_on = true;
/* True only when Fn+F9 deliberately selected the 0%% backlight-off step. */
static bool s_keyboard_forced_off = false;
static int64_t s_last_brightness_update_us = 0;
static uint32_t s_brightness_slew_percent_per_sec = DDC_BRIGHTNESS_SLEW_PERCENT_PER_SEC;

static int64_t s_ddc_both_low_since_us = 0;
static bool s_ddc_bus_forced_off = false;

static void set_target_brightness_with_rate(uint8_t target, uint32_t rate_percent_per_sec)
{
    if (target > 100) {
        target = 100;
    }

    if (rate_percent_per_sec == 0) {
        rate_percent_per_sec = 1;
    }

    s_target_brightness = target;
    s_brightness_slew_percent_per_sec = rate_percent_per_sec;

    /*
     * Start timing the new fade from now. This prevents elapsed time from a
     * previous target/rate being consumed as an immediate multi-percent jump.
     */
    s_last_brightness_update_us = esp_timer_get_time();
}

static inline void set_ddc_target_brightness(uint8_t target)
{
    set_target_brightness_with_rate(
        target,
        DDC_BRIGHTNESS_SLEW_PERCENT_PER_SEC
    );
}

static inline void set_keyboard_target_brightness(uint8_t target)
{
    set_target_brightness_with_rate(
        target,
        KEYBOARD_BRIGHTNESS_SLEW_PERCENT_PER_SEC
    );
}

static void brightness_service(void)
{
    const int64_t now_us = esp_timer_get_time();

    if (s_last_brightness_update_us == 0) {
        s_last_brightness_update_us = now_us;
        return;
    }

    int64_t elapsed_us = now_us - s_last_brightness_update_us;

    if (elapsed_us <= 0) {
        return;
    }

    const uint32_t slew_rate = s_brightness_slew_percent_per_sec;

    uint32_t steps = (uint32_t)(
        (elapsed_us * (int64_t)slew_rate) /
        1000000LL
    );

    if (steps == 0) {
        return;
    }

    const int64_t consumed_us =
        ((int64_t)steps * 1000000LL) /
        (int64_t)slew_rate;

    s_last_brightness_update_us += consumed_us;

    uint8_t current = backlight_pwm_get_percent();
    const uint8_t target = s_target_brightness;

    if (current == target) {
        s_last_brightness_update_us = now_us;
        return;
    }

    if (current < target) {
        uint32_t distance = (uint32_t)target - current;
        if (steps > distance) {
            steps = distance;
        }
        current = (uint8_t)(current + steps);
    } else {
        uint32_t distance = (uint32_t)current - target;
        if (steps > distance) {
            steps = distance;
        }
        current = (uint8_t)(current - steps);
    }

    esp_err_t err = backlight_pwm_set_percent(current);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "PWM update failed: %s", esp_err_to_name(err));
    }
}

static void apply_brightness_request(uint8_t brightness)
{
    if (brightness > 100) {
        brightness = 100;
    }

    /* VCP 0x10 = 0 is treated as temporary display-off. */
    if (brightness > 0) {
        s_requested_brightness = brightness;
        s_keyboard_forced_off = false;
    }

    if (!s_ddcci_power_on) {
        set_ddc_target_brightness(0);
        ESP_LOGI(
            TAG,
            "Brightness=%u%% remembered while D6 power is off",
            brightness
        );
        return;
    }

    set_ddc_target_brightness(brightness);

    ESP_LOGI(
        TAG,
        "Brightness target=%u%% current=%u%%",
        brightness,
        backlight_pwm_get_percent()
    );
}

/*
 * Keyboard brightness ladder:
 *
 *   DOWN: 100, 90, ... 20, 10, 1, 0
 *   UP:     0,  1, 10, 20, ... 90, 100
 *
 * Values that came from DDC/CI and are not on the normal ladder are handled
 * sensibly: <=10 moves to 1/10, otherwise the ordinary 10-point step is used.
 *
 * The keyboard NEVER writes PWM directly. It only changes the target, so the
 * same non-blocking fade used by DDC/CI is applied to Fn+F9/F10 as well.
 */
static uint8_t keyboard_next_brightness(uint8_t current, int direction)
{
    if (direction < 0) {
        if (current == 0) {
            return 0;
        }

        if (current <= 1) {
            return 0;
        }

        if (current <= 10) {
            return 1;
        }

        int next = (int)current - KEY_BRIGHTNESS_STEP;
        if (next < 10) {
            next = 10;
        }
        return (uint8_t)next;
    }

    if (current == 0) {
        return 1;
    }

    if (current < 10) {
        return 10;
    }

    int next = (int)current + KEY_BRIGHTNESS_STEP;
    if (next > KEY_BRIGHTNESS_MAX) {
        next = KEY_BRIGHTNESS_MAX;
    }
    return (uint8_t)next;
}

static void apply_keyboard_brightness_step(int direction)
{
    const uint8_t current = ddcci_get_brightness();
    const uint8_t next = keyboard_next_brightness(current, direction);

    if (next == current) {
        return;
    }

    /* Keep Get VCP 0x10 synchronized with the keyboard-selected level. */
    ddcci_set_brightness(next);

    if (next == 0) {
        /*
         * 0%% from the keyboard is an intentional backlight-off state.
         * Use the keyboard fade rate here too.
         */
        s_keyboard_forced_off = true;
        set_keyboard_target_brightness(0);
    } else {
        s_keyboard_forced_off = false;
        s_requested_brightness = next;

        if (s_ddcci_power_on) {
            set_keyboard_target_brightness(next);
        } else {
            /* Keep it dark while D6 is off, but remember the new level. */
            set_ddc_target_brightness(0);
        }
    }

    ESP_LOGI(
        TAG,
        "Keyboard brightness %s: %u%% -> %u%% (fading)",
        direction < 0 ? "DOWN" : "UP",
        (unsigned)current,
        (unsigned)next
    );
}

static void apply_power_mode(uint8_t mode)
{
    if (mode == DDCCI_POWER_ON) {
        if (!s_ddcci_power_on) {
            s_ddcci_power_on = true;

            const uint8_t restore =
                s_keyboard_forced_off ? 0 : s_requested_brightness;

            set_ddc_target_brightness(restore);

            ESP_LOGI(
                TAG,
                "D6 ON -> target=%u%%",
                restore
            );
        }
        return;
    }

    if (mode == DDCCI_POWER_STANDBY ||
        mode == DDCCI_POWER_SUSPEND ||
        mode == DDCCI_POWER_OFF ||
        mode == DDCCI_POWER_OFF_WRITE) {

        s_ddcci_power_on = false;
        set_ddc_target_brightness(0);

        ESP_LOGI(TAG, "D6 0x%02X -> target=0%%", mode);
    }
}

/*
 * Returns true while PWM must remain forced off due to both DDC lines being
 * continuously low. Brief normal I2C low periods are much shorter than 20 ms.
 */
static bool ddc_bus_low_service(void)
{
    const int64_t now_us = esp_timer_get_time();
    const bool both_low =
        gpio_get_level(SDA_GPIO) == 0 &&
        gpio_get_level(SCL_GPIO) == 0;

    if (both_low) {
        if (s_ddc_both_low_since_us == 0) {
            s_ddc_both_low_since_us = now_us;
        }

        if (!s_ddc_bus_forced_off &&
            (now_us - s_ddc_both_low_since_us) >= DDC_BOTH_LOW_OFF_US) {

            s_ddc_bus_forced_off = true;

            esp_err_t err = backlight_pwm_set_percent(0);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "PWM force-off failed: %s", esp_err_to_name(err));
            }

            ESP_LOGI(TAG, "DDC SDA+SCL held low -> PWM forced to 0%%");
        }

        return s_ddc_bus_forced_off;
    }

    s_ddc_both_low_since_us = 0;

    if (s_ddc_bus_forced_off) {
        s_ddc_bus_forced_off = false;
        s_last_brightness_update_us = now_us;

        ESP_LOGI(
            TAG,
            "DDC bus released -> resume target=%u%%",
            s_target_brightness
        );
    }

    return false;
}

void app_main(void)
{
    ESP_LOGI(TAG, "ESP32-C3 EDID + DDC/CI + keyboard + smooth PWM");
    ESP_LOGI(TAG, "DDC SDA=GPIO0 SCL=GPIO1");
    ESP_LOGI(TAG, "PWM GPIO5 5 kHz");
    ESP_LOGI(TAG, "Keyboard Fn=6/8 F9=10/7 F10=9/7");
    ESP_LOGI(
        TAG,
        "Fade rates: DDC=%u%%/s keyboard=%u%%/s",
        (unsigned)DDC_BRIGHTNESS_SLEW_PERCENT_PER_SEC,
        (unsigned)KEYBOARD_BRIGHTNESS_SLEW_PERCENT_PER_SEC
    );

    ESP_ERROR_CHECK(backlight_pwm_init(INITIAL_BRIGHTNESS));

    edid_eeprom_init();
    ddcci_init(INITIAL_BRIGHTNESS);

    ESP_ERROR_CHECK(soft_i2c_register_device(edid_eeprom_device()));
    ESP_ERROR_CHECK(soft_i2c_register_device(ddcci_device()));

    /* Reset the DDC pads before installing the software-I2C engine. */
    gpio_reset_pin(SDA_GPIO);
    gpio_reset_pin(SCL_GPIO);

    ESP_ERROR_CHECK(soft_i2c_start(SDA_GPIO, SCL_GPIO));

    /* soft_i2c_start() installs the shared GPIO ISR service first. */
    ESP_ERROR_CHECK(keyboard_matrix_init());

    s_last_brightness_update_us = esp_timer_get_time();

    ESP_LOGI(TAG, "READY");

    for (;;) {
        uint8_t value;

        if (ddcci_take_brightness_update(&value)) {
            apply_brightness_request(value);
        }

        if (ddcci_take_power_mode_update(&value)) {
            apply_power_mode(value);
        }

        keyboard_matrix_service();

        if (keyboard_matrix_take_brightness_down()) {
            apply_keyboard_brightness_step(-1);
        }

        if (keyboard_matrix_take_brightness_up()) {
            apply_keyboard_brightness_step(+1);
        }

        if (!ddc_bus_low_service()) {
            brightness_service();
        }

        /* One actual RTOS tick so CPU0 idle gets time. */
        vTaskDelay(1);
    }
}
