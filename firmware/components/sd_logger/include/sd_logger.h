#pragma once
#include "esp_err.h"
#include "max30102.h"   // max30102_sample_t (raw PPG capture)
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

// Crash-safe night logger. Registers a sleep_core_hooks_t vtable so the epoch
// assembler can persist without depending on this component (one-way dep:
// sd_logger -> sleep_core). Writes an append-only CSV epoch stream plus an
// events.log flight recorder to the already-mounted microSD card, fsync-ing
// after every epoch so a power cut loses at most the unflushed final line.
//
// Format (see tools/read_night.py / tools/read_raw_ppg.py):
//   /sdcard/sleeptrk/YYYYMMDD_HHMMSS.csv       one header row + one line per epoch
//   /sdcard/sleeptrk/events.log                t_unix,kind,detail  (session events)
//   /sdcard/sleeptrk/YYYYMMDD_HHMMSS_ppg.bin   raw PPG samples (one block per PPG window)

// Register the persistence hooks into sleep_core. Call once at boot, after
// sleep_core_init() and board_sdcard_mount(). Safe even if no card is present —
// logging simply no-ops until a card is available at the next session.
esp_err_t sd_logger_init(void);

// True if an epoch file is currently open for writing.
bool sd_logger_is_logging(void);

// Tell the logger the PPG sample rate used during a recording, so the events.log
// session-start marker records the real rate (`ppg_fs_hz=`) instead of a stale
// literal. Call once at boot (and any time the tracking rate changes).
void sd_logger_set_ppg_rate(uint32_t hz);

// Raw-PPG capture (sensor task / core 1, during a TRACKING session). Samples are
// buffered in PSRAM and flushed to the session's `_ppg.bin` in large blocks (one
// per PPG window) to minimize SD write count/wear. Both are no-ops unless the
// session's raw file and PSRAM buffer are available (best-effort — never blocks
// or fails the epoch log). raw_write appends `n` samples stamped `t_unix` at rate
// `rate_hz`; raw_flush writes the buffered block (call at each PPG-window end).
void sd_logger_raw_write(const max30102_sample_t *samples, size_t n,
                         uint32_t t_unix, uint16_t rate_hz);
void sd_logger_raw_flush(void);
