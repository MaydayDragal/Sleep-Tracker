idf_component_register(
    SRCS "ui.c"
    INCLUDE_DIRS "include"
)
# TODO(phase0/4): add managed deps for the display + LVGL, e.g.
#   idf.py add-dependency "lvgl/lvgl^9"
#   idf.py add-dependency "espressif/esp_lcd_sh8601"
# then add them to REQUIRES above. See firmware/README.md.
