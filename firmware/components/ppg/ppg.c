#include "ppg.h"
#include "esp_timer.h"
#include <math.h>
#include <string.h>

// First-pass PPG pipeline (Phase 1 bring-up): DC removal + light smoothing, an
// adaptive-threshold peak detector on the IR channel for HR, and a rough
// ratio-of-ratios SpO2 estimate. This is enough to show a live, finger-responsive
// HR/SpO2; the HRV-grade fiducial/sub-sample work in PLAN.md §3.3 comes later.

#define PPG_FS_HZ        100.0f     // MUST match the MAX30102 sample rate
#define FINGER_IR_MIN    30000.0f   // IR DC below this => no finger on the sensor
#define REFRACTORY_S     0.35f      // min beat spacing (=> max ~171 bpm)
#define IBI_MAX_S        2.0f       // max beat spacing (=> min 30 bpm)
#define PEAK_FRAC        0.5f       // a beat must reach this fraction of the tracked
                                    // systolic amplitude (rejects the dicrotic notch)

static struct {
    float    dc_ir, dc_red;         // slow baselines
    float    ac_prev, ac_prev2;     // smoothed AC history for peak detection
    float    thr;                   // adaptive noise threshold (EMA of |AC|)
    float    peak_amp;              // tracked systolic-peak amplitude
    uint32_t idx;                   // running sample index
    int      last_beat_idx;         // -1 = none yet
    float    hr_bpm;
    float    spo2;
    float    ac_ir_max, ac_ir_min, ac_red_max, ac_red_min;  // per-beat extremes
    bool     finger;
    bool     seeded;
} S;

void ppg_reset(void)
{
    memset(&S, 0, sizeof S);
    S.last_beat_idx = -1;
}

bool ppg_process(const max30102_sample_t *samples, size_t n, ppg_beat_t *out_beat)
{
    bool beat = false;

    for (size_t i = 0; i < n; i++) {
        const float ir  = (float)samples[i].ir;
        const float red = (float)samples[i].red;

        if (!S.seeded) {
            S.dc_ir = ir;
            S.dc_red = red;
            S.seeded = true;
        }
        // Track the DC baseline; adapt fast on large steps (finger on/off) so a
        // slow baseline doesn't create a huge transient that poisons peak_amp.
        const float d_ir  = ir  - S.dc_ir;
        const float d_red = red - S.dc_red;
        S.dc_ir  += (fabsf(d_ir)  > 5000.0f) ? d_ir  * 0.5f : d_ir  * 0.01f;
        S.dc_red += (fabsf(d_red) > 5000.0f) ? d_red * 0.5f : d_red * 0.01f;
        S.finger = S.dc_ir > FINGER_IR_MIN;

        const float ac_ir  = ir  - S.dc_ir;
        const float ac_red = red - S.dc_red;
        const float ac_s   = (ac_ir + S.ac_prev + S.ac_prev2) / 3.0f;  // 3-tap smooth
        S.thr += (fabsf(ac_s) - S.thr) * 0.01f;
        S.peak_amp *= 0.999f;   // slow decay so a stale/too-high amplitude self-heals

        // Track AC extremes for SpO2 (reset each accepted beat).
        if (ac_ir  > S.ac_ir_max)  S.ac_ir_max  = ac_ir;
        if (ac_ir  < S.ac_ir_min)  S.ac_ir_min  = ac_ir;
        if (ac_red > S.ac_red_max) S.ac_red_max = ac_red;
        if (ac_red < S.ac_red_min) S.ac_red_min = ac_red;

        S.idx++;

        // Peak = the previous smoothed sample was a local max, above the noise
        // floor AND a strong fraction of the tracked systolic amplitude (so the
        // smaller dicrotic notch mid-cycle doesn't register as a second beat).
        if (S.finger &&
            S.ac_prev > ac_s && S.ac_prev >= S.ac_prev2 &&
            S.ac_prev > S.thr &&
            S.ac_prev > PEAK_FRAC * S.peak_amp) {
            const int peak_idx = (int)S.idx - 1;

            if (S.last_beat_idx < 0) {
                S.last_beat_idx = peak_idx;
                S.peak_amp = S.ac_prev;
            } else {
                const float ibi = (peak_idx - S.last_beat_idx) / PPG_FS_HZ;
                if (ibi >= REFRACTORY_S && ibi <= IBI_MAX_S) {
                    const float hr = 60.0f / ibi;
                    S.hr_bpm = (S.hr_bpm == 0) ? hr : S.hr_bpm + (hr - S.hr_bpm) * 0.2f;
                    S.peak_amp += (S.ac_prev - S.peak_amp) * 0.2f;

                    // Rough SpO2: R = (ACred/DCred)/(ACir/DCir).
                    const float ir_pp  = S.ac_ir_max  - S.ac_ir_min;
                    const float red_pp = S.ac_red_max - S.ac_red_min;
                    if (S.dc_ir > 0 && S.dc_red > 0 && ir_pp > 0 && red_pp > 0) {
                        const float r = (red_pp / S.dc_red) / (ir_pp / S.dc_ir);
                        float spo2 = 110.0f - 25.0f * r;
                        if (spo2 > 100.0f) spo2 = 100.0f;
                        if (spo2 < 70.0f)  spo2 = 70.0f;
                        S.spo2 = (S.spo2 == 0) ? spo2 : S.spo2 + (spo2 - S.spo2) * 0.3f;
                    }

                    if (out_beat) {
                        out_beat->ibi_ms = ibi * 1000.0f;
                        out_beat->cls    = PPG_BEAT_NORMAL;
                        out_beat->sqi    = 1.0f;
                        out_beat->t_us   = (uint64_t)esp_timer_get_time();
                    }
                    beat = true;
                    S.last_beat_idx = peak_idx;
                } else if (ibi > IBI_MAX_S) {
                    S.last_beat_idx = peak_idx;   // gap too long — restart
                    S.peak_amp = S.ac_prev;
                }
            }
            // Reset per-beat extremes.
            S.ac_ir_max = S.ac_ir_min = ac_ir;
            S.ac_red_max = S.ac_red_min = ac_red;
        }

        if (!S.finger) {
            S.hr_bpm = 0;
            S.spo2 = 0;
            S.last_beat_idx = -1;
        }

        S.ac_prev2 = S.ac_prev;
        S.ac_prev  = ac_s;
    }

    return beat;
}

ppg_vitals_t ppg_current_vitals(void)
{
    ppg_vitals_t v = {
        .hr_bpm   = S.hr_bpm,
        .spo2_pct = S.spo2,
        .sqi      = S.finger ? 1.0f : 0.0f,
        .valid    = S.finger && S.hr_bpm > 30.0f && S.hr_bpm < 220.0f,
    };
    return v;
}
