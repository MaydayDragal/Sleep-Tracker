#include "sync.h"
#include "esp_log.h"

// STUB (integration roadmap I0-I5, INTEGRATION.md §7).

static const char *TAG = "sync";

esp_err_t sync_init(void)
{
    ESP_LOGD(TAG, "init stub — HA/MQTT + CPAP integration not enabled");
    return ESP_OK;
}
