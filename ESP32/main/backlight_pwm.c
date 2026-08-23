#include "backlight_pwm.h"

#include <stdint.h>

#include "driver/ledc.h"
#include "driver/gpio.h"

/*
 * Existing backlight PWM output pin.
 */
#define BACKLIGHT_PWM_GPIO       GPIO_NUM_5

/*
 * 5 kHz is used as a conservative laptop-panel PWM default.
 * Change this if the exact panel datasheet specifies another range.
 */
#define BACKLIGHT_PWM_FREQ_HZ    5000

#define BACKLIGHT_PWM_MODE       LEDC_LOW_SPEED_MODE
#define BACKLIGHT_PWM_TIMER      LEDC_TIMER_0
#define BACKLIGHT_PWM_CHANNEL    LEDC_CHANNEL_0
#define BACKLIGHT_PWM_RESOLUTION LEDC_TIMER_10_BIT

#define BACKLIGHT_PWM_DUTY_MAX   ((1U << 10) - 1U)

static uint8_t s_percent = 0;

static uint32_t percent_to_duty(uint8_t percent)
{
    if (percent >= 100) {
        return BACKLIGHT_PWM_DUTY_MAX;
    }

    return ((uint32_t)percent * BACKLIGHT_PWM_DUTY_MAX + 50U) / 100U;
}

esp_err_t backlight_pwm_init(uint8_t initial_percent)
{
    if (initial_percent > 100) {
        initial_percent = 100;
    }

    ledc_timer_config_t timer = {
        .speed_mode = BACKLIGHT_PWM_MODE,
        .duty_resolution = BACKLIGHT_PWM_RESOLUTION,
        .timer_num = BACKLIGHT_PWM_TIMER,
        .freq_hz = BACKLIGHT_PWM_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
        .deconfigure = false,
    };

    esp_err_t err = ledc_timer_config(&timer);
    if (err != ESP_OK) {
        return err;
    }

    ledc_channel_config_t channel = {
        .gpio_num = BACKLIGHT_PWM_GPIO,
        .speed_mode = BACKLIGHT_PWM_MODE,
        .channel = BACKLIGHT_PWM_CHANNEL,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = BACKLIGHT_PWM_TIMER,
        .duty = percent_to_duty(initial_percent),
        .hpoint = 0,
        .sleep_mode = LEDC_SLEEP_MODE_NO_ALIVE_NO_PD,
        .flags = {
            .output_invert = 0,
        },
    };

    err = ledc_channel_config(&channel);
    if (err != ESP_OK) {
        return err;
    }

    s_percent = initial_percent;
    return ESP_OK;
}

esp_err_t backlight_pwm_set_percent(uint8_t percent)
{
    if (percent > 100) {
        percent = 100;
    }

    uint32_t duty = percent_to_duty(percent);

    esp_err_t err = ledc_set_duty(
        BACKLIGHT_PWM_MODE,
        BACKLIGHT_PWM_CHANNEL,
        duty
    );

    if (err != ESP_OK) {
        return err;
    }

    err = ledc_update_duty(
        BACKLIGHT_PWM_MODE,
        BACKLIGHT_PWM_CHANNEL
    );

    if (err == ESP_OK) {
        s_percent = percent;
    }

    return err;
}

uint8_t backlight_pwm_get_percent(void)
{
    return s_percent;
}
