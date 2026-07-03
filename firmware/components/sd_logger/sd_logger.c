#include "sd_logger.h"
#include "sleep_core.h"
#include "board.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

// Crash-safe CSV night logger. Implements the sleep_core persistence vtable.
// All callbacks run on the sensor task (core 1) — single-threaded FATFS access.

static const char *TAG = "sd_logger";

#define LOG_DIR      "/sdcard/sleeptrk"
#define EVENTS_PATH  LOG_DIR "/events.log"

static const char *CSV_HEADER =
    "seq,t_unix,activity,body_position,body_activity,hr_mean,hr_min,hr_max,"
    "rmssd_ms,spo2_pct,sqi,batt_pct,beat_accept,flags\n";

static FILE    *s_epoch_f;
static FILE    *s_event_f;
static uint32_t s_seq;
static uint32_t s_t_start;
static bool     s_rtc_valid;
static bool     s_session;       // a session is active (want an epoch file open)
static uint32_t s_unset_count;   // disambiguates RTC-unset filenames
static uint32_t s_reopen_count;  // disambiguates "_r" filenames on repeated pulls

static const char *ev_name(uint8_t kind)
{
    switch (kind) {
        case SLEEP_EV_START:     return "START";
        case SLEEP_EV_STOP:      return "STOP";
        case SLEEP_EV_PPG_ON:    return "PPG_ON";
        case SLEEP_EV_PPG_OFF:   return "PPG_OFF";
        case SLEEP_EV_WAKE:      return "WAKE";
        case SLEEP_EV_SD_REOPEN: return "SD_REOPEN";
        case SLEEP_EV_RESUMED:   return "RESUMED";
        default:                 return "?";
    }
}

static bool ensure_card(void)
{
    if (board_sdcard_mounted()) {
        return true;
    }
    return board_sdcard_remount() == ESP_OK && board_sdcard_mounted();
}

// A write/fsync failed: the card was likely pulled. Close our handles (they sit
// on a dead FATFS volume — don't write through them, and they'd block a remount)
// and invalidate the mount so ensure_card() actually re-initializes it next time.
static void drop_card(void)
{
    if (s_epoch_f) { fclose(s_epoch_f); s_epoch_f = NULL; }
    if (s_event_f) { fclose(s_event_f); s_event_f = NULL; }
    board_sdcard_mark_lost();
}

static void open_events(void)
{
    if (s_event_f == NULL) {
        s_event_f = fopen(EVENTS_PATH, "a");
    }
}

// Open (or reopen) the epoch file. `resumed` names a fresh file after a card
// pull/remount and stamps a RESUMED marker so the reader knows it's a new grid.
// Each resumed open gets a distinct "_rN" suffix so no prior segment is clobbered.
static bool open_epoch_file(bool resumed)
{
    if (!ensure_card()) {
        return false;
    }
    mkdir(LOG_DIR, 0777);   // ignore EEXIST

    char suffix[12] = "";
    if (resumed) {
        snprintf(suffix, sizeof suffix, "_r%u", (unsigned)++s_reopen_count);
    }

    char path[96];
    if (s_rtc_valid) {
        time_t tt = (time_t)s_t_start;
        struct tm tmv;
        gmtime_r(&tt, &tmv);
        snprintf(path, sizeof path, LOG_DIR "/%04d%02d%02d_%02d%02d%02d%s.csv",
                 tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                 tmv.tm_hour, tmv.tm_min, tmv.tm_sec, suffix);
    } else {
        snprintf(path, sizeof path, LOG_DIR "/unset_%03u%s.csv",
                 (unsigned)s_unset_count, suffix);
    }

    s_epoch_f = fopen(path, "w");
    if (s_epoch_f == NULL) {
        // Last-ditch 8.3 fallback so a night is never lost to a naming issue.
        s_epoch_f = fopen(LOG_DIR "/night.csv", "a");
    }
    if (s_epoch_f == NULL) {
        ESP_LOGW(TAG, "could not open epoch file (%s)", path);
        return false;
    }
    // Write the header only for a genuinely empty file, so the "a" fallback
    // never produces a file with a header row spliced mid-stream.
    fseek(s_epoch_f, 0, SEEK_END);
    if (ftell(s_epoch_f) == 0) {
        fputs(CSV_HEADER, s_epoch_f);
    }
    fflush(s_epoch_f);
    fsync(fileno(s_epoch_f));
    s_seq = 0;
    ESP_LOGI(TAG, "logging to %s", path);
    return true;
}

