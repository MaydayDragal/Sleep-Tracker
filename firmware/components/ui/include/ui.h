#pragma once
#include "esp_err.h"

// LVGL screens: watch face, tracking, morning report, settings (PLAN.md §4).
// Stubbed until the SH8601 QSPI display + FT3168 touch + LVGL are wired.

// Bring up display, touch, and LVGL; build the initial screen.
esp_err_t ui_init(void);

// Pump the UI once per loop iteration (wraps lv_timer_handler()).
void ui_tick(void);
