// Sleep Tracker — entry point.
//
// Architecture (PLAN.md §3): the dual-core S3 keeps sensor acquisition isolated
// from the UI. Core 1 samples the MAX3010x + QMI8658 and assembles epochs; core
// 0 drives LVGL. In Phase 2 the sensor task runs in one of two modes:
//   ACTIVE   — live vitals on the display (the Phase-1 behavior).
//   TRACKING — recording a night: panel blanked, sensors duty-cycled, each 30 s
//              epoch logged to microSD. LVGL/touch stay live so the on-screen
//              Stop button remains reachable.
// The trigger is the on-screen Start/Stop button (ui.c): it calls
// sleep_core_request_start/stop(); the sensor task applies the request in
// sleep_core_service() and switches mode off sleep_core_state().

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "board.h"
#include "max30102.h"
#include "ppg.h"
#include "actigraphy.h"
#include "pcf85063.h"
#include "pmu.h"
#include "bodynet.h"
#include "sleep_core.h"
#include "sd_logger.h"
#include "power.h"
#include "ui.h"
#include "sync.h"
#include "nettime.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>   // abort()
#include <time.h>

static const char *TAG = "main";

// Debug: stream every raw PPG sample as "R,<ir>,<red>" over the console while in
// ACTIVE mode, for offline analysis (tools/capture_ppg.py -> tools/analyze_ppg.py).
// Set to 0 to silence. Not emitted during TRACKING (the console is idle then).
#define PPG_RAW_STREAM 1

// Set by the UI task once the display is up, so the sensor task never drives a
// mode transition (display off / LVGL stop) before LVGL exists.
static volatile bool s_display_ready = false;

// Local wall-clock time fetched over NTP in app_main() BEFORE any I2C is up (so
// WiFi can't wedge the shared I2C bus). 0 if unavailable. The sensor task writes
// it to the RTC once the RTC (I2C) is initialized. gmtime_r() yields local Y/M/D.
static time_t s_ntp_time;

// ACTIVE mode runs continuously at the user-selected PPG rate (Settings ->
// ui_hr_rate_hz(), one of 50/100/200/400/800 Hz, default 50). No HR/HRV rate
// switching — the sensor stays at whatever rate is chosen (live HRV only
// accumulates at >= 200 Hz; ppg.c gates that). TRACKING samples at HRV_RATE_HZ.
#define HRV_RATE_HZ    400      // full-rate PPG used while recording (TRACKING)
static uint16_t s_ppg_rate;     // current sensor rate (0 => not yet configured)

static void active_set_ppg_rate(uint16_t hz)
{
    if (hz == s_ppg_rate) {
        return;
    }
    max30102_set_sample_rate(hz);
    ppg_set_rate((float)hz);
    s_ppg_rate = hz;
}

