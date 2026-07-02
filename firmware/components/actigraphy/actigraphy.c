#include "actigraphy.h"
#include "esp_log.h"

// STUB (phase1): QMI8658 register setup + activity-count integration.

static const char *TAG = "actigraphy";

esp_err_t actigraphy_init(i2c_master_bus_handle_t bus)
{
    (void)bus;
    ESP_LOGW(TAG, "init stub");
    // TODO(phase1): probe WHO_AM_I (0x05), accel-only low-power ~50 Hz, gyro off,
    // configure motion interrupt for wake-on-motion.
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t actigraphy_read_activity(float *out_count)
{
    if (out_count) *out_count = 0.0f;
    return ESP_ERR_NOT_SUPPORTED;   // TODO(phase1)
}
