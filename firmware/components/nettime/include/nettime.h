#pragma once
#include <time.h>

// Opportunistic NTP time fetch. Brings WiFi up JUST long enough to fetch the time
// from an NTP server, then shuts WiFi back down — the radio is not kept running.
//
// IMPORTANT: this must be called at the very start of boot, BEFORE any I2C bus /
// device is initialized (before board_init()). WiFi's interrupt latency can glitch
// an in-flight I2C transaction and wedge the shared bus (which hangs the display
// flush and kills touch), so WiFi and I2C must never overlap. This returns the
// time as a value; the caller writes it to the RTC once I2C is up and WiFi is off.
//
// Blocking, with internal timeouts. Credentials come from a gitignored
// components/nettime/wifi_config.h (copy wifi_config.example.h). If that file is
// absent or the SSID is empty, this is a no-op. The ESP32-S3 is 2.4 GHz only.
//
// Returns the local wall-clock time as a time_t (UTC + SLEEPTRK_UTC_OFFSET_MIN, so
// gmtime_r() on it yields the local Y/M/D h:m:s to store in the RTC), or 0 if the
// time could not be fetched (no creds / WiFi or NTP unreachable).
time_t nettime_fetch(void);
