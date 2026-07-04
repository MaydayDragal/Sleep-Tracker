#pragma once
#include "max30102.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// Streaming PPG pipeline: DC-removal + band-pass -> beat detection with
// sub-sample peak refinement -> inter-beat intervals (IBIs), HR, SpO2, and a
// signal-quality index. HRV (RMSSD) is computed downstream from accepted IBIs.
// The timing rationale for all of this lives in PLAN.md §3.3.
//
// The beat detector runs on a selectable "pulse" channel — IR for fingertip
// PPG (the legacy default) or GREEN for wrist PPG (MAX30101). SpO2 always uses
// RED+IR. Signal quality now includes an SNR term computed the way Analog
// Devices' AN6410 defines it for human PPG: in-band (cardiac) power over
// out-of-band (>~6 Hz) residual power.

// Band-passed PPG waveform samples retained for the on-device debug graph
// (100 Hz sampling => a ~2.4 s scrolling window).
#define PPG_WAVE_N 240

typedef enum {
    PPG_SRC_IR,      // fingertip PPG on the IR channel (default; MAX30102/30101)
    PPG_SRC_GREEN,   // wrist PPG on the green channel (MAX30101 only)
} ppg_source_t;

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
    float sqi;      // 0..1 smoothed signal-quality index (perfusion x consistency x SNR)
    float snr_db;   // in-band / out-of-band power ratio of the pulse channel (AN6410)
    bool  finger;   // sensor is covered (pulse-channel DC above the contact threshold)
    bool  valid;    // finger + plausible HR + SQI above the trust floor
} ppg_vitals_t;

// Reset filter/detector state (call when (re)starting acquisition).
void ppg_reset(void);

// Change the processing sample rate at runtime to match the sensor (used by the
// HR/HRV duty-cycle). Retunes the filters and resets the detector transient;
// keeps the last vitals for display. HRV (RMSSD) only accumulates at >= 200 Hz.
// Pass the sensor's OUTPUT rate (max30102_output_rate_hz(), i.e. after averaging).
void ppg_set_rate(float fs);

// Select which channel drives beat detection / HR / HRV. SpO2 is unaffected
// (always RED+IR). Default PPG_SRC_IR. Switching resets the detector transient.
void ppg_set_hr_source(ppg_source_t src);

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
