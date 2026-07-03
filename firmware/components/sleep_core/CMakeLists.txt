idf_component_register(
    SRCS "sleep_core.c"
    INCLUDE_DIRS "include"
    REQUIRES ppg actigraphy rtc pmu esp_timer
)
