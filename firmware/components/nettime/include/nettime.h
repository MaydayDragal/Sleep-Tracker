#pragma once
#include "esp_err.h"

// Opportunistic NTP time set. Brings WiFi up JUST long enough to fetch the time
// from an NTP server, writes it to the PCF85063 RTC (as a local wall clock =
// UTC + SLEEPTRK_UTC_OFFSET_MIN, matching the project's naive-clock convention),
// then shuts WiFi back down — the radio is not kept running. Blocking, with
// internal timeouts. Call once at boot on the sensor task, AFTER pcf85063_init()
// and BEFORE the RTC seed fallback so a good NTP time wins.
//
// Credentials come from a gitignored components/nettime/wifi_config.h (copy
// wifi_config.example.h). If that file is absent or the SSID is empty, this is a
// no-op. Note: the ESP32-S3 is 2.4 GHz only.
//
// Returns ESP_OK if the RTC was set from NTP; otherwise an error and the RTC is
// left untouched (the caller's existing seed fallback then applies).
esp_err_t nettime_sync(void);
