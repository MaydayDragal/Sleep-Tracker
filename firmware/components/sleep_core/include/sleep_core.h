#pragma once
#include "ppg.h"
#include "pcf85063.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// Epoch assembly, night-session state machine, persistence hooks, and (later)
// sleep scoring. See PLAN.md §3.1 (epoch record) and §3.2 (scoring).
//
// Ownership model (Phase 2): sleep_core OWNS epoch assembly and the session
// state machine and runs entirely on the sensor task (core 1). It NEVER calls
// the SD writer directly — instead a writer registers a `sleep_core_hooks_t`
// vtable via sleep_core_set_hooks(). This keeps FATFS access single-threaded
// and breaks any component dependency cycle (sd_logger -> sleep_core, one way).

#define SLEEP_EPOCH_SEC 30u   // one epoch record every 30 s (PLAN §3.1)

// Epoch flags (bitfield in sleep_epoch_t.flags, u16 — 10 distinct meanings).
enum {
    SLEEP_FLAG_WRIST_OFF    = 1u << 0,  // PPG ran but no finger/skin contact
    SLEEP_FLAG_MOTION       = 1u << 1,  // significant movement this epoch
    SLEEP_FLAG_DESAT        = 1u << 2,  // SpO2 dipped below 90 %
    SLEEP_FLAG_SENSOR_LOST  = 1u << 3,  // a sensor read failed this epoch
    SLEEP_FLAG_NO_PPG       = 1u << 4,  // PPG deliberately duty-cycled off
    SLEEP_FLAG_NO_CARDIAC   = 1u << 5,  // no usable HR/SpO2 this epoch (any cause)
    SLEEP_FLAG_RTC_UNSYNCED = 1u << 6,  // RTC was invalid at session start
    SLEEP_FLAG_SQI_PROXY    = 1u << 7,  // sqi is an epoch-level proxy, not real PPG SQI
    SLEEP_FLAG_HRV_VALID    = 1u << 8,  // rmssd_ms is from a qualified clean window
    SLEEP_FLAG_BATT_INVALID = 1u << 9,  // batt_pct not trustworthy (uncalibrated gauge)
};

// Event kinds for the events.log flight recorder (sleep_core_event / hooks.event).
enum {
    SLEEP_EV_START = 0,   // session started
    SLEEP_EV_STOP,        // session stopped
    SLEEP_EV_PPG_ON,      // PPG duty window opened
    SLEEP_EV_PPG_OFF,     // PPG duty window closed
    SLEEP_EV_WAKE,        // periodic wake heartbeat
    SLEEP_EV_SD_REOPEN,   // logger reopened the file after a write error
    SLEEP_EV_RESUMED,     // logger resumed on a fresh file after remount
};

// 30-second epoch record. Serialized to SD as one CSV row (see sd_logger).
typedef struct {
    uint32_t t_unix;         // epoch start = session_start + 30 * seq
    uint16_t activity;       // wrist actigraphy count (Σ|Δ|accel||, scaled)
    uint8_t  body_position;  // body_position_t from bodynet (0 = unknown; Phase 2.5)
    uint16_t body_activity;  // body-sensor movement count (Phase 2.5)
    uint8_t  hr_mean;
    uint8_t  hr_min;
    uint8_t  hr_max;
    uint16_t rmssd_ms;       // HRV for clean windows; 0 if not computed this epoch
    uint8_t  spo2_pct;
    uint8_t  sqi;            // 0..100 epoch-quality proxy (see SLEEP_FLAG_SQI_PROXY)
    uint8_t  batt_pct;
    uint8_t  beat_accept;    // % accepted beats backing hr_* / rmssd (§3.3)
    uint16_t flags;          // SLEEP_FLAG_* bitfield
} sleep_epoch_t;

typedef enum {
    SLEEP_IDLE,       // display off / active watch face, not recording
    SLEEP_TRACKING,   // recording a night
    SLEEP_MORNING,    // session ended, report available
} sleep_state_t;

// Persistence vtable. All callbacks run on the sensor task (core 1). Any may be
// NULL. log_open is called on session start (returns non-OK to abort logging but
// NOT the session), log_append once per closed epoch, log_fsync after each
// append, log_close on session stop. event() records a state transition.
typedef struct {
    esp_err_t (*log_open)(uint32_t t_start_unix, bool rtc_valid);
    void      (*log_append)(const sleep_epoch_t *ep);
    void      (*log_fsync)(void);
    void      (*log_close)(void);
    void      (*event)(uint32_t t_unix, uint8_t kind, int32_t detail);
} sleep_core_hooks_t;

// Reset session state (state = IDLE). Call once at boot.
void sleep_core_init(void);

// Register the persistence vtable (typically from sd_logger_init). Order-
// independent w.r.t. sleep_core_init as long as both precede the first session.
void sleep_core_set_hooks(const sleep_core_hooks_t *hooks);

// Request a session start/stop. Cheap and thread-safe (may be called from the
// UI/core 0); the request is applied on the next sleep_core_service() on core 1.
void sleep_core_request_start(void);
void sleep_core_request_stop(void);

sleep_state_t sleep_core_state(void);

// --- Live sensor feeds (sensor task / core 1 only) -------------------------
// No-ops unless a session is TRACKING. Accumulate into the open epoch.
void sleep_core_feed_accel(float ax, float ay, float az);
void sleep_core_feed_vitals(const ppg_vitals_t *v);
void sleep_core_add_beat(const ppg_beat_t *beat);

// Tell the assembler whether the PPG sensor is currently powered, so epochs with
// no PPG window are flagged NO_PPG rather than mistaken for wrist-off.
void sleep_core_set_ppg_powered(bool on);

// Note that a sensor read failed this epoch (-> SLEEP_FLAG_SENSOR_LOST).
void sleep_core_note_sensor_error(void);

// Apply any pending start/stop request, then close every epoch whose 30 s
// boundary has elapsed (a slept-through gap may span several). Returns the count
// closed. Call from the sensor task every wake.
int sleep_core_service(void);

// Emit a flight-recorder event (routes to hooks.event with an RTC timestamp).
void sleep_core_event(uint8_t kind, int32_t detail);

// RTC datetime -> Unix seconds. The RTC holds a naive wall clock; this treats it
// as UTC (tz_offset_min = 0 is our honesty marker — see the on-SD header). Pure
// and testable; valid for years 1901..2099. Golden: 1970-01-01 00:00:00 -> 0.
uint32_t sleep_core_datetime_to_unix(const pcf85063_datetime_t *dt);

// RMSSD (ms) over a window of accepted IBIs. Returns -1 if too few beats for a
// stable estimate (PLAN.md §3.3 gating).
float sleep_hrv_rmssd(const float *ibi_ms, size_t n);
