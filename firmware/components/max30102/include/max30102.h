#pragma once
#include "esp_err.h"
#include "driver/i2c_master.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// MAX30102 pulse-oximeter / PPG sensor (external, on the I2C expansion pads
// @ 0x57). Interrupt-driven FIFO drain keeps the CPU asleep between reads.
// See PLAN.md §2.2 and §3.3 (reliable-HRV timing requirements).

typedef struct {
    uint32_t red;   // red LED channel (SpO2)
    uint32_t ir;    // IR LED channel  (HR / HRV)
} max30102_sample_t;

typedef struct {
    uint16_t sample_rate_hz;   // 200 min, 400 preferred for HRV timing (§3.3)
    uint8_t  led_current_red;  // ~0.2 mA per step (0..255)
    uint8_t  led_current_ir;
    int      int_gpio;         // FIFO-ready interrupt line; -1 to poll
} max30102_config_t;

// Attach to the shared I2C bus and configure sampling/LED currents.
esp_err_t max30102_init(i2c_master_bus_handle_t bus, const max30102_config_t *cfg);

// Drain up to `max` samples from the FIFO; sets *out_count to samples read.
esp_err_t max30102_read_fifo(max30102_sample_t *out, size_t max, size_t *out_count);

// Low-power shutdown between duty-cycle windows (PLAN.md §2.3).
esp_err_t max30102_shutdown(bool enable);
