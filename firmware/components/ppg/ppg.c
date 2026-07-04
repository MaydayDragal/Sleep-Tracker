#include "ppg.h"
#include "esp_timer.h"
#include <math.h>
#include <string.h>

// PPG pipeline (Phase 1 + accuracy passes):
//   raw -> median-3 despike (kills isolated sensor glitches, edge-preserving)
//       -> DC removal (high-pass) + 2-pole low-pass  => a ~0.16-6 Hz band-pass
//       -> PPG-only motion/artifact detection (suppresses beats + drops quality
//          during large non-cardiac excursions; the IMU version comes later)
//       -> adaptive-threshold peak detector with IBI plausibility gating
//       -> a real signal-quality index (perfusion x amplitude-consistency x
//          IBI-regularity x SNR) that gates whether the HR is trustworthy
//       -> rough ratio-of-ratios SpO2.
//
// The beat detector runs on a SELECTABLE pulse channel: IR for fingertip PPG
// (legacy default) or GREEN for wrist PPG (MAX30101). SpO2 always uses RED+IR.
//
// The SNR term follows Analog Devices' AN6410 definition for human PPG: split
// the signal into an in-band (cardiac, <~6 Hz) component and an out-of-band
// (>~6 Hz) residual and take the ratio of their powers. Our 2-pole low-pass
// already produces the in-band component (`lp`); the residual is (ac - lp). It is
// folded into the SQI as one extra factor (delete the marked line to revert to
// the legacy 3-term SQI) and reported as snr_db.
//
// A pure linear-filter swap (e.g. a Butterworth band-pass) was evaluated offline
// against captured data and did NOT beat the adaptive detector.
//
// All the filter coefficients are derived from the current sample rate (S.fs),
// which can change at RUNTIME via ppg_set_rate(). The absolute raw-count
// thresholds are additionally scaled by ADC bit depth (which drops with rate).

#define PPG_FS_DEFAULT   400.0f     // startup rate (until ppg_set_rate() is called)
#define HRV_RATE_MIN     200.0f     // only accumulate HRV IBIs at/above this rate
#define FINGER_IR_MIN    30000.0f   // IR DC below this => no finger (IR/fingertip, 18-bit)
#define FINGER_GREEN_MIN 3000.0f    // GREEN DC contact floor (wrist, 18-bit) — NEEDS
                                    // on-wrist tuning; green DC runs far below IR
#define DC_STEP_BASE     5000.0f    // fast-baseline-step threshold (18-bit magnitude)
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
#define FINGER_SETTLE_S  0.7f       // suppress beats this long after contact
#define PI_FULL          0.004f     // perfusion index (ACpp/DC) giving full quality (fingertip ~0.9%)
#define PI_MIN           0.0004f    // perfusion at/below which the perfusion term -> 0
#define SQI_VALID        0.3f       // vitals.valid requires the smoothed SQI above this
#define SNR_HALF         3.0f       // linear SNR at which the SNR quality term = 0.5 (~4.8 dB)

// --- Live HRV (for testing) ---
#define HRV_WIN          32         // rolling window of recent NORMAL IBIs
#define HRV_MIN_BEATS    6          // min IBIs before reporting a live RMSSD

