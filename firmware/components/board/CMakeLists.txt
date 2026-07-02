idf_component_register(
    SRCS "board.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_i2c esp_lvgl_port esp_io_expander esp_lcd_touch waveshare__esp32_s3_touch_amoled_1_8
)
# Delegates the heavy board bring-up (CO5300 QSPI display, FT/CST touch, LVGL,
# SDMMC, TCA9554 IO-expander) to Waveshare's managed BSP. The managed dependency
# is declared in idf_component.yml; the component manager fetches it on build.
