// Sleep Tracker — entry point.
//
// Architecture (PLAN.md §3): the dual-core S3 keeps sensor acquisition isolated
// from the UI. Core 1 samples the MAX3010x + QMI8658 and assembles epochs; core
// 0 drives LVGL. In Phase 2 the sensor task runs in one of two modes:
//   ACTIVE   — on charger: live vitals on the display (the Phase-1 behavior).
//   TRACKING — off charger: display off, sensors duty-cycled, CPU light-sleeping
//              between wakes, each 30 s epoch logged to microSD.
// The trigger is VBUS: unplug to start a night, plug in to end it.

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

#include <math.h>
#include <stdio.h>

static const char *TAG = "main";

// Consecutive VBUS readings that must agree before starting/stopping a night, so
// a brief glitch can't spuriously begin or end recording. In ACTIVE the loop is
// ~120 ms (fast start); in TRACKING it is one duty wake (~30 s), a deliberate stop.
#define VBUS_DEBOUNCE 2

// Debug: stream every raw PPG sample as "R,<ir>,<red>" over the console while in
// ACTIVE mode, for offline analysis (tools/capture_ppg.py -> tools/analyze_ppg.py).
// Set to 0 to silence. Not emitted during TRACKING (the console is idle then).
#define PPG_RAW_STREAM 1

// Set by the UI task once the display is up, so the sensor task never drives a
// mode transition (display off / LVGL stop) before LVGL exists.
static volatile bool s_display_ready = false;

// ACTIVE-mode HR/HRV duty-cycle. HR only needs a low sample rate (validated down
// to ~12.5 Hz offline; 50 Hz is the MAX30102 native floor and ~half the LED duty
// of 100 Hz), so we run HR at a low rate to save power and periodically bump to
// 400 Hz for an HRV snapshot. Testable over USB now; the same infra is meant to
// drive the battery TRACKING path later.
#define HR_RATE_HZ     50       // low-power HR-only rate
#define HRV_RATE_HZ    400      // high-rate window for precise HRV timing
#define HR_WINDOW_MS   20000    // stay in low-rate HR mode this long...
#define HRV_WINDOW_MS  15000    // ...then run 400 Hz this long, capturing HRV
static uint16_t s_ppg_rate;     // current sensor rate (0 => not yet configured)
static bool     s_hrv_window;   // true while in the 400 Hz HRV window
static int64_t  s_mode_t0;      // esp_timer at the current window's start

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
            ESP_LOGI(TAG, "readout: t=%s rate=%uHz hr=%d spo2=%d hrv=%dms finger=%d sqi=%.2f%s batt=%d%% %s",
                     timebuf, (unsigned)s_ppg_rate, st.hr_bpm, st.spo2, st.hrv_ms,
                     st.finger, v.sqi, v.valid ? "" : " (untrusted)", st.batt_pct,
                     st.charging ? "CHG" : (st.vbus ? "USB" : "BAT"));
        }

        ui_set_status(&st);
    }
}

// -- TRACKING mode: one duty-cycle wake ------------------------------------
// Every wake samples the accelerometer; every POWER_PPG_PERIOD_S it also runs a
// PPG capture window. sleep_core_service() closes any elapsed 30 s epochs. The
// trailing long vTaskDelay lets the FreeRTOS tickless idle drop the CPU into
// automatic light sleep until the next boundary (on battery — the USB console
// holds a no-sleep lock, so a bench run over USB validates cadence, not power).
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
        .led_current_red = 40,
        .led_current_ir  = 40,
        .int_gpio        = -1,   // polled FIFO; no INT line on this board
    };
    max30102_init(bus, &ppg_cfg);
    actigraphy_init(bus);
    pcf85063_init(bus);
    pmu_init(bus);
    ppg_reset();

    // Seed the RTC if it powered up unset, so timestamps are sane.
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
    bool last_vbus = true;   // assume on-charger at boot until a read says otherwise
    int  vbus_agree = 0;     // consecutive readings disagreeing with `tracking`
    int  wake_count = 0;
    for (;;) {
        bool vbus = last_vbus;   // on a failed read, retain last — never fake "plugged in"
        pmu_status_t p;
        if (pmu_read(&p) == ESP_OK) {
            vbus = p.vbus_present;
        }
        last_vbus = vbus;

        const bool want_tracking = !vbus;   // off charger => record the night
        if (want_tracking != tracking && ++vbus_agree >= VBUS_DEBOUNCE) {
            vbus_agree = 0;
            if (want_tracking) {
                power_enter_tracking();
                max30102_set_sample_rate(HRV_RATE_HZ);   // tracking runs full-rate PPG
                ppg_set_rate((float)HRV_RATE_HZ);
                sleep_core_request_start();
                sleep_core_service();           // apply the start (opens the SD log)
                tracking = true;
                wake_count = 0;
                ESP_LOGI(TAG, "VBUS lost -> start tracking");
            } else {
                sleep_core_request_stop();
                sleep_core_service();           // apply the stop (closes the SD log)
                max30102_shutdown(false);       // re-wake PPG for the live ACTIVE UI
                power_exit_tracking();
                ui_display_wake();              // clear any manual display-sleep so
                                               // ACTIVE never resumes behind the overlay
                tracking = false;
                s_ppg_rate = 0;                 // force the ACTIVE duty-cycle to reconfigure
                ESP_LOGI(TAG, "VBUS present -> stop tracking");
            }
        } else if (want_tracking == tracking) {
            vbus_agree = 0;
        }

        if (tracking) {
            tracking_wake(wake_count++);
        } else {
            // HR/HRV duty-cycle: low-rate HR most of the time, a periodic 400 Hz
            // window for HRV. Sensor + DSP rate are switched together.
            const int64_t now = esp_timer_get_time();
            if (s_ppg_rate == 0) {
                s_mode_t0 = now;
                active_set_ppg_rate(HR_RATE_HZ);
            }
            const int64_t elapsed_ms = (now - s_mode_t0) / 1000;
            if (!s_hrv_window && elapsed_ms >= HR_WINDOW_MS) {
                s_hrv_window = true;  s_mode_t0 = now;
                active_set_ppg_rate(HRV_RATE_HZ);
                ESP_LOGI(TAG, "duty-cycle: HRV window @ %d Hz", HRV_RATE_HZ);
            } else if (s_hrv_window && elapsed_ms >= HRV_WINDOW_MS) {
                s_hrv_window = false; s_mode_t0 = now;
                active_set_ppg_rate(HR_RATE_HZ);
                ESP_LOGI(TAG, "duty-cycle: HR low-power @ %d Hz", HR_RATE_HZ);
            }
            active_poll();
            // Poll fast enough for the FIFO at the current rate (32 deep): ~20 ms
            // at 400 Hz, a relaxed ~200 ms at 50 Hz (also less CPU wake).
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
        power_ui_gate_wait();             // blocks while TRACKING (display off)
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

    ESP_ERROR_CHECK(board_init());       // shared I2C bus + enumeration scan
    board_sdcard_mount();                // night logs (no-op-safe if no card)
    sleep_core_init();                   // epoch assembler / session state machine
    sd_logger_init();                    // register crash-safe SD persistence hooks
    power_init();                        // UI gate + ACTIVE power profile
    bodynet_init();                      // BLE central for WT9011DCL body sensors + H10
    sync_init();                         // BLE/MQTT — stretch, no-op for now

    // UI on core 0, sensing pinned to core 1.
    xTaskCreatePinnedToCore(ui_task,     "ui",     6144, NULL, 5, NULL, 0);
    xTaskCreatePinnedToCore(sensor_task, "sensor", 8192, NULL, 6, NULL, 1);
}
