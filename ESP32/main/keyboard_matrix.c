#include "keyboard_matrix.h"

#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_timer.h"

#include "soc/gpio_reg.h"
#include "soc/soc.h"

/*
 * ESP32-C3 keyboard wiring used by the integrated build.
 *
 * DDC is on GPIO0/GPIO1, so the original measured keyboard mapping can
 * be used unchanged.
 *
 *   GPIO6  = Fn KSO
 *   GPIO8  = Fn KSI
 *
 *   GPIO10 = F9 KSO
 *   GPIO9  = F10 KSO
 *   GPIO7  = shared F9/F10 KSI
 *
 * Motherboard already pulls the KSI lines up to 3.3 V.
 * Every ESP32 pin below remains a passive input with ESP pull resistors off.
 */
#define FN_KSO      GPIO_NUM_6
#define FN_KSI      GPIO_NUM_8
#define F9_KSO      GPIO_NUM_10
#define F10_KSO     GPIO_NUM_20
#define FKEY_KSI    GPIO_NUM_7

/* Electrical edge-pair qualification. */
#define PAIR_FORWARD_US        80U
#define PAIR_REVERSE_US        20U
#define PAIR_DEDUP_US         500U

/* Measured keyboard scan period is about 5 ms / ~190-200 Hz. */
#define SCAN_MIN_US          3500U
#define SCAN_MAX_US          7500U
#define REQUIRED_HITS           3U
#define RELEASE_TIMEOUT_US  30000U

/* Natural laptop-key repeat behaviour for brightness combos. */
#define REPEAT_INITIAL_US   400000U
#define REPEAT_INTERVAL_US  150000U

typedef struct {
    volatile uint32_t last_scan_us;
    volatile uint32_t last_pair_us;
    volatile uint8_t consecutive;
    volatile bool pressed;
} key_detector_t;

static key_detector_t s_fn  = {0};
static key_detector_t s_f9  = {0};
static key_detector_t s_f10 = {0};

static volatile uint32_t s_fn_kso_fall = 0;
static volatile uint32_t s_fn_ksi_fall = 0;
static volatile uint32_t s_f9_kso_fall = 0;
static volatile uint32_t s_f10_kso_fall = 0;
static volatile uint32_t s_fkey_ksi_fall = 0;

static bool s_down_active = false;
static bool s_up_active = false;
static bool s_down_pending = false;
static bool s_up_pending = false;
static uint32_t s_down_repeat_deadline = 0;
static uint32_t s_up_repeat_deadline = 0;

static inline uint32_t IRAM_ATTR gpio_snapshot(void)
{
    return REG_READ(GPIO_IN_REG);
}

static inline bool IRAM_ATTR gpio_bit(uint32_t snapshot, gpio_num_t gpio)
{
    return ((snapshot >> gpio) & 1U) != 0;
}

static inline bool IRAM_ATTR valid_edge_pair(uint32_t kso_time, uint32_t ksi_time)
{
    if (kso_time == 0 || ksi_time == 0) {
        return false;
    }

    int32_t delta = (int32_t)(ksi_time - kso_time);

    if (delta >= 0) {
        return (uint32_t)delta <= PAIR_FORWARD_US;
    }

    return (uint32_t)(-delta) <= PAIR_REVERSE_US;
}

static inline void IRAM_ATTR record_pair(key_detector_t *key, uint32_t now)
{
    if (key->last_pair_us != 0) {
        uint32_t since_pair = (uint32_t)(now - key->last_pair_us);
        if (since_pair < PAIR_DEDUP_US) {
            return;
        }
    }

    key->last_pair_us = now;

    if (key->last_scan_us != 0) {
        uint32_t period = (uint32_t)(now - key->last_scan_us);

        if (period >= SCAN_MIN_US && period <= SCAN_MAX_US) {
            if (key->consecutive < 255U) {
                key->consecutive++;
            }
        } else {
            key->consecutive = 1U;
        }
    } else {
        key->consecutive = 1U;
    }

    key->last_scan_us = now;

    if (key->consecutive >= REQUIRED_HITS) {
        key->pressed = true;
    }
}

static inline void IRAM_ATTR try_fn_pair(uint32_t now)
{
    uint32_t g = gpio_snapshot();

    bool fn_kso  = gpio_bit(g, FN_KSO);
    bool fn_ksi  = gpio_bit(g, FN_KSI);
    bool f9_kso  = gpio_bit(g, F9_KSO);
    bool f10_kso = gpio_bit(g, F10_KSO);

    /*
     * Exact Fn slot:
     *   Fn KSO low, Fn KSI low, both observed F-key KSO lines high.
     * The high checks reject the global/all-low scan states seen earlier.
     */
    if (fn_kso || fn_ksi || !f9_kso || !f10_kso) {
        return;
    }

    if (!valid_edge_pair(s_fn_kso_fall, s_fn_ksi_fall)) {
        return;
    }

    record_pair(&s_fn, now);
}

static inline void IRAM_ATTR try_f9_pair(uint32_t now)
{
    uint32_t g = gpio_snapshot();

    bool ksi     = gpio_bit(g, FKEY_KSI);
    bool f9_kso  = gpio_bit(g, F9_KSO);
    bool f10_kso = gpio_bit(g, F10_KSO);

    /* F9: shared KSI low while F9 KSO low and F10 KSO high. */
    if (ksi || f9_kso || !f10_kso) {
        return;
    }

    if (!valid_edge_pair(s_f9_kso_fall, s_fkey_ksi_fall)) {
        return;
    }

    record_pair(&s_f9, now);
}

