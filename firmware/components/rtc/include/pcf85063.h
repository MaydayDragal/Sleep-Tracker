#pragma once
#include "esp_err.h"
#include "driver/i2c_master.h"
#include <stdint.h>
#include <stdbool.h>

// PCF85063A real-time clock (onboard, I2C @ 0x51). Provides wall-clock time for
// epoch timestamps (PLAN.md §3.1). Minimal register-level driver — the managed
// waveshare/pcf85063a component needs IDF >=5.5 and we're pinned to 5.4.
//
// NOTE: the `pcf85063_` prefix (not `rtc_`) is deliberate — ESP-IDF's
// esp_hw_support already defines a `rtc.h` header AND an `rtc_init()` symbol,
// both of which would collide.

typedef struct {
    uint16_t year;   // full year, e.g. 2026
    uint8_t  month;  // 1..12
    uint8_t  day;    // 1..31
    uint8_t  dotw;   // day of the week, 0..6
    uint8_t  hour;   // 0..23
    uint8_t  min;    // 0..59
    uint8_t  sec;    // 0..59
} pcf85063_datetime_t;

// Attach to the shared I2C bus and ensure the oscillator is running.
esp_err_t pcf85063_init(i2c_master_bus_handle_t bus);

esp_err_t pcf85063_get(pcf85063_datetime_t *out);
esp_err_t pcf85063_set(const pcf85063_datetime_t *in);

// True if the datetime is within a sane range (used to detect an unset RTC).
bool pcf85063_time_valid(const pcf85063_datetime_t *t);
