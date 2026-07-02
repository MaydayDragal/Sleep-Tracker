# Top-level ESP-IDF project — Sleep Tracker (Waveshare ESP32-S3-Touch-AMOLED-1.8)
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# The managed Waveshare BSP logs a uint32_t with %02X
# (managed_components/waveshare__esp32_s3_touch_amoled_1_8/...), which GCC 14
# flags under -Werror=format. We can't edit managed_components/, so downgrade
# just that error across the build. Must be set before project().
idf_build_set_property(COMPILE_OPTIONS "-Wno-error=format" APPEND)

project(sleep_tracker)
