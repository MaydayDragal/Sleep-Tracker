#include "ppg.h"

// STUB (phase1): DSP pipeline to be implemented and tuned against recorded
// nights + ECG ground truth (PLAN.md §3.3, Phase 3 validation). The public
// shape (samples in -> beats/vitals out) is what the rest of the system binds
// to, so it is defined now even though the math is not.

static ppg_vitals_t s_vitals;

void ppg_reset(void)
{
    s_vitals = (ppg_vitals_t){ .valid = false };
}

bool ppg_process(const max30102_sample_t *samples, size_t n, ppg_beat_t *out_beat)
{
    (void)samples; (void)n; (void)out_beat;
    // TODO(phase1):
    //   - remove DC, band-pass ~0.5-5 Hz
    //   - detect systolic upstroke (max of 1st derivative) as the fiducial point
    //   - parabolic sub-sample interpolation to recover < 1 ms beat timing
    //   - classify beat (normal/ectopic/artifact); compute SQI
    //   - ratio-of-ratios red/ir -> SpO2
    return false;
}

ppg_vitals_t ppg_current_vitals(void)
{
    return s_vitals;
}
