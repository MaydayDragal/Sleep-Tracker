#pragma once
#include "esp_err.h"
#include "driver/i2c_master.h"

// QMI8658 6-axis IMU (WRIST) driven in low-power accel-only mode -> per-epoch
// activity counts (band-passed accelerometer magnitude), plus wake-on-motion.
// See PLAN.md §3.
//
// NOTE: authoritative sleep POSITION (back/left/right/belly) comes from the
// torso WT9011DCL via the `bodynet` component, not the wrist — the wrist's
// orientation is a poor proxy for how the body is lying (PLAN.md §2.4).

// Configure the QMI8658 (gyro off for power) on the shared I2C bus.
esp_err_t actigraphy_init(i2c_master_bus_handle_t bus);

// Latest accelerometer sample in g (any pointer may be NULL).
esp_err_t actigraphy_read_accel_g(float *ax, float *ay, float *az);

// Instantaneous activity proxy: |accel magnitude - 1 g|.
esp_err_t actigraphy_read_activity(float *out_count);
