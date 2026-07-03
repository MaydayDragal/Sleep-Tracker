#include "ppg.h"
#include "esp_timer.h"
#include <math.h>
#include <string.h>

// PPG pipeline (Phase 1 + accuracy pass): DC removal (high-pass) followed by a
// 1-pole low-pass => a ~0.16-5 Hz band-pass; an adaptive-threshold peak detector
// on the filtered IR channel for HR, with inter-beat-interval plausibility
// gating so a spurious/missed beat can't yank the displayed rate; and a rough
// ratio-of-ratios SpO2. The band-passed waveform is retained (decimated to ~100
// Hz) in a ring buffer for the on-device debug graph (ppg_copy_waveform).
//
// All the filter coefficients are derived from PPG_FS_HZ at ppg_reset(), so the
// sample rate is the single knob — change it here + the driver's SPO2_CONFIG +
// the FIFO poll cadence in main, and the filters retune themselves.

#define PPG_FS_HZ        400.0f     // MUST match the MAX30102 SPO2_CONFIG rate
#define FINGER_IR_MIN    30000.0f   // IR DC below this => no finger on the sensor
#define REFRACTORY_S     0.35f      // min beat spacing (=> max ~171 bpm)
#define IBI_MAX_S        2.0f       // max beat spacing (=> min 30 bpm)
#define PEAK_FRAC        0.5f       // a beat must reach this fraction of the tracked
                                    // systolic amplitude (rejects the dicrotic notch)
#define LP_FC_HZ         5.0f       // band-pass low-pass corner
#define HP_FC_HZ         0.16f      // baseline (high-pass) corner
#define WAVE_TARGET_HZ   100.0f     // decimate the graph waveform to ~this rate

static struct {
    float    dc_ir, dc_red;         // slow baselines (the high-pass stage)
    float    lp;                    // band-passed IR (DC-removed, low-passed)
    float    lp_prev, lp_prev2;     // filtered history for peak detection
    float    thr;                   // adaptive noise threshold (EMA of |lp|)
    float    peak_amp;              // tracked systolic-peak amplitude
    uint32_t idx;                   // running sample index
    int      last_beat_idx;         // -1 = none yet
    float    hr_bpm;
    float    spo2;
    float    ac_ir_max, ac_ir_min, ac_red_max, ac_red_min;  // per-beat extremes
    bool     finger;
    bool     seeded;
    // fs-derived coefficients (set in ppg_reset)
    float    a_lp, a_dc, a_thr, a_pkdecay;
    int      wave_decim, wave_dcount;
    int32_t  wave[PPG_WAVE_N];      // band-passed waveform ring (on-device graph)
    uint32_t wave_head;             // total decimated samples written
} S;

void ppg_reset(void)
{
    memset(&S, 0, sizeof S);
    S.last_beat_idx = -1;
    // 1-pole coefficients from the target corners (alpha = 1 - e^{-2*pi*fc/fs}).
    S.a_lp  = 1.0f - expf(-2.0f * (float)M_PI * LP_FC_HZ / PPG_FS_HZ);
    S.a_dc  = 1.0f - expf(-2.0f * (float)M_PI * HP_FC_HZ / PPG_FS_HZ);
    S.a_thr = 1.0f / PPG_FS_HZ;                  // ~1 s envelope time constant
    S.a_pkdecay = expf(-1.0f / (PPG_FS_HZ * 10.0f));  // ~10 s peak-amp decay
    S.wave_decim = (int)(PPG_FS_HZ / WAVE_TARGET_HZ + 0.5f);
    if (S.wave_decim < 1) S.wave_decim = 1;
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
        // High-pass: track the DC baseline; adapt fast on large steps (finger
        // on/off) so a slow baseline doesn't create a transient that poisons
        // peak_amp.
        const float d_ir  = ir  - S.dc_ir;
        const float d_red = red - S.dc_red;
        S.dc_ir  += (fabsf(d_ir)  > 5000.0f) ? d_ir  * 0.5f : d_ir  * S.a_dc;
        S.dc_red += (fabsf(d_red) > 5000.0f) ? d_red * 0.5f : d_red * S.a_dc;
        S.finger = S.dc_ir > FINGER_IR_MIN;

        const float ac_ir  = ir  - S.dc_ir;
        const float ac_red = red - S.dc_red;

        // Low-pass the AC => a clean band-passed pulse.
        S.lp += (ac_ir - S.lp) * S.a_lp;

        // Retain the filtered sample for the on-device debug graph (decimated so
        // the window duration is fs-independent).
        if (++S.wave_dcount >= S.wave_decim) {
            S.wave_dcount = 0;
            S.wave[S.wave_head % PPG_WAVE_N] = (int32_t)S.lp;
            S.wave_head++;
        }

        S.thr += (fabsf(S.lp) - S.thr) * S.a_thr;
        S.peak_amp *= S.a_pkdecay;   // slow decay so a stale/too-high amplitude self-heals

        // Track AC extremes for SpO2 (reset each accepted beat).
        if (ac_ir  > S.ac_ir_max)  S.ac_ir_max  = ac_ir;
        if (ac_ir  < S.ac_ir_min)  S.ac_ir_min  = ac_ir;
        if (ac_red > S.ac_red_max) S.ac_red_max = ac_red;
        if (ac_red < S.ac_red_min) S.ac_red_min = ac_red;

        S.idx++;

        // Peak = the previous filtered sample was a local max, above the noise
        // floor AND a strong fraction of the tracked systolic amplitude (so the
        // smaller dicrotic notch mid-cycle doesn't register as a second beat).
        if (S.finger &&
            S.lp_prev > S.lp && S.lp_prev >= S.lp_prev2 &&
            S.lp_prev > S.thr &&
            S.lp_prev > PEAK_FRAC * S.peak_amp) {
            const int peak_idx = (int)S.idx - 1;

            if (S.last_beat_idx < 0) {
                S.last_beat_idx = peak_idx;
                S.peak_amp = S.lp_prev;
            } else {
                const float ibi = (peak_idx - S.last_beat_idx) / PPG_FS_HZ;
                if (ibi >= REFRACTORY_S && ibi <= IBI_MAX_S) {
                    const float hr = 60.0f / ibi;
                    // Plausibility gate: a single IBI that implies an HR far from
                    // the running rate is almost always a missed or spurious beat
                    // — keep the detector synced but don't let it move the reading.
                    if (S.hr_bpm == 0.0f ||
                        (hr > S.hr_bpm * 0.6f && hr < S.hr_bpm * 1.6f)) {
                        S.hr_bpm = (S.hr_bpm == 0.0f) ? hr
                                 : S.hr_bpm + (hr - S.hr_bpm) * 0.2f;
                    }
                    S.peak_amp += (S.lp_prev - S.peak_amp) * 0.2f;

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
                    S.peak_amp = S.lp_prev;
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

        S.lp_prev2 = S.lp_prev;
        S.lp_prev  = S.lp;
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

size_t ppg_copy_waveform(int32_t *dst, size_t max)
{
    if (dst == NULL || max == 0) {
        return 0;
    }
    const uint32_t head = S.wave_head;                 // snapshot the writer's index
    const uint32_t n    = (PPG_WAVE_N < max) ? PPG_WAVE_N : (uint32_t)max;
    for (uint32_t i = 0; i < n; i++) {
        dst[i] = S.wave[(head - n + i) % PPG_WAVE_N];   // oldest..newest
    }
    return n;
}
