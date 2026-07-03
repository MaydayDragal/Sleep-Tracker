#include "ppg.h"
#include "esp_timer.h"
#include <math.h>
#include <string.h>

// PPG pipeline (Phase 1 + accuracy passes):
//   raw -> median-3 despike (kills isolated sensor glitches, edge-preserving)
//       -> DC removal (high-pass) + 2-pole low-pass  => a ~0.16-5 Hz band-pass
//       -> PPG-only motion/artifact detection (suppresses beats + drops quality
//          during large non-cardiac excursions; the IMU version comes later)
//       -> adaptive-threshold peak detector with IBI plausibility gating
//       -> a real signal-quality index (perfusion x amplitude-consistency x
//          IBI-regularity) that gates whether the HR is trustworthy
//       -> rough ratio-of-ratios SpO2.
// The band-passed waveform is retained (decimated to ~100 Hz) in a ring buffer
// for the on-device debug graph (ppg_copy_waveform).
//
// A pure linear-filter swap (e.g. a Butterworth band-pass) was evaluated offline
// against captured data and did NOT beat the adaptive detector — so noise
// reduction here is despike (removes what a filter can't) + an SQI/artifact gate
// (don't emit HR you can't trust), not a fancier band-pass.
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
#define LP_FC_HZ         6.0f       // band-pass low-pass corner (2-pole cascade)
#define HP_FC_HZ         0.16f      // baseline (high-pass) corner
#define WAVE_TARGET_HZ   100.0f     // decimate the graph waveform to ~this rate

// --- Noise reduction / signal quality ---
#define ART_EXCURSION    4.0f       // |band-passed| beyond this x tracked amplitude => motion
#define ART_SLEW         2.5f       // per-sample jump beyond this x amplitude => motion
#define ART_HOLD_S       0.4f       // suppress beats + decay quality this long after motion
#define PI_FULL          0.004f     // perfusion index (ACpp/DC) giving full quality (fingertip ~0.9%)
#define PI_MIN           0.0004f    // perfusion at/below which the perfusion term -> 0
#define SQI_VALID        0.3f       // vitals.valid requires the smoothed SQI above this

// --- Live HRV (for testing) ---
#define HRV_WIN          32         // rolling window of recent NORMAL IBIs
#define HRV_MIN_BEATS    6          // min IBIs before reporting a live RMSSD

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
    float    lp2;                   // second low-pass pole (=> 12 dB/oct roll-off)
    float    med_ir1, med_ir2;      // raw median-3 despike history (IR)
    float    med_red1, med_red2;    // raw median-3 despike history (RED)
    int      art_hold;              // motion-artifact suppression counter (samples)
    float    sqi;                   // smoothed 0..1 signal-quality index
    float    ibi_win[HRV_WIN];      // rolling recent NORMAL IBIs (ms) for live HRV
    int      ibi_count, ibi_head;
    float    rmssd_ms;              // live RMSSD over the window (0 until HRV_MIN_BEATS)
    // fs-derived coefficients (set in ppg_reset)
    float    a_lp, a_dc, a_thr, a_pkdecay;
    int      wave_decim, wave_dcount;
    int32_t  wave[PPG_WAVE_N];      // band-passed waveform ring (on-device graph)
    uint32_t wave_head;             // total decimated samples written
} S;

// Median of three: returns the middle value (sum minus the extremes). A 3-tap
// median removes isolated single-sample spikes while leaving pulse edges intact.
static inline float med3f(float a, float b, float c)
{
    const float mx = fmaxf(a, fmaxf(b, c));
    const float mn = fminf(a, fminf(b, c));
    return a + b + c - mx - mn;
}

static inline float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

