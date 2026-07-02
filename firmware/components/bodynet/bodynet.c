#include "bodynet.h"
#include "esp_log.h"

// STUB (phase2.5): NimBLE central, WT9011DCL discovery/bonding, 0x61-frame
// parsing, and torso-angle -> position classification to be implemented.
// Interfaces are stable so the rest of the system can bind to them now.

static const char *TAG = "bodynet";

esp_err_t bodynet_init(void)
{
    ESP_LOGW(TAG, "init stub — BLE central not started (no WT9011DCL/H10 yet)");
    // TODO(phase2.5): init NimBLE, load bonded sensors, begin auto-reconnect.
    return ESP_OK;
}

esp_err_t bodynet_pair_start(void)
{
    ESP_LOGI(TAG, "pair_start stub");
    return ESP_ERR_NOT_SUPPORTED;   // TODO(phase2.5)
}

esp_err_t bodynet_assign_role(const uint8_t mac[6], body_role_t role)
{
    (void)mac; (void)role;
    return ESP_ERR_NOT_SUPPORTED;   // TODO(phase2.5)
}

int bodynet_sensor_count(void)
{
    return 0;
}

esp_err_t bodynet_get_sensor(int idx, body_sensor_state_t *out)
{
    (void)idx; (void)out;
    return ESP_ERR_NOT_FOUND;       // TODO(phase2.5)
}

body_position_t bodynet_sleep_position(void)
{
    // TODO(phase2.5): threshold torso roll -> back/left/right/belly, pitch -> upright.
    return BODY_POS_UNKNOWN;
}
