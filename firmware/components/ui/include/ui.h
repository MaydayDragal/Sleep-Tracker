#pragma once
#include "esp_err.h"
#include <stdbool.h>

// LVGL screens: watch face, tracking, morning report, settings (PLAN.md §4).
// The CO5300 QSPI display + FT3168 touch are brought up by `board` (which
// delegates to the managed Waveshare BSP); this component builds the screens.

// Live diagnostic readout pushed from the sensor task (Phase-1 bring-up).
typedef struct {
    const char *time_str;   // "HH:MM:SS" (copied immediately)
    float       accel_g;    // accelerometer magnitude
    int         batt_pct;   // 0..100, or -1 if unknown
    int         vbat_mv;    // battery voltage (mV)
    bool        charging;
    bool        vbus;       // USB present
} ui_status_t;

// Attach LVGL to the board display/touch and build the initial screen.
esp_err_t ui_init(void);

// Update the on-screen live readout. Safe to call from any task (locks LVGL);
// a no-op until ui_init() has built the screen.
void ui_set_status(const ui_status_t *s);

// Pump the UI once per loop iteration (no-op: LVGL runs on the esp_lvgl_port task).
void ui_tick(void);
