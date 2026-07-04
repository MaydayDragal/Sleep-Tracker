#include "power.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

// Owns the ACTIVE/TRACKING mode CPU power profile and the (no-op-while-active) UI
// task gate. See power.h for the mode model. The panel blank/unblank lives in the
// UI layer (ui_display_sleep/wake) so the double-tap-to-wake overlay stays armed;
// LVGL/touch are deliberately kept running in TRACKING so the on-screen Stop
// button remains reachable.

static const char *TAG = "power";

#define UI_ACTIVE_BIT  (1u << 0)   // set = ACTIVE (UI runs); clear = TRACKING (UI blocks)

static EventGroupHandle_t s_evt;
static bool               s_tracking;

// Full speed, no light sleep. TRACKING keeps this SAME profile — it does NOT drop
// into DFS / automatic light sleep. With the QSPI panel + LVGL kept live for the
// on-screen Stop button, scaling APB down / light-sleeping glitched the live panel
// back on mid-session (the screen turned itself back on ~10 s after Start with no
// firmware wake). The panel-off (the dominant load) is the real TRACKING saving;
// deep CPU light-sleep needs a hardware wake source to coexist with an on-screen
// button and is deferred (PLAN.md §5 Phase 2).
static const esp_pm_config_t PM_ACTIVE = {
    .max_freq_mhz = 240,
    .min_freq_mhz = 240,
    .light_sleep_enable = false,
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

    // Gate the redundant UI task so it stops waking every frame (it blocks in
    // power_ui_gate_wait). LVGL keeps running on the esp_lvgl_port task, so
    // touch/the Stop button stay live; the panel is blanked separately by
    // ui_display_sleep() (which also arms the double-tap-to-wake overlay). The CPU
    // stays at the ACTIVE profile — no PM change — so the live panel stays off.
    xEventGroupClearBits(s_evt, UI_ACTIVE_BIT);
    ESP_LOGI(TAG, "-> TRACKING (panel off; CPU full-speed — deep sleep deferred)");
}

void power_exit_tracking(void)
{
    if (!s_tracking) {
        return;
    }
    s_tracking = false;

    xEventGroupSetBits(s_evt, UI_ACTIVE_BIT);
    ESP_LOGI(TAG, "-> ACTIVE");
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
