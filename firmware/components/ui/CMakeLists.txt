idf_component_register(
    SRCS "ui.c"
    INCLUDE_DIRS "include"
    REQUIRES board
)
# LVGL + the display/touch stack come transitively from `board`, which delegates
# to the managed Waveshare BSP (waveshare/esp32_s3_touch_amoled_1_8).
