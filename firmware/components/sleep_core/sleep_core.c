#include "sleep_core.h"
#include "esp_log.h"
#include <math.h>

// Session/epoch plumbing is real enough to build on; scoring + persistence are
// stubbed until the sensor pipeline produces data (Phase 2-3).

static const char *TAG = "sleep_core";
static sleep_state_t s_state = SLEEP_IDLE;

void sleep_core_init(void)
{
    s_state = SLEEP_IDLE;
    ESP_LOGI(TAG, "init (state=IDLE)");
}

void sleep_core_start_session(void)
{
    s_state = SLEEP_TRACKING;
    ESP_LOGI(TAG, "session start");
    // TODO(phase2): open the SD log file, stamp start time from RTC.
}

void sleep_core_stop_session(void)
{
    s_state = SLEEP_MORNING;
    ESP_LOGI(TAG, "session stop");
    // TODO(phase3): full-night re-pass — actigraphy sleep/wake + HR/HRV stage
    // refinement, summary metrics, sleep score.
}

sleep_state_t sleep_core_state(void)
{
    return s_state;
}

void sleep_core_add_beat(const ppg_beat_t *beat)
{
    (void)beat;
    // TODO(phase2): accumulate accepted IBIs for the current epoch's HRV window.
}

void sleep_core_close_epoch(sleep_epoch_t *out)
{
    if (out) *out = (sleep_epoch_t){ 0 };
    // TODO(phase2): assemble from ppg vitals + actigraphy + PMU + RTC, append to SD.
}

// RMSSD = sqrt(mean(diff(IBI)^2)). Real and testable now — the pipeline that
// feeds it clean IBIs is what's still stubbed.
float sleep_hrv_rmssd(const float *ibi_ms, size_t n)
{
    if (ibi_ms == NULL || n < 2) {
        return -1.0f;   // too few beats for a stable estimate (§3.3)
    }
    double sumsq = 0.0;
    for (size_t i = 1; i < n; i++) {
        const double d = (double)ibi_ms[i] - (double)ibi_ms[i - 1];
        sumsq += d * d;
    }
    return (float)sqrt(sumsq / (double)(n - 1));
}
