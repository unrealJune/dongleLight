# ST7735 esp_lcd panel driver, vendored from Xinyuan-LilyGO/T-Dongle-S3
# (examples/lcd/bsp_lcd, MIT licensed; see LICENSE).
idf_component_register(
    SRCS "esp_lcd_st7735.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_lcd esp_driver_gpio
)
