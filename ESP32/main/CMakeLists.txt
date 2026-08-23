idf_component_register(
    SRCS
        "main.c"
        "soft_i2c.c"
        "edid_eeprom.c"
        "ddcci.c"
        "backlight_pwm.c"
        "keyboard_matrix.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_driver_gpio
        esp_driver_ledc
        esp_timer
)
