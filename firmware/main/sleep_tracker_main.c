// Sleep Tracker — entry point.
//
// Architecture (PLAN.md §3): the dual-core S3 lets us keep sensor acquisition
// isolated from the UI and SD I/O. One core samples the MAX3010x + QMI8658 and
// assembles epochs; the other drives LVGL and flushes logs. Neither can stall
// the other, so a screen redraw or a slow SD write never drops a heartbeat.

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "board.h"
#include "max30102.h"
#include "ppg.h"
#include "actigraphy.h"
#include "pcf85063.h"
#include "pmu.h"
#include "bodynet.h"
#include "sleep_core.h"
#include "ui.h"
#include "sync.h"

#include <math.h>
#include <stdio.h>

static const char *TAG = "main";

// -- Core 1: sensor acquisition + epoch assembly ----------------------------
static void sensor_task(void *arg)
{
    i2c_master_bus_handle_t bus;
    ESP_ERROR_CHECK(board_i2c_get_bus(&bus));

    // 400 Hz PPG gives headroom for the optional HRV feature (PLAN.md §3.3); HR/SpO2 need less.
    const max30102_config_t ppg_cfg = {
        .sample_rate_hz  = 400,
        .led_current_red = 40,
        .led_current_ir  = 40,
        .int_gpio        = -1,   // TODO(phase1): wire FIFO-ready INT to a spare GPIO
    };
    max30102_init(bus, &ppg_cfg);   // TODO(phase1): real driver
    actigraphy_init(bus);           // QMI8658 accel (real)
    pcf85063_init(bus);             // PCF85063 RTC (real)
    pmu_init(bus);                  // AXP2101 PMU (real)
    ppg_reset();
    sleep_core_init();

    // Seed the RTC if it powered up unset, so the clock ticks.
    pcf85063_datetime_t seed_check;
    if (pcf85063_get(&seed_check) != ESP_OK || !pcf85063_time_valid(&seed_check)) {
        const pcf85063_datetime_t seed = {
            .year = 2026, .month = 7, .day = 2, .dotw = 4,
            .hour = 12, .min = 0, .sec = 0,
        };
        pcf85063_set(&seed);
        ESP_LOGW(TAG, "RTC unset — seeded to 2026-07-02 12:00:00");
    }

    ESP_LOGI(TAG, "sensor task running on core %d", xPortGetCoreID());

    for (;;) {
        // Phase-1 bring-up: read the onboard chips and push a live readout.
        // TODO(phase1-2): drain MAX3010x FIFO -> ppg_process() -> sleep_core;
        // accumulate 30 s epochs and persist to SD.
        char timebuf[16] = "--:--:--";
        ui_status_t st = { .time_str = timebuf, .accel_g = 0.0f, .batt_pct = -1 };

        pcf85063_datetime_t t;
        if (pcf85063_get(&t) == ESP_OK) {
            snprintf(timebuf, sizeof timebuf, "%02u:%02u:%02u",
                     (unsigned)t.hour, (unsigned)t.min, (unsigned)t.sec);
        }

        float x, y, z;
        if (actigraphy_read_accel_g(&x, &y, &z) == ESP_OK) {
            st.accel_g = sqrtf(x * x + y * y + z * z);
        }

        pmu_status_t p;
        if (pmu_read(&p) == ESP_OK) {
            st.batt_pct = p.batt_pct;
            st.vbat_mv  = p.vbat_mv;
            st.charging = p.charging;
            st.vbus     = p.vbus_present;
        }

        // Throttled serial echo (~every 5 s) for off-screen verification.
        static int tick = 0;
        if ((tick++ % 10) == 0) {
            ESP_LOGI(TAG, "readout: t=%s accel=%dmg batt=%d%% vbat=%dmV %s",
                     timebuf, (int)(st.accel_g * 1000.0f), st.batt_pct,
                     st.vbat_mv, st.charging ? "CHG" : (st.vbus ? "USB" : "BAT"));
        }

        ui_set_status(&st);
        vTaskDelay(pdMS_TO_TICKS(500));
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
    // Phase-0 bring-up aid: give a freshly-attached serial monitor time to
    // connect before the one-shot boot banner + I2C scan print. Remove once
    // board bring-up is done.
    vTaskDelay(pdMS_TO_TICKS(3000));

    ESP_LOGI(TAG, "Sleep Tracker booting (Waveshare ESP32-S3-Touch-AMOLED-1.8)");

    ESP_ERROR_CHECK(board_init());       // shared I2C bus + enumeration scan
    board_sdcard_mount();                // night logs (no-op-safe if no card)
    bodynet_init();                      // BLE central for WT9011DCL body sensors + H10
    sync_init();                         // BLE/MQTT — stretch, no-op for now

    // UI on core 0, sensing pinned to core 1.
    xTaskCreatePinnedToCore(ui_task,     "ui",     6144, NULL, 5, NULL, 0);
    xTaskCreatePinnedToCore(sensor_task, "sensor", 8192, NULL, 6, NULL, 1);
}
