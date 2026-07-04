#include "sleep_core.h"
#include "pmu.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <math.h>
#include <string.h>

// Phase-2 epoch assembler + night-session state machine. Runs on the sensor
// task (core 1). Turns the live PPG/accel/PMU streams into 30 s epoch records
// and hands each to the registered persistence vtable. See PLAN.md §3.1.

static const char *TAG = "sleep_core";

// Tunables ------------------------------------------------------------------
#define MOTION_MAG_THRESH   0.05f   // |Δ|accel|| (g) above this => "moving" sample
#define MOTION_EPOCH_FRAC   0.30f   // >30 % moving samples => MOTION flag
#define ACT_SCALE           1000.0f // activity count = Σ|Δmag| * this, clamped u16
#define HRV_MIN_BEATS       20      // min accepted NORMAL IBIs for a per-epoch RMSSD
#define HRV_MIN_ACCEPT_PCT  70      // and min beat-acceptance for that RMSSD
#define HRV_IBI_CAP         64      // ring cap for per-epoch accepted IBIs

// Session state -------------------------------------------------------------
static sleep_state_t       s_state = SLEEP_IDLE;
static sleep_core_hooks_t  s_hooks;             // zero-inited => all NULL
static volatile bool       s_req_start;
static volatile bool       s_req_stop;

static uint32_t s_t0_unix;      // Unix time at session start (from RTC)
static int64_t  s_t0_us;        // esp_timer at session start (survives light sleep)
static bool     s_rtc_valid;    // RTC was valid at start
static uint32_t s_epoch_index;  // number of epochs closed this session

// Current-epoch accumulator -------------------------------------------------
static struct {
    double   act_accum;      // Σ|Δ|accel|| over the epoch
    float    prev_mag;
    bool     have_prev_mag;
    uint32_t accel_n;        // total accel samples
    uint32_t motion_n;       // accel samples exceeding the motion threshold

    uint32_t hr_sum;         // Σ hr over valid vitals samples
    uint16_t hr_n;
    uint8_t  hr_min, hr_max;
    uint32_t spo2_sum;
    uint16_t spo2_n;
    uint8_t  spo2_min;

    uint32_t vitals_n;       // total vitals samples (PPG window on)
    uint32_t finger_n;       // vitals samples with a finger present

    uint32_t beats_total;
    uint32_t beats_accepted; // NORMAL beats
    float    ibi[HRV_IBI_CAP];
    size_t   ibi_n;

    bool     saw_ppg;        // PPG was powered at some point this epoch
    bool     sensor_lost;    // a sensor read failed this epoch
} acc;

static void acc_reset(void)
{
    memset(&acc, 0, sizeof acc);
    acc.hr_min   = 255;
    acc.spo2_min = 255;
}

// --- RTC <-> Unix (Howard-Hinnant days_from_civil; pure, tz-free) ----------
static int64_t days_from_civil(int y, unsigned m, unsigned d)
{
    y -= (m <= 2);
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);                 // [0, 399]
    const unsigned doy = (153u * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1; // [0, 365]
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;     // [0, 146096]
    return era * 146097 + (int64_t)doe - 719468;
}

uint32_t sleep_core_datetime_to_unix(const pcf85063_datetime_t *dt)
{
    if (dt == NULL) {
        return 0;
    }
    const int64_t days = days_from_civil(dt->year, dt->month, dt->day);
    const int64_t secs = days * 86400 + dt->hour * 3600 + dt->min * 60 + dt->sec;
    return (secs < 0) ? 0u : (uint32_t)secs;
}

// Session-relative Unix time now. Grid is fixed (t0 + 30*i) so recorded
// timestamps stay monotonic and drift-free regardless of loop cadence; esp_timer
// only measures elapsed time and keeps running through tickless light sleep.
static uint32_t now_unix(void)
{
    const int64_t elapsed_s = (esp_timer_get_time() - s_t0_us) / 1000000;
    int64_t t = (int64_t)s_t0_unix + elapsed_s;
    if (t < (int64_t)s_t0_unix) {
        t = s_t0_unix;
    }
    return (uint32_t)t;
}

void sleep_core_init(void)
{
    s_state = SLEEP_IDLE;
    s_req_start = false;
    s_req_stop  = false;
    acc_reset();
    ESP_LOGI(TAG, "init (state=IDLE, epoch=%us)", (unsigned)SLEEP_EPOCH_SEC);
}

void sleep_core_set_hooks(const sleep_core_hooks_t *hooks)
{
    if (hooks) {
        s_hooks = *hooks;
    } else {
        memset(&s_hooks, 0, sizeof s_hooks);
    }
}

