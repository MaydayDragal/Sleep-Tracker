#pragma once
#include "esp_err.h"
#include "driver/i2c_master.h"

// QMI8658 6-axis IMU driven in low-power accel-only mode -> per-epoch activity
// counts (band-passed accelerometer magnitude). Also the source for
// wake-on-motion and sleep-position tracking. See PLAN.md §3.

typedef enum {
    SLEEP_POS_UNKNOWN = 0,
    SLEEP_POS_BACK,
    SLEEP_POS_LEFT,
    SLEEP_POS_RIGHT,
    SLEEP_POS_FRONT,
} sleep_position_t;

// Configure the QMI8658 (gyro off for power) on the shared I2C bus.
esp_err_t actigraphy_init(i2c_master_bus_handle_t bus);

// Accumulated activity count since the last call (resets on read).
esp_err_t actigraphy_read_activity(float *out_count);

// Coarse wrist orientation -> sleep position (feeds positional-apnea analysis,
// INTEGRATION.md §6).
esp_err_t actigraphy_get_position(sleep_position_t *out_position);
