#include "ui.h"
#include "esp_log.h"

// STUB (phase0/4): LVGL bring-up + screens. The SH8601 panel + FT3168 touch are
// initialized in `bsp` (board-specific); here we attach LVGL to them. On the S3
// the full 368x448 RGB565 frame buffer lives in PSRAM (PLAN.md §3).

static const char *TAG = "ui";

esp_err_t ui_init(void)
{
    ESP_LOGW(TAG, "init stub — LVGL not wired yet");
    // TODO(phase0): attach esp_lvgl_port to the panel + touch brought up in bsp.
    // TODO(phase4): watch face / tracking / morning-report / settings screens.
    return ESP_OK;
}

void ui_tick(void)
{
    // TODO(phase0): lv_timer_handler();
}
