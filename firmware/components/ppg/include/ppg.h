#pragma once
#include "max30102.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// Streaming PPG pipeline: DC-removal + band-pass -> beat detection with
// sub-sample peak refinement -> inter-beat intervals (IBIs), HR, SpO2, and a
// signal-quality index. HRV (RMSSD) is computed downstream from accepted IBIs.
// The timing rationale for all of this lives in PLAN.md §3.3.

typedef enum {
    PPG_BEAT_NORMAL,    // accepted for HR/HRV
    PPG_BEAT_ECTOPIC,   // physiological but excluded from HRV
    PPG_BEAT_ARTIFACT,  // motion/noise — excluded
} ppg_beat_class_t;

typedef struct {
    float            ibi_ms;   // inter-beat interval, sub-sample refined
    ppg_beat_class_t cls;
    float            sqi;      // 0..1 signal-quality index for this beat
    uint64_t         t_us;     // timestamp of the fiducial point
} ppg_beat_t;

typedef struct {
    float hr_bpm;
    float spo2_pct;
    float sqi;     // 0..1
    bool  valid;   // false until enough clean signal has accumulated
} ppg_vitals_t;

// Reset filter/detector state (call when (re)starting acquisition).
void ppg_reset(void);

// Feed `n` raw samples. Returns true and fills *out_beat when a beat is
// detected this call; updates the running vitals either way.
bool ppg_process(const max30102_sample_t *samples, size_t n, ppg_beat_t *out_beat);

// Latest smoothed vitals (for the live screen and per-epoch summary).
ppg_vitals_t ppg_current_vitals(void);
