#pragma once
#include "esp_err.h"
#include "driver/i2c_master.h"

// ---------------------------------------------------------------------------
// Board support — Waveshare ESP32-S3-Touch-AMOLED-1.8 (primary target).
//
// The pin-compatible ESP32-C6 variant is the documented power-optimization
// fallback (PLAN.md §2). Keep ALL board-specific pin/peripheral definitions in
// this component so switching SoC stays confined here.
//
// TODO(phase0): confirm every GPIO below against the Rev1.1 schematic in
// https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.8 — the values here
// are placeholders until checked on real hardware.
// ---------------------------------------------------------------------------

// Shared I2C bus: AXP2101 PMU, QMI8658 IMU, PCF85063 RTC, FT3168 touch, ES8311
// codec — and the external MAX30102 joins it on the expansion pads.
#define BSP_I2C_PORT        0
#define BSP_I2C_SDA_GPIO    (-1)   // TODO: from schematic
#define BSP_I2C_SCL_GPIO    (-1)   // TODO: from schematic
#define BSP_I2C_FREQ_HZ     400000

// Known 7-bit addresses on the bus (all distinct from MAX30102 @ 0x57).
#define BSP_I2C_ADDR_ES8311    0x18
#define BSP_I2C_ADDR_AXP2101   0x34
#define BSP_I2C_ADDR_FT3168    0x38
#define BSP_I2C_ADDR_PCF85063  0x51
#define BSP_I2C_ADDR_MAX30102  0x57
#define BSP_I2C_ADDR_QMI8658   0x6A

// Bring up PMU rails, clocks, and the shared I2C master bus. Call once, early.
esp_err_t bsp_init(void);

// Hand back the shared I2C bus so sensor drivers can attach their devices.
esp_err_t bsp_i2c_get_bus(i2c_master_bus_handle_t *out_bus);

// Mount the microSD card (FAT) at mount_point. ESP_OK when ready.
esp_err_t bsp_sdcard_mount(const char *mount_point);
