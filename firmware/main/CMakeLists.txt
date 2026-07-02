idf_component_register(
    SRCS "sleep_tracker_main.c"
    INCLUDE_DIRS "."
    REQUIRES board max30102 ppg actigraphy rtc pmu bodynet sleep_core ui sync
)