static inline void IRAM_ATTR try_f10_pair(uint32_t now)
{
    uint32_t g = gpio_snapshot();

    bool ksi     = gpio_bit(g, FKEY_KSI);
    bool f9_kso  = gpio_bit(g, F9_KSO);
    bool f10_kso = gpio_bit(g, F10_KSO);

    /* F10: shared KSI low while F10 KSO low and F9 KSO high. */
    if (ksi || !f9_kso || f10_kso) {
        return;
    }

    if (!valid_edge_pair(s_f10_kso_fall, s_fkey_ksi_fall)) {
        return;
    }

    record_pair(&s_f10, now);
}

static void IRAM_ATTR fn_kso_isr(void *arg)
{
    (void)arg;
    uint32_t now = (uint32_t)esp_timer_get_time();
    s_fn_kso_fall = now;
    try_fn_pair(now);
}

static void IRAM_ATTR fn_ksi_isr(void *arg)
{
    (void)arg;
    uint32_t now = (uint32_t)esp_timer_get_time();
    s_fn_ksi_fall = now;
    try_fn_pair(now);
}

static void IRAM_ATTR f9_kso_isr(void *arg)
{
    (void)arg;
    uint32_t now = (uint32_t)esp_timer_get_time();
    s_f9_kso_fall = now;
    try_f9_pair(now);
}

static void IRAM_ATTR f10_kso_isr(void *arg)
{
    (void)arg;
    uint32_t now = (uint32_t)esp_timer_get_time();
    s_f10_kso_fall = now;
    try_f10_pair(now);
}

static void IRAM_ATTR fkey_ksi_isr(void *arg)
{
    (void)arg;
    uint32_t now = (uint32_t)esp_timer_get_time();
    s_fkey_ksi_fall = now;

    try_f9_pair(now);
    try_f10_pair(now);
}

static inline void update_release(key_detector_t *key, uint32_t now)
{
    if (!key->pressed || key->last_scan_us == 0) {
        return;
    }

    if ((uint32_t)(now - key->last_scan_us) > RELEASE_TIMEOUT_US) {
        key->pressed = false;
    }
}

static inline bool time_reached(uint32_t now, uint32_t deadline)
{
    return (int32_t)(now - deadline) >= 0;
}

static void service_combo(bool active,
                          bool *was_active,
                          bool *pending,
                          uint32_t *repeat_deadline,
                          uint32_t now)
{
    if (!active) {
        *was_active = false;
        *repeat_deadline = 0;
        return;
    }

    if (!*was_active) {
        *was_active = true;
        *pending = true;
        *repeat_deadline = now + REPEAT_INITIAL_US;
        return;
    }

    if (*repeat_deadline != 0 && time_reached(now, *repeat_deadline)) {
        *pending = true;
        *repeat_deadline = now + REPEAT_INTERVAL_US;
    }
}

esp_err_t keyboard_matrix_init(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask =
            (1ULL << FN_KSO) |
            (1ULL << FN_KSI) |
            (1ULL << F9_KSO) |
            (1ULL << F10_KSO) |
            (1ULL << FKEY_KSI),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        return err;
    }

    /*
     * Do NOT call gpio_install_isr_service() here.
     * soft_i2c_start() already installed the shared IRAM/LEVEL3 service.
     */
    const gpio_num_t pins[] = {
        FN_KSO, FN_KSI, F9_KSO, F10_KSO, FKEY_KSI
    };

    for (unsigned i = 0; i < sizeof(pins) / sizeof(pins[0]); ++i) {
        err = gpio_set_intr_type(pins[i], GPIO_INTR_NEGEDGE);
        if (err != ESP_OK) {
            return err;
        }
    }

    err = gpio_isr_handler_add(FN_KSO, fn_kso_isr, NULL);
    if (err != ESP_OK) return err;

    err = gpio_isr_handler_add(FN_KSI, fn_ksi_isr, NULL);
    if (err != ESP_OK) return err;

    err = gpio_isr_handler_add(F9_KSO, f9_kso_isr, NULL);
    if (err != ESP_OK) return err;

    err = gpio_isr_handler_add(F10_KSO, f10_kso_isr, NULL);
    if (err != ESP_OK) return err;

    err = gpio_isr_handler_add(FKEY_KSI, fkey_ksi_isr, NULL);
    if (err != ESP_OK) return err;

    return ESP_OK;
}

void keyboard_matrix_service(void)
{
    uint32_t now = (uint32_t)esp_timer_get_time();

    update_release(&s_fn, now);
    update_release(&s_f9, now);
    update_release(&s_f10, now);

    bool fn  = s_fn.pressed;
    bool f9  = s_f9.pressed;
    bool f10 = s_f10.pressed;

    /* If both brightness keys are held, do nothing in either direction. */
    bool down = fn && f9 && !f10;
    bool up   = fn && f10 && !f9;

    service_combo(
        down,
        &s_down_active,
        &s_down_pending,
        &s_down_repeat_deadline,
        now
    );

    service_combo(
        up,
        &s_up_active,
        &s_up_pending,
        &s_up_repeat_deadline,
        now
    );
}

bool keyboard_matrix_take_brightness_down(void)
{
    if (!s_down_pending) {
        return false;
    }

    s_down_pending = false;
    return true;
}

bool keyboard_matrix_take_brightness_up(void)
{
    if (!s_up_pending) {
        return false;
    }

    s_up_pending = false;
    return true;
}
