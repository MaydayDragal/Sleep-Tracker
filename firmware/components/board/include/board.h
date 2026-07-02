#pragma once
#include "esp_err.h"
#include "driver/i2c_master.h"
#include <stdint.h>
#include <stdbool.h>

// ---------------------------------------------------------------------------
// Board support seam — Waveshare ESP32-S3-Touch-AMOLED-1.8 (primary target).
//
// This component is the ONLY place that talks to board-specific hardware, so an
// S3 -> C6 swap stays confined here (PLAN.md §2/§3). Internally it delegates the
// heavy bring-up (CO5300 QSPI display, FT/CST touch, LVGL, SDMMC, TCA9554
// IO-expander) to Waveshare's managed BSP component
// `waveshare/esp32_s3_touch_amoled_1_8`, and re-exports a small stable API so
// the rest of the firmware never depends on the vendor component directly.
//
// The vendor BSP owns the `bsp_` symbol namespace (bsp_i2c_init, bsp_display_*,
// bsp_sdcard_mount, ...). Our seam uses `board_` to avoid colliding with it.
// ---------------------------------------------------------------------------

// Shared I2C bus: peripheral 1 @ 400 kHz, SDA=GPIO15 SCL=GPIO14 (from the BSP).
#define BOARD_I2C_FREQ_HZ     400000

// Known/expected 7-bit addresses on the shared bus. Confirm on real hardware
// via board_i2c_scan() — some may not be populated on this board revision.
#define BOARD_ADDR_ES8311     0x18   // audio codec
#define BOARD_ADDR_TCA9554    0x20   // IO-expander (LCD/touch reset)
#define BOARD_ADDR_AXP2101    0x34   // PMU (presence unconfirmed on this rev)
#define BOARD_ADDR_FT3168     0x38   // capacitive touch (or CST816 variant)
#define BOARD_ADDR_PCF85063   0x51   // RTC
#define BOARD_ADDR_MAX30102   0x57   // external PPG sensor (expansion pads)
#define BOARD_ADDR_QMI8658    0x6B   // IMU (wrist actigraphy) — SA0 high on this board

// Bring up the shared I2C master bus (and log an enumeration scan). Call once,
// early, before any sensor driver attaches.
esp_err_t board_init(void);

// Hand back the shared I2C bus so sensor drivers can attach their devices.
esp_err_t board_i2c_get_bus(i2c_master_bus_handle_t *out_bus);

// Probe every 7-bit address and log which devices ACK (Phase 0 enumeration —
// settles which onboard chips are actually populated on this revision).
void board_i2c_scan(void);

// Mount the microSD card (FAT) at the BSP's configured mount point ("/sdcard").
esp_err_t board_sdcard_mount(void);

// Start display + touch + LVGL (delegates to the managed BSP). After this,
// take board_display_lock()/unlock() around any lv_* calls (LVGL is serviced by
// the esp_lvgl_port task inside the BSP — do NOT call lv_timer_handler here).
esp_err_t board_display_start(void);
bool      board_display_lock(uint32_t timeout_ms);
void      board_display_unlock(void);