void sleep_core_request_start(void) { s_req_start = true; }
void sleep_core_request_stop(void)  { s_req_stop  = true; }
sleep_state_t sleep_core_state(void) { return s_state; }
uint32_t sleep_core_now_unix(void)  { return now_unix(); }

void sleep_core_event(uint8_t kind, int32_t detail)
{
    if (s_hooks.event) {
        s_hooks.event(now_unix(), kind, detail);
    }
}

void sleep_core_set_ppg_powered(bool on)
{
    if (on) {
        acc.saw_ppg = true;   // this epoch had a PPG window
    }
}

void sleep_core_note_sensor_error(void)
{
    if (s_state == SLEEP_TRACKING) {
        acc.sensor_lost = true;   // -> SLEEP_FLAG_SENSOR_LOST on epoch close
    }
}

void sleep_core_feed_accel(float ax, float ay, float az)
{
    if (s_state != SLEEP_TRACKING) {
        return;
    }
    const float mag = sqrtf(ax * ax + ay * ay + az * az);
    if (acc.have_prev_mag) {
        const float d = fabsf(mag - acc.prev_mag);   // gravity-immune first difference
        acc.act_accum += d;
        if (d > MOTION_MAG_THRESH) {
            acc.motion_n++;
        }
    }
    acc.prev_mag = mag;
    acc.have_prev_mag = true;
    acc.accel_n++;
}

void sleep_core_feed_vitals(const ppg_vitals_t *v)
{
    if (s_state != SLEEP_TRACKING || v == NULL) {
        return;
    }
    acc.saw_ppg = true;      // a vitals sample implies the PPG window is running
    acc.vitals_n++;
    if (v->finger) {         // wrist-on is finger presence, independent of signal quality
        acc.finger_n++;
    }
    if (v->valid && v->hr_bpm > 0.0f) {
        const uint32_t hr = (uint32_t)(v->hr_bpm + 0.5f);
        acc.hr_sum += hr;
        acc.hr_n++;
        if (hr < acc.hr_min) acc.hr_min = (uint8_t)(hr > 255 ? 255 : hr);
        if (hr > acc.hr_max) acc.hr_max = (uint8_t)(hr > 255 ? 255 : hr);
    }
    if (v->valid && v->spo2_pct > 0.0f) {
        const uint32_t s = (uint32_t)(v->spo2_pct + 0.5f);
        acc.spo2_sum += s;
        acc.spo2_n++;
        if (s < acc.spo2_min) acc.spo2_min = (uint8_t)(s > 255 ? 255 : s);
    }
}

void sleep_core_add_beat(const ppg_beat_t *beat)
{
    if (s_state != SLEEP_TRACKING || beat == NULL) {
        return;
    }
    acc.beats_total++;
    if (beat->cls == PPG_BEAT_NORMAL) {
        acc.beats_accepted++;
        if (acc.ibi_n < HRV_IBI_CAP) {
            acc.ibi[acc.ibi_n++] = beat->ibi_ms;
        }
    }
}

