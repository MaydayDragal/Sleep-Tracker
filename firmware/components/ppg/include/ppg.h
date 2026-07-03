#pragma once
#include "max30102.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// Streaming PPG pipeline: DC-removal + band-pass -> beat detection with
// sub-sample peak refinement -> inter-beat intervals (IBIs), HR, SpO2, and a
// signal-quality index. HRV (RMSSD) is computed downstream from accepted IBIs.
// The timing rationale for all of this lives in PLAN.md §3.3.

// Band-passed PPG waveform samples retained for the on-device debug graph
// (100 Hz sampling => a ~2.4 s scrolling window).
#define PPG_WAVE_N 240

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
    float rmssd_ms; // live HRV over a rolling window of recent NORMAL beats; 0 until enough
    float sqi;     // 0..1 smoothed signal-quality index (perfusion x consistency)
    bool  finger;  // sensor is covered (DC above the finger threshold)
    bool  valid;   // finger + plausible HR + SQI above the trust floor
} ppg_vitals_t;

// Reset filter/detector state (call when (re)starting acquisition).
void ppg_reset(void);

// Feed `n` raw samples. Returns true and fills *out_beat when a beat is
// detected this call; updates the running vitals either way.
bool ppg_process(const max30102_sample_t *samples, size_t n, ppg_beat_t *out_beat);

// Latest smoothed vitals (for the live screen and per-epoch summary).
ppg_vitals_t ppg_current_vitals(void);

// Copy up to `max` most-recent band-passed waveform samples into `dst`, in
// chronological order (oldest..newest); returns the count copied. Feeds the
// on-device debug graph. Intended to be read from the same task that calls
// ppg_process() (the sensor task), so no locking is needed.
size_t ppg_copy_waveform(int32_t *dst, size_t max);