// Push a NORMAL inter-beat interval into the rolling window and recompute the
// live RMSSD = sqrt(mean(diff(IBI)^2)) over the ordered window (matches
// sleep_hrv_rmssd, but self-contained so ppg doesn't depend on sleep_core).
static void hrv_push(float ibi_ms)
{
    S.ibi_win[S.ibi_head] = ibi_ms;
    S.ibi_head = (S.ibi_head + 1) % HRV_WIN;
    if (S.ibi_count < HRV_WIN) S.ibi_count++;
    if (S.ibi_count >= HRV_MIN_BEATS) {
        const int start = (S.ibi_head - S.ibi_count + HRV_WIN) % HRV_WIN;
        float prev = S.ibi_win[start];
        double sumsq = 0.0;
        for (int k = 1; k < S.ibi_count; k++) {
            const float cur = S.ibi_win[(start + k) % HRV_WIN];
            const double d = (double)cur - (double)prev;
            sumsq += d * d;
            prev = cur;
        }
        S.rmssd_ms = (float)sqrt(sumsq / (double)(S.ibi_count - 1));
    }
}

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
        const float ir_raw  = (float)samples[i].ir;
        const float red_raw = (float)samples[i].red;

        if (!S.seeded) {
            S.med_ir1  = S.med_ir2  = ir_raw;
            S.med_red1 = S.med_red2 = red_raw;
            S.dc_ir = ir_raw;
            S.dc_red = red_raw;
            S.seeded = true;
        }

        // Median-3 despike on the raw channels (removes lone glitch samples
        // before they reach the filters/detector).
        const float ir  = med3f(ir_raw,  S.med_ir1,  S.med_ir2);
        const float red = med3f(red_raw, S.med_red1, S.med_red2);
        S.med_ir2  = S.med_ir1;  S.med_ir1  = ir_raw;
        S.med_red2 = S.med_red1; S.med_red1 = red_raw;
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

        // Two cascaded 1-pole low-passes => a 12 dB/oct roll-off (sharper HF-noise
        // rejection than a single pole) for a clean band-passed pulse.
        S.lp  += (ac_ir - S.lp)  * S.a_lp;
        S.lp2 += (S.lp  - S.lp2) * S.a_lp;
        const float lp = S.lp2;   // detector/graph operate on the 2-pole output

        // Retain the filtered sample for the on-device debug graph (decimated so
        // the window duration is fs-independent).
        if (++S.wave_dcount >= S.wave_decim) {
            S.wave_dcount = 0;
            S.wave[S.wave_head % PPG_WAVE_N] = (int32_t)lp;
            S.wave_head++;
        }

        // PPG-only motion/artifact detection: a cardiac pulse's band-passed
        // excursion stays bounded near the tracked systolic amplitude and can't
        // slew arbitrarily fast. A large excursion or step is motion — hold off
        // beat acceptance and let quality decay until it settles.
        const float dlp = lp - S.lp_prev;
        if (S.finger && S.peak_amp > 50.0f &&
            (fabsf(lp)  > ART_EXCURSION * S.peak_amp ||
             fabsf(dlp) > ART_SLEW      * S.peak_amp)) {
            S.art_hold = (int)(ART_HOLD_S * PPG_FS_HZ);
        } else if (S.art_hold > 0) {
            S.art_hold--;
        }
        const bool artifact = S.art_hold > 0;
        if (artifact) {
            S.sqi += (0.0f - S.sqi) * 0.02f;   // quality falls during motion
        }

        S.thr += (fabsf(lp) - S.thr) * S.a_thr;
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
        // Suppressed entirely while a motion artifact is in progress.
        if (!artifact && S.finger &&
            S.lp_prev > lp && S.lp_prev >= S.lp_prev2 &&
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
                    const bool plausible = (S.hr_bpm == 0.0f) ||
                        (hr > S.hr_bpm * 0.6f && hr < S.hr_bpm * 1.6f);
                    if (plausible) {
                        S.hr_bpm = (S.hr_bpm == 0.0f) ? hr
                                 : S.hr_bpm + (hr - S.hr_bpm) * 0.2f;
                        hrv_push(ibi * 1000.0f);   // feed the live HRV window
                    }

                    // Per-beat signal quality = perfusion x amplitude-consistency
                    // x IBI-regularity (all must hold), smoothed across beats.
                    const float ir_pp  = S.ac_ir_max  - S.ac_ir_min;
                    const float red_pp = S.ac_red_max - S.ac_red_min;
                    const float pi = (S.dc_ir > 0.0f) ? ir_pp / S.dc_ir : 0.0f;
                    const float sqi_pi  = clampf((pi - PI_MIN) / (PI_FULL - PI_MIN), 0.0f, 1.0f);
                    const float ratio   = (S.peak_amp > 1.0f) ? S.lp_prev / S.peak_amp : 1.0f;
                    const float sqi_amp = 1.0f - clampf(fabsf(1.0f - ratio), 0.0f, 1.0f);
                    const float sqi_reg = plausible ? 1.0f : 0.3f;
                    const float sqi_beat = sqi_pi * sqi_amp * sqi_reg;
                    S.sqi += (sqi_beat - S.sqi) * 0.3f;

                    S.peak_amp += (S.lp_prev - S.peak_amp) * 0.2f;

                    // Rough SpO2: R = (ACred/DCred)/(ACir/DCir).
                    if (S.dc_ir > 0 && S.dc_red > 0 && ir_pp > 0 && red_pp > 0) {
                        const float r = (red_pp / S.dc_red) / (ir_pp / S.dc_ir);
                        float spo2 = 110.0f - 25.0f * r;
                        if (spo2 > 100.0f) spo2 = 100.0f;
                        if (spo2 < 70.0f)  spo2 = 70.0f;
                        S.spo2 = (S.spo2 == 0) ? spo2 : S.spo2 + (spo2 - S.spo2) * 0.3f;
                    }

                    if (out_beat) {
                        out_beat->ibi_ms = ibi * 1000.0f;
                        out_beat->cls    = plausible ? PPG_BEAT_NORMAL : PPG_BEAT_ARTIFACT;
                        out_beat->sqi    = S.sqi;
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
            S.sqi = 0;
            S.last_beat_idx = -1;
            S.ibi_count = S.ibi_head = 0;   // drop stale HRV window on a new contact
            S.rmssd_ms = 0;
        }

        S.lp_prev2 = S.lp_prev;
        S.lp_prev  = lp;
    }

    return beat;
}

ppg_vitals_t ppg_current_vitals(void)
{
    ppg_vitals_t v = {
        .hr_bpm   = S.hr_bpm,
        .spo2_pct = S.spo2,
        .rmssd_ms = S.rmssd_ms,
        .sqi      = S.sqi,
        .finger   = S.finger,
        .valid    = S.finger && S.hr_bpm > 30.0f && S.hr_bpm < 220.0f &&
                    S.sqi > SQI_VALID,
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
