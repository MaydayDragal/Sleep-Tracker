// Sleep Tracker — entry point.
//
// Architecture (PLAN.md §3): the dual-core S3 lets us keep sensor acquisition
// isolated from the UI and SD I/O. One core samples the MAX3010x + QMI8658 and
// assembles epochs; the other drives LVGL and flushes logs. Neither can stall
// the other, so a screen redraw or a slow SD write never drops a heartbeat.

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "bsp.h"
#include "max30102.h"
#include "ppg.h"
#include "actigraphy.h"
#include "bodynet.h"
#include "sleep_core.h"
#include "ui.h"
#include "sync.h"

static const char *TAG = "main";

// -- Core 1: sensor acquisition + epoch assembly ----------------------------
static void sensor_task(void *arg)
{
    i2c_master_bus_handle_t bus;
    ESP_ERROR_CHECK(bsp_i2c_get_bus(&bus));

    // 400 Hz PPG gives headroom for the optional HRV feature (PLAN.md §3.3); HR/SpO2 need less.
    const max30102_config_t ppg_cfg = {
        .sample_rate_hz  = 400,
        .led_current_red = 40,
        .led_current_ir  = 40,
        .int_gpio        = -1,   // TODO(phase1): wire FIFO-ready INT to a spare GPIO
    };
    max30102_init(bus, &ppg_cfg);   // TODO(phase1): real driver
    actigraphy_init(bus);           // TODO(phase1): real driver
    ppg_reset();
    sleep_core_init();

    ESP_LOGI(TAG, "sensor task running on core %d", xPortGetCoreID());

    for (;;) {
        // TODO(phase1-2):
        //   1. drain MAX3010x FIFO -> ppg_process() -> sleep_core_add_beat()
        //   2. read actigraphy activity
        //   3. on each 30 s boundary: sleep_core_close_epoch() and persist to SD
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

// -- Core 0: display / touch / LVGL -----------------------------------------
static void ui_task(void *arg)
{
    ui_init();   // TODO(phase0/4): bring up SH8601 + FT3168 + LVGL
    ESP_LOGI(TAG, "ui task running on core %d", xPortGetCoreID());

    for (;;) {
        ui_tick();                       // lv_timer_handler() once implemented
        vTaskDelay(pdMS_TO_TICKS(16));   // ~60 fps budget
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "Sleep Tracker booting (Waveshare ESP32-S3-Touch-AMOLED-1.8)");

    ESP_ERROR_CHECK(bsp_init());         // PMU rails, clocks, shared I2C bus
    bsp_sdcard_mount("/sdcard");         // night logs (TODO: wire in bsp)
    bodynet_init();                      // BLE central for WT9011DCL body sensors + H10
    sync_init();                         // BLE/MQTT — stretch, no-op for now

    // UI on core 0, sensing pinned to core 1.
    xTaskCreatePinnedToCore(ui_task,     "ui",     6144, NULL, 5, NULL, 0);
    xTaskCreatePinnedToCore(sensor_task, "sensor", 6144, NULL, 6, NULL, 1);
}
