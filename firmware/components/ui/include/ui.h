#pragma once
#include "esp_err.h"
#include <stdbool.h>

// Phase-4 watch UI: an 11-tile swipeable LVGL app (watch face, live vitals,
// tracking, morning report, position, history, alarm, settings, + a PPG-debug
// tile). The CO5300 QSPI display + FT3168 touch are brought up by `board`; this
// component builds the screens and is fed by the sensor task.

// Live readout pushed from the sensor task.
typedef struct {
    const char *time_str;   // "HH:MM:SS" (copied immediately)
    const char *date_str;   // e.g. "Wed  Jul 3" (copied immediately), NULL if unknown
    float       accel_g;    // accelerometer magnitude
    int         hr_bpm;     // heart rate, 0 if no finger / not locked
    int         spo2;       // SpO2 %, 0 if unavailable
    int         hrv_ms;     // live RMSSD (ms), 0 until enough beats
    int         sqi_pct;    // signal-quality index, 0..100
    bool        finger;     // PPG sensor covered
    int         batt_pct;   // 0..100, or -1 if unknown
    int         vbat_mv;    // battery voltage (mV)
    bool        charging;
    bool        vbus;       // USB present
} ui_status_t;

// Attach LVGL to the board display/touch and build the tiles.
esp_err_t ui_init(void);

// Update the live tiles (watch face / live vitals / tracking / PPG-debug). Safe
// to call from any task (locks LVGL); a no-op until ui_init() has built the UI.
void ui_set_status(const ui_status_t *s);

// Refresh the live PPG waveform (live-vitals pulse chart + PPG-debug graph) from
// ppg_copy_waveform(). Call from the sensor task at ~display rate.
void ui_update_waveform(void);

// Pump the UI once per loop iteration (no-op: LVGL runs on the esp_lvgl_port task).
void ui_tick(void);

// User-selected PPG sample rate for ACTIVE/live mode, chosen on the Settings page
// (one of 50/100/200/400/800 Hz; default 50). Read by the sensor task each loop.
uint16_t ui_hr_rate_hz(void);

// Manual display blank/unblank (LVGL + touch stay live so a double-tap wakes it).
// Driven by the Settings "Sleep display" button + a double-tap-anywhere gesture,
// but exposed so the app can force a known state (e.g. wake on returning to ACTIVE).
void ui_display_sleep(void);
void ui_display_wake(void);
bool ui_display_is_asleep(void);
