#pragma once
#include "esp_err.h"
#include "driver/i2c_master.h"
#include <stdint.h>
#include <stdbool.h>

// AXP2101 power-management IC (onboard, I2C @ 0x34): battery fuel gauge, voltage,
// and charge/USB state (PLAN.md §2.3). The managed Waveshare BSP does NOT manage
// the PMU, so this is our own minimal register-level reader. (Rail control for
// duty-cycling comes later; this covers the battery readout.)

typedef struct {
    int      batt_pct;      // 0..100 fuel gauge, or -1 if unknown / no battery
    uint16_t vbat_mv;       // battery voltage (mV) from the ADC
    bool     charging;      // battery is charging
    bool     vbus_present;  // USB/VBUS input present
} pmu_status_t;

// Attach to the shared I2C bus and enable the battery-voltage ADC channel.
esp_err_t pmu_init(i2c_master_bus_handle_t bus);

esp_err_t pmu_read(pmu_status_t *out);
