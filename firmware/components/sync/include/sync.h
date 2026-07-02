#pragma once
#include "esp_err.h"

// Off-device sync: BLE/Wi-Fi + MQTT to Home Assistant, and the combined
// wrist + CPAP night summary. Full design in INTEGRATION.md. Stretch — no-op
// stub for now so app_main can call it unconditionally.
esp_err_t sync_init(void);
