idf_component_register(
    SRCS "sleep_tracker_main.c"
    INCLUDE_DIRS "."
    REQUIRES bsp max30102 ppg actigraphy sleep_core ui sync
)
