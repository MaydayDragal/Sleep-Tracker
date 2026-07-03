idf_component_register(
    SRCS "power.c"
    INCLUDE_DIRS "include"
    REQUIRES board esp_pm esp_timer
)