static struct {
    float    dc_ir, dc_red, dc_green;   // slow baselines (the high-pass stage)
    float    lp;                    // band-passed pulse channel (DC-removed, low-passed)
    float    lp_prev, lp_prev2;     // filtered history for peak detection
    float    thr;                   // adaptive noise threshold (EMA of |lp|)
    float    peak_amp;              // tracked systolic-peak amplitude
    uint32_t idx;                   // running sample index
    int      last_beat_idx;         // -1 = none yet
    float    hr_bpm;
    float    spo2;
    float    ac_ir_max, ac_ir_min, ac_red_max, ac_red_min;  // per-beat extremes (SpO2)
    float    pulse_max, pulse_min;  // per-beat extremes on the pulse channel (PI/amp)
    bool     finger;
    bool     finger_prev;           // for detecting the contact edge
    bool     seeded;
    float    lp2;                   // second low-pass pole (=> 12 dB/oct roll-off)
    float    med_ir1, med_ir2;      // raw median-3 despike history (IR)
    float    med_red1, med_red2;    // raw median-3 despike history (RED)
    float    med_grn1, med_grn2;    // raw median-3 despike history (GREEN)
    int      art_hold;              // motion-artifact suppression counter (samples)
    float    sqi;                   // smoothed 0..1 signal-quality index
    float    p_sig, p_noise;        // EMA in-band / out-of-band power (AN6410 SNR)
    float    snr_db;                // derived, for display / logging
    float    ibi_win[HRV_WIN];      // rolling recent NORMAL IBIs (ms) for live HRV
    int      ibi_count, ibi_head;
    float    rmssd_ms;              // live RMSSD over the window (0 until HRV_MIN_BEATS)
    ppg_source_t src;               // pulse channel: IR (default) or GREEN
    // current sample rate + fs-derived coefficients (set in set_coeffs)
    float    fs;
    float    a_lp, a_dc, a_thr, a_pkdecay, a_pow;
    float    finger_min, dc_step;   // contact / fast-baseline-step thresholds,
                                    // scaled to the raw-count magnitude at fs
    float    sc;                    // the bit-depth scale factor (for the green threshold)
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
// live RMSSD = sqrt(mean(diff(IBI)^2)) over the ordered window.
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

// Derive all fs-dependent coefficients from a sample rate (alpha = 1 - e^{-2*pi*fc/fs}).
static void set_coeffs(float fs)
{
    S.fs = fs;
    S.a_lp  = 1.0f - expf(-2.0f * (float)M_PI * LP_FC_HZ / fs);
    S.a_dc  = 1.0f - expf(-2.0f * (float)M_PI * HP_FC_HZ / fs);
    S.a_thr = 1.0f / fs;                      // ~1 s envelope time constant
    S.a_pow = 3.0f / fs;                      // ~0.33 s power-EMA time constant (SNR)
    S.a_pkdecay = expf(-1.0f / (fs * 10.0f)); // ~10 s peak-amp decay
    S.wave_decim = (int)(fs / WAVE_TARGET_HZ + 0.5f);
    if (S.wave_decim < 1) S.wave_decim = 1;

    // The raw-count magnitude for a fixed photocurrent tracks the ADC bit depth,
    // which max30102.c's spo2_config() drops as the rate rises (18-bit @<=100 Hz,
    // 17-bit @200, 16-bit @400, 15-bit @800 — each dropped bit ~halves the count).
    // Scale the absolute count thresholds to match, so contact detection stays
    // rate-independent instead of failing at 400/800 Hz.
    S.sc = (fs <= 100.0f) ? 1.0f
         : (fs <= 200.0f) ? 0.5f
         : (fs <= 400.0f) ? 0.25f
                          : 0.125f;
    S.finger_min = FINGER_IR_MIN * S.sc;
    S.dc_step    = DC_STEP_BASE  * S.sc;
}

void ppg_reset(void)
{
    const ppg_source_t src = S.src;   // channel selection survives a reset
    memset(&S, 0, sizeof S);
    S.src = src;
    S.last_beat_idx = -1;
    set_coeffs(PPG_FS_DEFAULT);
}

void ppg_set_rate(float fs)
{
    if (fs <= 0.0f || fs == S.fs) {
        return;
    }
    set_coeffs(fs);
    // The FIFO is flushed on a rate change, so the sample-index timeline is
    // discontinuous: drop the beat-timing reference (one skipped IBI) and start a
    // fresh HRV window. Keep the band-pass / threshold / peak-amp / HR WARM.
    S.last_beat_idx = -1;
    S.art_hold = 0;
    S.ibi_count = S.ibi_head = 0;     // fresh HRV window per high-rate burst
}

void ppg_set_hr_source(ppg_source_t src)
{
    if (src == S.src) {
        return;
    }
    S.src = src;
    // The pulse channel changed — its DC level, amplitude and timing reference
    // are all different, so restart the detector transient (keep HR for display).
    S.last_beat_idx = -1;
    S.peak_amp = 0.0f;
    S.art_hold = (int)(FINGER_SETTLE_S * S.fs);
    S.ibi_count = S.ibi_head = 0;
}

// Process a batch of `n` samples. Returns true if at least one beat was accepted
// in the batch; `out_beat` (if non-NULL) receives only the MOST RECENT accepted
// beat of the batch. Every accepted beat still feeds the internal HRV window, so
// ppg.c's own RMSSD is complete — but a per-beat caller sees at most one beat per
// call. This is exact for the drain cadence in use (>=200 Hz polled every ~40 ms
// => <=1 beat per batch, well under the 0.35 s refractory); if the poll interval
// ever grows enough for a batch to span two beats, add a caller-supplied array.
bool ppg_process(const max30102_sample_t *samples, size_t n, ppg_beat_t *out_beat)
{
    bool beat = false;
    const bool use_green = (S.src == PPG_SRC_GREEN);
    const float contact_min = use_green ? (FINGER_GREEN_MIN * S.sc) : S.finger_min;

    for (size_t i = 0; i < n; i++) {
        const float ir_raw    = (float)samples[i].ir;
        const float red_raw   = (float)samples[i].red;
        const float green_raw = (float)samples[i].green;

        if (!S.seeded) {
            S.med_ir1  = S.med_ir2  = ir_raw;
            S.med_red1 = S.med_red2 = red_raw;
            S.med_grn1 = S.med_grn2 = green_raw;
            S.dc_ir = ir_raw;  S.dc_red = red_raw;  S.dc_green = green_raw;
            S.seeded = true;
        }

        // Median-3 despike on the raw channels (removes lone glitch samples).
        const float ir  = med3f(ir_raw,    S.med_ir1,  S.med_ir2);
        const float red = med3f(red_raw,   S.med_red1, S.med_red2);
        const float grn = med3f(green_raw, S.med_grn1, S.med_grn2);
        S.med_ir2  = S.med_ir1;  S.med_ir1  = ir_raw;
        S.med_red2 = S.med_red1; S.med_red1 = red_raw;
        S.med_grn2 = S.med_grn1; S.med_grn1 = green_raw;

        // High-pass: track each DC baseline; adapt fast on large steps (contact
        // on/off) so a slow baseline doesn't create a poisoning transient. The
        // step threshold is rate-scaled (S.dc_step).
        const float d_ir  = ir  - S.dc_ir;
        const float d_red = red - S.dc_red;
        const float d_grn = grn - S.dc_green;
        S.dc_ir    += (fabsf(d_ir)  > S.dc_step) ? d_ir  * 0.5f : d_ir  * S.a_dc;
        S.dc_red   += (fabsf(d_red) > S.dc_step) ? d_red * 0.5f : d_red * S.a_dc;
        S.dc_green += (fabsf(d_grn) > S.dc_step) ? d_grn * 0.5f : d_grn * S.a_dc;

        const float ac_ir  = ir  - S.dc_ir;
        const float ac_red = red - S.dc_red;
        const float ac_grn = grn - S.dc_green;

        // Pulse channel drives contact detection + beat detection.
        const float pulse_ac = use_green ? ac_grn : ac_ir;
        const float pulse_dc = use_green ? S.dc_green : S.dc_ir;
        S.finger = pulse_dc > contact_min;

        // On the contact edge, hold off beat detection while filters settle.
        if (S.finger && !S.finger_prev) {
            S.art_hold = (int)(FINGER_SETTLE_S * S.fs);
        }
        S.finger_prev = S.finger;

        // Two cascaded 1-pole low-passes => 12 dB/oct roll-off for a clean pulse.
        S.lp  += (pulse_ac - S.lp)  * S.a_lp;
        S.lp2 += (S.lp     - S.lp2) * S.a_lp;
        const float lp = S.lp2;   // detector/graph operate on the 2-pole output

        // AN6410 SNR: in-band power (the low-passed pulse) over out-of-band power
        // (what the low-pass rejected). EMA-smoothed so it tracks over ~0.3 s.
        const float resid = pulse_ac - lp;
        S.p_sig   += (lp * lp       - S.p_sig)   * S.a_pow;
        S.p_noise += (resid * resid - S.p_noise) * S.a_pow;
        const float snr_lin = S.p_sig / (S.p_noise + 1.0f);
        S.snr_db = 10.0f * log10f(snr_lin + 1e-9f);

        // Retain the filtered sample for the on-device debug graph.
        if (++S.wave_dcount >= S.wave_decim) {
            S.wave_dcount = 0;
            S.wave[S.wave_head % PPG_WAVE_N] = (int32_t)lp;
            S.wave_head++;
        }

        // PPG-only motion/artifact detection.
        const float dlp = lp - S.lp_prev;
        if (S.finger && S.peak_amp > 50.0f &&
            (fabsf(lp)  > ART_EXCURSION * S.peak_amp ||
             fabsf(dlp) > ART_SLEW      * S.peak_amp)) {
            S.art_hold = (int)(ART_HOLD_S * S.fs);
        } else if (S.art_hold > 0) {
            S.art_hold--;
        }
        const bool artifact = S.art_hold > 0;
        if (artifact) {
            S.sqi += (0.0f - S.sqi) * 0.02f;   // quality falls during motion
        }

        S.thr += (fabsf(lp) - S.thr) * S.a_thr;
        S.peak_amp *= S.a_pkdecay;   // slow decay so a stale/too-high amplitude self-heals

        // Track AC extremes: red/ir for SpO2, and the pulse channel for PI/amplitude.
        if (ac_ir  > S.ac_ir_max)  S.ac_ir_max  = ac_ir;
        if (ac_ir  < S.ac_ir_min)  S.ac_ir_min  = ac_ir;
        if (ac_red > S.ac_red_max) S.ac_red_max = ac_red;
        if (ac_red < S.ac_red_min) S.ac_red_min = ac_red;
        if (pulse_ac > S.pulse_max) S.pulse_max = pulse_ac;
        if (pulse_ac < S.pulse_min) S.pulse_min = pulse_ac;

        S.idx++;

        // Peak = previous filtered sample was a local max, above the noise floor
        // AND a strong fraction of the tracked systolic amplitude. Suppressed
        // entirely while a motion artifact is in progress.
        if (!artifact && S.finger &&
            S.lp_prev > lp && S.lp_prev >= S.lp_prev2 &&
            S.lp_prev > S.thr &&
            S.lp_prev > PEAK_FRAC * S.peak_amp) {
            const int peak_idx = (int)S.idx - 1;

            if (S.last_beat_idx < 0) {
                S.last_beat_idx = peak_idx;
                S.peak_amp = S.lp_prev;
            } else {
                const float ibi = (peak_idx - S.last_beat_idx) / S.fs;
                if (ibi >= REFRACTORY_S && ibi <= IBI_MAX_S) {
                    const float hr = 60.0f / ibi;
                    const bool plausible = (S.hr_bpm == 0.0f) ||
                        (hr > S.hr_bpm * 0.6f && hr < S.hr_bpm * 1.6f);
                    if (plausible) {
                        S.hr_bpm = (S.hr_bpm == 0.0f) ? hr
                                 : S.hr_bpm + (hr - S.hr_bpm) * 0.2f;
                        if (S.fs >= HRV_RATE_MIN) {
                            hrv_push(ibi * 1000.0f);
                        }
                    }

                    // Per-beat signal quality = perfusion x amplitude-consistency
                    // x IBI-regularity x SNR (all must hold), smoothed across beats.
                    const float pulse_pp = S.pulse_max - S.pulse_min;
                    const float pi = (pulse_dc > 0.0f) ? pulse_pp / pulse_dc : 0.0f;
                    const float sqi_pi  = clampf((pi - PI_MIN) / (PI_FULL - PI_MIN), 0.0f, 1.0f);
                    const float ratio   = (S.peak_amp > 1.0f) ? S.lp_prev / S.peak_amp : 1.0f;
                    const float sqi_amp = 1.0f - clampf(fabsf(1.0f - ratio), 0.0f, 1.0f);
                    const float sqi_reg = plausible ? 1.0f : 0.3f;
                    const float sqi_snr = clampf(snr_lin / (snr_lin + SNR_HALF), 0.0f, 1.0f);
                    float sqi_beat = sqi_pi * sqi_amp * sqi_reg;
                    sqi_beat *= sqi_snr;   // <-- AN6410 SNR factor; delete to revert to legacy SQI
                    S.sqi += (sqi_beat - S.sqi) * 0.3f;

                    S.peak_amp += (S.lp_prev - S.peak_amp) * 0.2f;

                    // Rough SpO2: R = (ACred/DCred)/(ACir/DCir). Always RED+IR.
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
            S.pulse_max = S.pulse_min = pulse_ac;
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
        .snr_db   = S.snr_db,
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
