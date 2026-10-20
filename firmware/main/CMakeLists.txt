idf_component_register(
    SRCS "main.c" "ks_ble.c" "status_led.c"
    INCLUDE_DIRS "."
    REQUIRES ksaudio bt esp_driver_gpio
)