// -- ACTIVE mode: drain PPG + refresh the live UI (the Phase-1 loop body) ----
static void active_poll(void)
{
    static int slow = 0, echo = 0, wave = 0;
    static int64_t rate_t0 = 0;
    static int rate_n = 0;

    max30102_sample_t fifo[32];
    size_t n = 0;
    if (max30102_read_fifo(fifo, 32, &n) == ESP_OK && n > 0) {
        ppg_beat_t beat;
        ppg_process(fifo, n, &beat);

        // Report the TRUE internal PPG sample rate once/sec (independent of the
        // debug stream), so we can tell whether the loop actually keeps up at 400 Hz.
        rate_n += (int)n;
        const int64_t now = esp_timer_get_time();
        if (rate_t0 == 0) {
            rate_t0 = now;
        } else if (now - rate_t0 >= 1000000) {
            ppg_vitals_t vd = ppg_current_vitals();
            ESP_LOGI(TAG, "ppg: %d Hz internal, ir=%lu hr=%d hrv=%dms sqi=%.2f",
                     (int)((int64_t)rate_n * 1000000 / (now - rate_t0)),
                     (unsigned long)fifo[0].ir, (int)(vd.hr_bpm + 0.5f),
                     (int)(vd.rmssd_ms + 0.5f), vd.sqi);
            rate_t0 = now;
            rate_n = 0;
        }
#if PPG_RAW_STREAM
        static int strd = 0;
        for (size_t k = 0; k < n; k++) {
            if ((strd++ & 1) == 0) {   // decimate the console stream to ~half rate for headroom
                printf("R,%lu,%lu\n", (unsigned long)fifo[k].ir, (unsigned long)fifo[k].red);
            }
        }
#endif
    }

    // Refresh the PPG graph ~every 100 ms only — the LVGL chart redraw is heavy,
    // and doing it every loop was stalling the 400 Hz FIFO drain (=> overflow).
    if ((wave++ % 5) == 0) {
        ui_update_waveform();
    }

    if ((slow++ % 25) == 0) {   // ~every 500 ms (loop is ~20 ms)
        char timebuf[16] = "--:--:--";
        char datebuf[16] = "";
        ui_status_t st = { .time_str = timebuf, .date_str = datebuf, .accel_g = 0.0f, .batt_pct = -1 };

        pcf85063_datetime_t t;
        if (pcf85063_get(&t) == ESP_OK) {
            static const char *dow[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
            static const char *mon[13] = { "", "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                           "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
            snprintf(timebuf, sizeof timebuf, "%02u:%02u:%02u",
                     (unsigned)t.hour, (unsigned)t.min, (unsigned)t.sec);
            snprintf(datebuf, sizeof datebuf, "%s  %s %u",
                     dow[t.dotw % 7], mon[t.month <= 12 ? t.month : 0], (unsigned)t.day);
        }

        float x, y, z;
        if (actigraphy_read_accel_g(&x, &y, &z) == ESP_OK) {
            st.accel_g = sqrtf(x * x + y * y + z * z);
        }

        ppg_vitals_t v = ppg_current_vitals();
        st.finger = v.finger;
        st.hr_bpm = (int)(v.hr_bpm + 0.5f);
        st.spo2   = (int)(v.spo2_pct + 0.5f);
        st.hrv_ms = (int)(v.rmssd_ms + 0.5f);
        st.sqi_pct = (int)(v.sqi * 100.0f + 0.5f);

        pmu_status_t p;
        if (pmu_read(&p) == ESP_OK) {
            st.batt_pct = p.batt_pct;
            st.vbat_mv  = p.vbat_mv;
            st.charging = p.charging;
            st.vbus     = p.vbus_present;
        }

        if ((echo++ % 10) == 0) {   // ~every 5 s
            ESP_LOGI(TAG, "readout: t=%s rate=%uHz hr=%d spo2=%d hrv=%dms finger=%d sqi=%.2f%s batt=%d%%/%dmV %s",
                     timebuf, (unsigned)s_ppg_rate, st.hr_bpm, st.spo2, st.hrv_ms,
                     st.finger, v.sqi, v.valid ? "" : " (untrusted)", st.batt_pct, st.vbat_mv,
                     st.charging ? "CHG" : (st.vbus ? "USB" : "BAT"));
        }

        ui_set_status(&st);
    }
}

// -- TRACKING mode: one duty-cycle wake ------------------------------------
// Every wake samples the accelerometer; every POWER_PPG_PERIOD_S it also runs a
// PPG capture window. sleep_core_service() closes any elapsed 30 s epochs. The
// trailing vTaskDelay only paces the epoch cadence — it does NOT light-sleep: the
// CPU stays at the ACTIVE power profile (full speed, light_sleep_enable=false) all
// night, because keeping the QSPI panel + LVGL/touch live for the on-screen Stop
// button ruled out DFS/automatic light sleep (it glitched the panel back on).
// Blanking the panel is the only TRACKING power saving; deep CPU sleep is deferred
// (see components/power).
static void tracking_wake(int wake_count)
{
    const int64_t t0 = esp_timer_get_time();
    const bool do_ppg = (wake_count % (POWER_PPG_PERIOD_S / POWER_EPOCH_S)) == 0;

    // Close any epoch boundaries crossed during the preceding light sleep BEFORE
    // feeding new samples — otherwise this wake's burst gets swept into the epoch
    // that just ended (misattributing the activity/motion by ~one epoch).
    sleep_core_service();

    if (do_ppg) {
        max30102_shutdown(false);
        sleep_core_set_ppg_powered(true);
        sleep_core_event(SLEEP_EV_PPG_ON, 0);
        ppg_reset();

        const int64_t end = esp_timer_get_time() + (int64_t)POWER_PPG_ON_S * 1000000;
        while (esp_timer_get_time() < end) {
            max30102_sample_t fifo[32];
            size_t n = 0;
            if (max30102_read_fifo(fifo, 32, &n) == ESP_OK && n > 0) {
                // Buffer the raw samples in PSRAM; flushed to SD per window below.
                sd_logger_raw_write(fifo, n, sleep_core_now_unix(), HRV_RATE_HZ);
                ppg_beat_t beat;
                if (ppg_process(fifo, n, &beat)) {
                    sleep_core_add_beat(&beat);
                }
            }
            float x, y, z;
            if (actigraphy_read_accel_g(&x, &y, &z) == ESP_OK) {
                sleep_core_feed_accel(x, y, z);
            } else {
                sleep_core_note_sensor_error();
            }
            ppg_vitals_t v = ppg_current_vitals();
            sleep_core_feed_vitals(&v);
            sleep_core_service();   // close boundaries crossed during the window
            vTaskDelay(pdMS_TO_TICKS(40));   // drain the 32-deep FIFO fast enough at 400 Hz
        }

        max30102_shutdown(true);
        sleep_core_set_ppg_powered(false);
        sleep_core_event(SLEEP_EV_PPG_OFF, 0);
        sd_logger_raw_flush();   // one large SD write per PPG window (PPG now off)
    } else {
        // Accel-only wake: a short burst so the activity count has samples.
        for (int i = 0; i < POWER_ACTI_BURST_N; i++) {
            float x, y, z;
            if (actigraphy_read_accel_g(&x, &y, &z) == ESP_OK) {
                sleep_core_feed_accel(x, y, z);
            } else {
                sleep_core_note_sensor_error();
            }
            vTaskDelay(pdMS_TO_TICKS(POWER_ACTI_BURST_MS));
        }
    }

    sleep_core_service();

    const int32_t elapsed_ms = (int32_t)((esp_timer_get_time() - t0) / 1000);
    const int32_t remain_ms  = POWER_EPOCH_S * 1000 - elapsed_ms;
    if (remain_ms > 20) {
        vTaskDelay(pdMS_TO_TICKS(remain_ms));
    }
}

// -- Core 1: sensor acquisition + epoch assembly ----------------------------
static void sensor_task(void *arg)
{
    i2c_master_bus_handle_t bus;
    ESP_ERROR_CHECK(board_i2c_get_bus(&bus));

    // 400 Hz PPG (SPO2_CONFIG 118 us / 16-bit). fs is the single knob: the driver
    // sets the register from sample_rate_hz and ppg.c derives its filter/IBI
    // constants from PPG_FS_HZ, so all three stay in sync. 400 Hz gives ~2.5 ms
    // beat-timing resolution for HRV (PLAN.md §3.3); the 32-deep FIFO fills in
    // ~80 ms, so the poll loops below drain it every ~40 ms.
    const max30102_config_t ppg_cfg = {
        .sample_rate_hz  = 400,
        .led_current_red = 60,   // ~12 mA (0.2 mA/step). Brighter LEDs => higher
        .led_current_ir  = 60,   // perfusion + SNR; ~208k finger DC @18-bit leaves
                                 // ~20% headroom below the 0x3FFFF (262143) ceiling.
        .int_gpio        = -1,   // polled FIFO; no INT line on this board
    };
    max30102_init(bus, &ppg_cfg);
    actigraphy_init(bus);
    pcf85063_init(bus);
    pmu_init(bus);
    ppg_reset();

    // Write the NTP time fetched in app_main() (before any I2C, so WiFi couldn't
    // wedge the bus) to the RTC. WiFi is already off by now. Runs before the seed
    // check so a good NTP time wins over the fixed fallback date.
    if (s_ntp_time > 0) {
        struct tm tmv;
        gmtime_r(&s_ntp_time, &tmv);
        const pcf85063_datetime_t dt = {
            .year  = (uint16_t)(tmv.tm_year + 1900), .month = (uint8_t)(tmv.tm_mon + 1),
            .day   = (uint8_t)tmv.tm_mday, .dotw = (uint8_t)tmv.tm_wday,
            .hour  = (uint8_t)tmv.tm_hour, .min  = (uint8_t)tmv.tm_min, .sec = (uint8_t)tmv.tm_sec,
        };
        if (pcf85063_set(&dt) == ESP_OK) {
            ESP_LOGI(TAG, "RTC set from NTP: %04u-%02u-%02u %02u:%02u:%02u",
                     (unsigned)dt.year, (unsigned)dt.month, (unsigned)dt.day,
                     (unsigned)dt.hour, (unsigned)dt.min, (unsigned)dt.sec);
        }
    }

    // Seed the RTC if it (still) powered up unset, so timestamps are sane.
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

    // Wait until the display is up before any mode transition can blank it.
    while (!s_display_ready) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    bool tracking = false;
    int  wake_count = 0;
    for (;;) {
        // Apply any pending start/stop request from the UI button (it runs on core
        // 0 and only sets a flag); the session actually opens/closes here on core 1.
        sleep_core_service();
        const bool want_tracking = (sleep_core_state() == SLEEP_TRACKING);

        if (want_tracking && !tracking) {
            power_enter_tracking();
            max30102_set_sample_rate(HRV_RATE_HZ);   // tracking runs full-rate PPG
            ppg_set_rate((float)HRV_RATE_HZ);
            tracking = true;
            wake_count = 0;
            ESP_LOGI(TAG, "Start pressed -> tracking");
        } else if (!want_tracking && tracking) {
            max30102_shutdown(false);       // re-wake PPG for the live ACTIVE UI
            power_exit_tracking();
            tracking = false;
            s_ppg_rate = 0;                 // force the ACTIVE duty-cycle to reconfigure
            ESP_LOGI(TAG, "Stop pressed -> active");
        }

        if (tracking) {
            tracking_wake(wake_count++);
        } else {
            // Run continuously at the user-selected rate (Settings -> ui_hr_rate_hz()).
            // Re-applied every loop; active_set_ppg_rate early-returns when unchanged,
            // so a Settings change takes effect within one poll.
            active_set_ppg_rate(ui_hr_rate_hz());
            active_poll();
            // Poll fast enough for the FIFO at the current rate (32 deep): ~20 ms
            // at >=200 Hz, a relaxed ~200 ms at low rates (also less CPU wake).
            vTaskDelay(pdMS_TO_TICKS(s_ppg_rate >= 200 ? 20 : 200));
        }
    }
}

// -- Core 0: display / touch / LVGL -----------------------------------------
static void ui_task(void *arg)
{
    ui_init();               // brings up CO5300 + FT3168 + LVGL, builds the screen
    s_display_ready = true;
    ESP_LOGI(TAG, "ui task running on core %d", xPortGetCoreID());

    for (;;) {
        power_ui_gate_wait();             // blocks the redundant UI task while TRACKING
                                          // (LVGL/touch keep running on the port task)
        ui_tick();                        // no-op: LVGL runs on the esp_lvgl_port task
        vTaskDelay(pdMS_TO_TICKS(16));    // ~60 fps budget
    }
}

void app_main(void)
{
    // Phase-0 bring-up aid: give a freshly-attached serial monitor time to
    // connect before the one-shot boot banner + I2C scan print.
    vTaskDelay(pdMS_TO_TICKS(3000));

    ESP_LOGI(TAG, "Sleep Tracker booting (Waveshare ESP32-S3-Touch-AMOLED-1.8)");

    // NTP time first, BEFORE any I2C is up: WiFi's interrupt latency wedges the
    // shared I2C bus if they overlap (froze the display/touch). nettime brings
    // WiFi up, fetches the time, and turns WiFi off — all before board_init().
    s_ntp_time = nettime_fetch();

    ESP_ERROR_CHECK(board_init());       // shared I2C bus + enumeration scan
    board_sdcard_mount();                // night logs (no-op-safe if no card)
    sleep_core_init();                   // epoch assembler / session state machine
    sd_logger_init();                    // register crash-safe SD persistence hooks
    sd_logger_set_ppg_rate(HRV_RATE_HZ); // recording always runs at the tracking rate
    power_init();                        // UI gate + ACTIVE power profile
    bodynet_init();                      // BLE central for WT9011DCL body sensors + H10
    sync_init();                         // BLE/MQTT — stretch, no-op for now

    // UI on core 0, sensing pinned to core 1. A failed create here (boot-time OOM)
    // would otherwise leave a dead display or a sensor-less device with no clue why.
    BaseType_t ok_ui = xTaskCreatePinnedToCore(ui_task,     "ui",     6144, NULL, 5, NULL, 0);
    BaseType_t ok_se = xTaskCreatePinnedToCore(sensor_task, "sensor", 8192, NULL, 6, NULL, 1);
    if (ok_ui != pdPASS || ok_se != pdPASS) {
        ESP_LOGE(TAG, "task create failed (ui=%d sensor=%d) — halting", (int)ok_ui, (int)ok_se);
        abort();
    }
}
