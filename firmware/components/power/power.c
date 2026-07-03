#include "power.h"
#include "board.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

// Owns the ACTIVE/TRACKING mode side effects: CPU power profile, LVGL port,
// panel, and the UI gate. See power.h for the mode model.

static const char *TAG = "power";

#define UI_ACTIVE_BIT  (1u << 0)   // set = ACTIVE (UI runs); clear = TRACKING (UI blocks)

static EventGroupHandle_t s_evt;
static bool               s_tracking;

// ACTIVE: full speed, no light sleep — the UI needs to be responsive.
static const esp_pm_config_t PM_ACTIVE = {
    .max_freq_mhz = 240,
    .min_freq_mhz = 240,
    .light_sleep_enable = false,
};
// TRACKING: allow DFS down to 40 MHz and automatic tickless light sleep between
// duty-cycle wakes. Requires CONFIG_PM_ENABLE + CONFIG_FREERTOS_USE_TICKLESS_IDLE.
static const esp_pm_config_t PM_TRACKING = {
    .max_freq_mhz = 240,
    .min_freq_mhz = 40,
    .light_sleep_enable = true,
};

esp_err_t power_init(void)
{
    s_evt = xEventGroupCreate();
    if (s_evt == NULL) {
        return ESP_ERR_NO_MEM;
    }
    xEventGroupSetBits(s_evt, UI_ACTIVE_BIT);   // start ACTIVE
    s_tracking = false;
    esp_err_t err = esp_pm_configure(&PM_ACTIVE);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_pm_configure(ACTIVE) failed: %s", esp_err_to_name(err));
    }
    ESP_LOGI(TAG, "init (ACTIVE)");
    return ESP_OK;
}

void power_enter_tracking(void)
{
    if (s_tracking) {
        return;
    }
    s_tracking = true;

    // 1. Gate the UI task so it stops waking (it blocks in power_ui_gate_wait).
    xEventGroupClearBits(s_evt, UI_ACTIVE_BIT);
    // 2. Stop the esp_lvgl_port timer task (the real ~periodic waker) and blank
    //    the panel. Order: stop LVGL before the panel so no draw races the off.
    board_lvgl_stop();
    board_display_set_on(false);
    // 3. Allow automatic light sleep.
    esp_err_t err = esp_pm_configure(&PM_TRACKING);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_pm_configure(TRACKING) failed: %s", esp_err_to_name(err));
    }
    ESP_LOGI(TAG, "-> TRACKING (display off, light sleep armed)");
}

void power_exit_tracking(void)
{
    if (!s_tracking) {
        return;
    }
    s_tracking = false;

    esp_err_t err = esp_pm_configure(&PM_ACTIVE);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_pm_configure(ACTIVE) failed: %s", esp_err_to_name(err));
    }
    board_display_set_on(true);
    board_lvgl_resume();
    xEventGroupSetBits(s_evt, UI_ACTIVE_BIT);
    ESP_LOGI(TAG, "-> ACTIVE (display on)");
}

void power_ui_gate_wait(void)
{
    if (s_evt) {
        xEventGroupWaitBits(s_evt, UI_ACTIVE_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
    }
}

bool power_is_tracking(void)
{
    return s_tracking;
}