// --- sleep_core_hooks_t vtable ---------------------------------------------

static esp_err_t hk_open(uint32_t t_start_unix, bool rtc_valid)
{
    s_t_start   = t_start_unix;
    s_rtc_valid = rtc_valid;
    s_session   = true;
    if (!rtc_valid) {
        s_unset_count++;
    }
    open_events();
    if (s_event_f) {
        fprintf(s_event_f,
                "# session start t_unix=%lu rtc_valid=%d ppg_fs_hz=100 epoch_sec=%u tz_offset_min=0\n",
                (unsigned long)t_start_unix, rtc_valid, (unsigned)SLEEP_EPOCH_SEC);
        fflush(s_event_f);
        fsync(fileno(s_event_f));
    }
    return open_epoch_file(false) ? ESP_OK : ESP_FAIL;
}

static void hk_append(const sleep_epoch_t *ep)
{
    if (!s_session || ep == NULL) {
        return;
    }
    // Reopen on a fresh file if a prior write error / card pull closed us.
    if (s_epoch_f == NULL) {
        if (!open_epoch_file(true)) {
            return;   // still no card — drop this epoch, retry next append (~30 s)
        }
        open_events();   // drop_card() closed events too; bring it back for the marker
        if (s_event_f) {
            fprintf(s_event_f, "%lu,%s,%ld\n", (unsigned long)ep->t_unix,
                    ev_name(SLEEP_EV_RESUMED), (long)ep->t_unix);
            fflush(s_event_f);
            fsync(fileno(s_event_f));
        }
    }

    int rc = fprintf(s_epoch_f,
        "%lu,%lu,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u\n",
        (unsigned long)s_seq, (unsigned long)ep->t_unix,
        (unsigned)ep->activity, (unsigned)ep->body_position, (unsigned)ep->body_activity,
        (unsigned)ep->hr_mean, (unsigned)ep->hr_min, (unsigned)ep->hr_max,
        (unsigned)ep->rmssd_ms, (unsigned)ep->spo2_pct, (unsigned)ep->sqi,
        (unsigned)ep->batt_pct, (unsigned)ep->beat_accept, (unsigned)ep->flags);

    if (rc < 0 || ferror(s_epoch_f)) {
        ESP_LOGW(TAG, "epoch write error — dropping card, will remount+reopen");
        drop_card();   // invalidate mount so ensure_card() remounts next append
        return;
    }
    s_seq++;
}

static void hk_fsync(void)
{
    if (s_epoch_f) {
        if (fflush(s_epoch_f) != 0 || fsync(fileno(s_epoch_f)) != 0) {
            ESP_LOGW(TAG, "fsync failed — dropping card, will remount+reopen");
            drop_card();
        }
    }
}

static void hk_close(void)
{
    s_session = false;
    if (s_epoch_f) {
        fflush(s_epoch_f);
        fsync(fileno(s_epoch_f));
        fclose(s_epoch_f);
        s_epoch_f = NULL;
    }
    if (s_event_f) {
        fflush(s_event_f);
        fsync(fileno(s_event_f));
        fclose(s_event_f);
        s_event_f = NULL;
    }
}

static void hk_event(uint32_t t_unix, uint8_t kind, int32_t detail)
{
    open_events();
    if (s_event_f) {
        fprintf(s_event_f, "%lu,%s,%ld\n", (unsigned long)t_unix, ev_name(kind), (long)detail);
        fflush(s_event_f);
        fsync(fileno(s_event_f));
    }
}

esp_err_t sd_logger_init(void)
{
    static const sleep_core_hooks_t hooks = {
        .log_open   = hk_open,
        .log_append = hk_append,
        .log_fsync  = hk_fsync,
        .log_close  = hk_close,
        .event      = hk_event,
    };
    sleep_core_set_hooks(&hooks);
    ESP_LOGI(TAG, "hooks registered (logs -> %s)", LOG_DIR);
    return ESP_OK;
}

bool sd_logger_is_logging(void)
{
    return s_epoch_f != NULL;
}
