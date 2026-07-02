#include "sync.h"
#include "esp_log.h"

// STUB: Phase 5 BLE GATT sync + companion log-pull (PLAN.md §5), then the v3+
// integration roadmap I0-I5 — MQTT to Home Assistant + CPAP (INTEGRATION.md §7).

static const char *TAG = "sync";

esp_err_t sync_init(void)
{
    ESP_LOGD(TAG, "init stub — HA/MQTT + CPAP integration not enabled");
    return ESP_OK;
}
