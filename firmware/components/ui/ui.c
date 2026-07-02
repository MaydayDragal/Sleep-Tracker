#include "ui.h"
#include "esp_log.h"

// STUB (phase0/4): display/touch/LVGL bring-up. On the S3 the full 368x448
// RGB565 frame buffer lives in PSRAM (PLAN.md §3).

static const char *TAG = "ui";

esp_err_t ui_init(void)
{
    ESP_LOGW(TAG, "init stub — SH8601 + FT3168 + LVGL not wired yet");
    // TODO(phase0): esp_lcd SH8601 QSPI panel, FT3168 touch, esp_lvgl_port.
    // TODO(phase4): watch face / tracking / morning-report / settings screens.
    return ESP_OK;
}

void ui_tick(void)
{
    // TODO(phase0): lv_timer_handler();
}
