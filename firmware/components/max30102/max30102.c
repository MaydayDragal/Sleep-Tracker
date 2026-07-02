#include "max30102.h"
#include "esp_log.h"

// STUB (phase1): register map, FIFO drain, and INT handling to be implemented
// against the confirmed MAX30102 breakout. Interfaces are stable; bodies aren't.

static const char *TAG = "max30102";

esp_err_t max30102_init(i2c_master_bus_handle_t bus, const max30102_config_t *cfg)
{
    (void)bus;
    ESP_LOGW(TAG, "init stub: %u Hz, LED red/ir=%u/%u, int_gpio=%d",
             cfg ? cfg->sample_rate_hz : 0,
             cfg ? cfg->led_current_red : 0,
             cfg ? cfg->led_current_ir : 0,
             cfg ? cfg->int_gpio : -1);
    // TODO(phase1): probe WHO_AM_I (part id 0x15), reset, set FIFO/SPO2/LED regs.
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t max30102_read_fifo(max30102_sample_t *out, size_t max, size_t *out_count)
{
    (void)out; (void)max;
    if (out_count) *out_count = 0;
    return ESP_ERR_NOT_SUPPORTED;   // TODO(phase1)
}

esp_err_t max30102_shutdown(bool enable)
{
    ESP_LOGD(TAG, "shutdown(%d) stub", enable);
    return ESP_ERR_NOT_SUPPORTED;   // TODO(phase1)
}