// Build and persist the record for the currently-open epoch, then reset acc.
static void finalize_epoch(void)
{
    sleep_epoch_t ep;
    memset(&ep, 0, sizeof ep);
    ep.t_unix = s_t0_unix + s_epoch_index * SLEEP_EPOCH_SEC;

    // Actigraphy count.
    double act = acc.act_accum * ACT_SCALE;
    ep.activity = (act > 65535.0) ? 65535u : (uint16_t)(act + 0.5);

    uint16_t flags = SLEEP_FLAG_SQI_PROXY;
    if (!s_rtc_valid)   flags |= SLEEP_FLAG_RTC_UNSYNCED;
    if (acc.sensor_lost) flags |= SLEEP_FLAG_SENSOR_LOST;

    // Motion.
    float motion_frac = 0.0f;
    if (acc.accel_n > 0) {
        motion_frac = (float)acc.motion_n / (float)acc.accel_n;
        if (motion_frac > MOTION_EPOCH_FRAC) flags |= SLEEP_FLAG_MOTION;
    }

    // Cardiac.
    if (!acc.saw_ppg) {
        flags |= SLEEP_FLAG_NO_PPG | SLEEP_FLAG_NO_CARDIAC;
    } else {
        if (acc.hr_n == 0) {
            flags |= SLEEP_FLAG_NO_CARDIAC;
        } else {
            ep.hr_mean = (uint8_t)((acc.hr_sum + acc.hr_n / 2) / acc.hr_n);
            ep.hr_min  = acc.hr_min;
            ep.hr_max  = acc.hr_max;
        }
        if (acc.spo2_n > 0) {
            ep.spo2_pct = (uint8_t)((acc.spo2_sum + acc.spo2_n / 2) / acc.spo2_n);
            if (acc.spo2_min < 90) flags |= SLEEP_FLAG_DESAT;
        }
        // Wrist-off = PPG ran but rarely saw a finger.
        const float finger_frac = acc.vitals_n ? (float)acc.finger_n / (float)acc.vitals_n : 0.0f;
        if (finger_frac < 0.5f) flags |= SLEEP_FLAG_WRIST_OFF;

        if (acc.beats_total > 0) {
            ep.beat_accept = (uint8_t)((acc.beats_accepted * 100u) / acc.beats_total);
        }
        // Honest epoch-quality proxy (NOT a real PPG SQI): finger × acceptance × stillness.
        const float accept_frac = acc.beats_total ? (float)acc.beats_accepted / (float)acc.beats_total : 0.0f;
        float q = finger_frac * accept_frac * (1.0f - motion_frac);
        if (q < 0.0f) q = 0.0f;
        ep.sqi = (acc.hr_n == 0) ? 0u : (uint8_t)(q * 100.0f + 0.5f);

        // Per-epoch RMSSD only when a genuinely clean window happened to land in
        // this epoch. Real HRV capture is the opportunistic side-file (§3.3).
        if (acc.ibi_n >= HRV_MIN_BEATS && ep.beat_accept >= HRV_MIN_ACCEPT_PCT) {
            const float r = sleep_hrv_rmssd(acc.ibi, acc.ibi_n);
            if (r >= 0.0f) {
                ep.rmssd_ms = (r > 65535.0f) ? 65535u : (uint16_t)(r + 0.5f);
                flags |= SLEEP_FLAG_HRV_VALID;
            }
        }
    }

    // Battery — read once per epoch. 0 % is almost certainly the uncalibrated
    // gauge (see phase0-state), so flag it low-confidence rather than trusting it.
    pmu_status_t p;
    if (pmu_read(&p) == ESP_OK) {
        ep.batt_pct = (p.batt_pct > 0) ? (uint8_t)p.batt_pct : 0u;
        ep.vbat_mv  = p.vbat_mv;   // voltage is trustworthy even when the % gauge reads 0
        if (p.batt_pct <= 0) flags |= SLEEP_FLAG_BATT_INVALID;
    } else {
        flags |= SLEEP_FLAG_BATT_INVALID | SLEEP_FLAG_SENSOR_LOST;
    }

    ep.flags = flags;

    if (s_hooks.log_append) s_hooks.log_append(&ep);
    if (s_hooks.log_fsync)  s_hooks.log_fsync();

    s_epoch_index++;
    acc_reset();
}

static void do_start(void)
{
    pcf85063_datetime_t dt;
    s_rtc_valid = (pcf85063_get(&dt) == ESP_OK) && pcf85063_time_valid(&dt);
    s_t0_unix = s_rtc_valid ? sleep_core_datetime_to_unix(&dt) : 0u;
    s_t0_us   = esp_timer_get_time();
    s_epoch_index = 0;
    acc_reset();
    s_state = SLEEP_TRACKING;

    if (s_hooks.log_open) {
        esp_err_t err = s_hooks.log_open(s_t0_unix, s_rtc_valid);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "log_open failed (%s); tracking without persistence", esp_err_to_name(err));
        }
    }
    sleep_core_event(SLEEP_EV_START, (int32_t)s_t0_unix);
    ESP_LOGI(TAG, "session start (t0=%lu rtc_valid=%d)", (unsigned long)s_t0_unix, s_rtc_valid);
}

static void do_stop(void)
{
    // Discard the (<1 epoch) partial tail so every row is a full 30 s epoch.
    sleep_core_event(SLEEP_EV_STOP, (int32_t)s_epoch_index);
    if (s_hooks.log_close) s_hooks.log_close();
    s_state = SLEEP_MORNING;
    ESP_LOGI(TAG, "session stop (%lu epochs)", (unsigned long)s_epoch_index);
    // TODO(phase3): full-night re-pass — actigraphy sleep/wake + HR/HRV staging,
    // summary metrics, sleep score.
}

int sleep_core_service(void)
{
    if (s_req_start) {
        s_req_start = false;
        if (s_state != SLEEP_TRACKING) do_start();
    }
    if (s_req_stop) {
        s_req_stop = false;
        if (s_state == SLEEP_TRACKING) do_stop();
    }
    if (s_state != SLEEP_TRACKING) {
        return 0;
    }

    int closed = 0;
    // Close every boundary that has elapsed — a slept-through PPG-off gap can
    // span many epochs at once. Cap the catch-up so a bad clock can't spin.
    while (now_unix() >= s_t0_unix + (uint32_t)(s_epoch_index + 1) * SLEEP_EPOCH_SEC) {
        finalize_epoch();
        if (++closed >= 4096) break;
    }
    return closed;
}

// RMSSD = sqrt(mean(diff(IBI)^2)). Real and testable now.
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
