#include "sd_logger.h"
#include "sleep_core.h"
#include "board.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
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

// Raw-PPG buffering. Samples accumulate in a PSRAM block and are flushed to the
// `_ppg.bin` file one block per PPG window (~45 s @ 400 Hz ≈ 108 KB) — a large,
// infrequent sequential SD write (~12/hour) instead of a stream of tiny writes,
// which is what saves the card's wear. 256 KB holds one window with headroom
// (even a 45 s window at 800 Hz is ~216 KB).
#define RAW_BUF_BYTES      (256 * 1024)
#define RAW_SAMPLE_BYTES   6            // red[3] + ir[3], little-endian 18-bit

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

// Raw-PPG state (PSRAM buffer + current binary file + in-progress block).
static uint8_t *s_raw_buf;       // PSRAM block buffer (NULL => raw logging disabled)
static FILE    *s_raw_f;         // open `_ppg.bin` for the current session
static size_t   s_raw_len;       // bytes buffered for the current block
static uint32_t s_raw_n;         // samples buffered for the current block
static uint32_t s_raw_t0;        // t_unix of the first sample in the current block
static uint16_t s_raw_rate;      // sample rate for the current block

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
    // Raw logging is best-effort: drop it for the rest of the session on a card
    // pull (the epoch CSV handles remount+resume; raw does not).
    if (s_raw_f) { fclose(s_raw_f); s_raw_f = NULL; }
    s_raw_len = 0; s_raw_n = 0; s_raw_t0 = 0;
    board_sdcard_mark_lost();
}

// Little-endian store helpers for the raw file/block headers.
static void put_u16(uint8_t *p, uint16_t v) { p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; }
static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; p[2] = (v >> 16) & 0xFF; p[3] = (v >> 24) & 0xFF;
}

// Open the session's raw-PPG file and write its 16-byte header. Best-effort:
// silently disables raw logging for the session if PSRAM/card/file is unavailable.
//   file header: 'P','P','G','1' | version(u16) | sample_bytes(u16) | t_start(u32) | rsv(u32)
static void open_raw_file(void)
{
    if (s_raw_buf == NULL || !ensure_card()) {
        return;
    }
    mkdir(LOG_DIR, 0777);

    char path[96];
    if (s_rtc_valid) {
        time_t tt = (time_t)s_t_start;
        struct tm tmv;
        gmtime_r(&tt, &tmv);
        snprintf(path, sizeof path, LOG_DIR "/%04d%02d%02d_%02d%02d%02d_ppg.bin",
                 tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                 tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    } else {
        snprintf(path, sizeof path, LOG_DIR "/unset_%03u_ppg.bin", (unsigned)s_unset_count);
    }

    s_raw_f = fopen(path, "wb");
    if (s_raw_f == NULL) {
        ESP_LOGW(TAG, "could not open raw PPG file (%s) — raw logging off this session", path);
        return;
    }
    uint8_t hdr[16] = { 'P', 'P', 'G', '1' };
    put_u16(&hdr[4], 1);                 // version
    put_u16(&hdr[6], RAW_SAMPLE_BYTES);  // bytes per sample
    put_u32(&hdr[8], s_t_start);         // session start (reference)
    // hdr[12..15] reserved (0)
    fwrite(hdr, 1, sizeof hdr, s_raw_f);
    fflush(s_raw_f);
    fsync(fileno(s_raw_f));
    s_raw_len = 0; s_raw_n = 0; s_raw_t0 = 0;
    ESP_LOGI(TAG, "raw PPG -> %s", path);
}

void sd_logger_raw_flush(void)
{
    if (s_raw_f == NULL || s_raw_n == 0) {
        return;
    }
    // Block header: 'P','R' | rate(u16) | t_unix(u32) | n_samples(u32) | then n*6 bytes.
    uint8_t bh[12] = { 'P', 'R' };
    put_u16(&bh[2], s_raw_rate);
    put_u32(&bh[4], s_raw_t0);
    put_u32(&bh[8], s_raw_n);
    if (fwrite(bh, 1, sizeof bh, s_raw_f) != sizeof bh ||
        fwrite(s_raw_buf, 1, s_raw_len, s_raw_f) != s_raw_len || ferror(s_raw_f)) {
        ESP_LOGW(TAG, "raw PPG write error — raw logging off this session");
        fclose(s_raw_f);
        s_raw_f = NULL;
    } else {
        fflush(s_raw_f);
        fsync(fileno(s_raw_f));
        ESP_LOGI(TAG, "raw PPG flush: %lu samples (%u B) @ %u Hz",
                 (unsigned long)s_raw_n, (unsigned)s_raw_len, (unsigned)s_raw_rate);
    }
    s_raw_len = 0; s_raw_n = 0; s_raw_t0 = 0;
}

void sd_logger_raw_write(const max30102_sample_t *samples, size_t n,
                         uint32_t t_unix, uint16_t rate_hz)
{
    if (s_raw_f == NULL || samples == NULL || n == 0) {
        return;
    }
    // Safety flush if this burst wouldn't fit (a window larger than the buffer);
    // normally the per-window flush keeps the buffer well under RAW_BUF_BYTES.
    if (s_raw_len + n * RAW_SAMPLE_BYTES > RAW_BUF_BYTES) {
        sd_logger_raw_flush();
    }
    if (s_raw_n == 0) {          // starting a fresh block
        s_raw_t0   = t_unix;
        s_raw_rate = rate_hz;
    }
    for (size_t i = 0; i < n; i++) {
        uint8_t *p = &s_raw_buf[s_raw_len];
        const uint32_t red = samples[i].red, ir = samples[i].ir;
        p[0] = red & 0xFF; p[1] = (red >> 8) & 0xFF; p[2] = (red >> 16) & 0xFF;
        p[3] = ir  & 0xFF; p[4] = (ir  >> 8) & 0xFF; p[5] = (ir  >> 16) & 0xFF;
        s_raw_len += RAW_SAMPLE_BYTES;
        s_raw_n++;
    }
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
    bool epoch_ok = open_epoch_file(false);
    open_raw_file();   // best-effort raw PPG side-file (no-op if PSRAM/card absent)
    return epoch_ok ? ESP_OK : ESP_FAIL;
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
    if (s_raw_f) {
        sd_logger_raw_flush();   // write the final (partial) raw block
        fclose(s_raw_f);
        s_raw_f = NULL;
    }
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

    // PSRAM block buffer for raw PPG (freed never — held for the app lifetime).
    s_raw_buf = heap_caps_malloc(RAW_BUF_BYTES, MALLOC_CAP_SPIRAM);
    if (s_raw_buf == NULL) {
        ESP_LOGW(TAG, "raw PPG buffer alloc failed (%d B PSRAM) — raw logging disabled",
                 (int)RAW_BUF_BYTES);
    } else {
        ESP_LOGI(TAG, "raw PPG PSRAM buffer: %d KB", (int)(RAW_BUF_BYTES / 1024));
    }

    ESP_LOGI(TAG, "hooks registered (logs -> %s)", LOG_DIR);
    return ESP_OK;
}

bool sd_logger_is_logging(void)
{
    return s_epoch_f != NULL;
}
