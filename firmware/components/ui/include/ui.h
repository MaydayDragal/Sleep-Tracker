#pragma once
#include "esp_err.h"

// LVGL screens: watch face, tracking, morning report, settings (PLAN.md §4).
// The SH8601 QSPI display + FT3168 touch panels are brought up by `bsp`
// (board-specific code); this component attaches LVGL to them and builds screens.

// Attach LVGL to the bsp-provided display/touch and build the initial screen.
esp_err_t ui_init(void);

// Pump the UI once per loop iteration (wraps lv_timer_handler()).
void ui_tick(void);
