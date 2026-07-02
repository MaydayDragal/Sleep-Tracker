#pragma once
#include "ppg.h"
#include <stdint.h>
#include <stddef.h>

// Epoch assembly, night-session state machine, persistence, and sleep scoring.
// See PLAN.md §3.1 (epoch record) and §3.2 (scoring).

// 30-second epoch record (~40 bytes). Also the row format written to SD.
typedef struct {
    uint32_t t_unix;         // epoch start, from the PCF85063 RTC
    uint16_t activity;       // wrist actigraphy count
    uint8_t  body_position;  // body_position_t from bodynet (torso WT9011DCL)
    uint16_t body_activity;  // body-sensor movement count
    uint8_t  hr_mean;
    uint8_t  hr_min;
    uint8_t  hr_max;
    uint16_t rmssd_ms;       // HRV for clean windows; 0 if not computed this epoch
    uint8_t  spo2_pct;
    uint8_t  sqi;            // 0..100 signal quality
    uint8_t  batt_pct;
    uint8_t  beat_accept;    // % accepted beats backing hr_* / rmssd (§3.3)
    uint8_t  flags;          // bitfield: wrist-off, motion, desat, sensor-lost, etc.
} sleep_epoch_t;

typedef enum {
    SLEEP_IDLE,       // display off, wake-on-motion armed
    SLEEP_TRACKING,   // recording a night
    SLEEP_MORNING,    // session ended, report available
} sleep_state_t;

void          sleep_core_init(void);
void          sleep_core_start_session(void);
void          sleep_core_stop_session(void);
sleep_state_t sleep_core_state(void);

// Feed accepted beats through the night; used for per-epoch HRV.
void sleep_core_add_beat(const ppg_beat_t *beat);

// Finalize the current 30 s epoch into *out and persist it. Called on the
// epoch boundary by the sensor task.
void sleep_core_close_epoch(sleep_epoch_t *out);

// RMSSD (ms) over a window of accepted IBIs. Returns -1 if too few beats for a
// stable estimate (PLAN.md §3.3 gating).
float sleep_hrv_rmssd(const float *ibi_ms, size_t n);
