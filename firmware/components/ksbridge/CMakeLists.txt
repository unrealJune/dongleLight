idf_component_register(
    SRCS "ks_ble.c" "ksb_bridge.c" "ksb_ui.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES kscore
    PRIV_REQUIRES bt esp_driver_gpio esp_driver_spi esp_lcd esp_timer st7735
)
