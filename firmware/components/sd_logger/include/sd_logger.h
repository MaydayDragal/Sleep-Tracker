#pragma once
#include "esp_err.h"
#include <stdbool.h>

// Crash-safe night logger. Registers a sleep_core_hooks_t vtable so the epoch
// assembler can persist without depending on this component (one-way dep:
// sd_logger -> sleep_core). Writes an append-only CSV epoch stream plus an
// events.log flight recorder to the already-mounted microSD card, fsync-ing
// after every epoch so a power cut loses at most the unflushed final line.
//
// Format (see tools/read_night.py):
//   /sdcard/sleeptrk/YYYYMMDD_HHMMSS.csv   one header row + one line per epoch
//   /sdcard/sleeptrk/events.log            t_unix,kind,detail  (session events)

// Register the persistence hooks into sleep_core. Call once at boot, after
// sleep_core_init() and board_sdcard_mount(). Safe even if no card is present —
// logging simply no-ops until a card is available at the next session.
esp_err_t sd_logger_init(void);

// True if an epoch file is currently open for writing.
bool sd_logger_is_logging(void);
