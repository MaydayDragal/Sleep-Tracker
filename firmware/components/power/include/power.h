#pragma once
#include "esp_err.h"
#include <stdbool.h>

// Runtime power manager for the two operating modes (PLAN.md §2.3, §4):
//
//   ACTIVE   — watch face / live vitals: display on, LVGL running, CPU pinned at
//              full speed, no light sleep. The live UI updates.
//   TRACKING — recording a night: the panel is blanked (via ui_display_sleep, the
//              dominant load) but LVGL/touch stay live so the on-screen Stop
//              button remains reachable; the redundant UI task is gated. The CPU
//              stays at the ACTIVE profile (full speed, no light sleep) — dropping
//              into DFS / automatic light sleep glitched the still-live QSPI panel
//              back on mid-session, so deep CPU sleep is deferred to a future mode
//              with a hardware wake source (the panel-off is the real saving).
//
// The trigger (the on-screen Start/Stop button -> sleep_core_request_start/stop)
// and the duty-cycle loop live in the sensor task; this component owns the UI gate
// (and the boot PM profile). Wake-on-motion is intentionally NOT implemented — the
// QMI8658 INT is not routed on this board; the duty cycle wakes on a timer.

// Duty-cycle schedule (owner-tunable). PPG is the power-hungry sensor, so it is
// duty-cycled; the accelerometer is sampled briefly every epoch.
#define POWER_EPOCH_S       30    // epoch cadence == accel wake cadence
#define POWER_PPG_PERIOD_S  300   // run one PPG capture window every 5 min
#define POWER_PPG_ON_S      45    // PPG capture duration per window
#define POWER_ACTI_BURST_N  4     // accel samples taken per accel-only wake
#define POWER_ACTI_BURST_MS 250   // spacing between burst samples

// Create the UI gate and apply the ACTIVE power profile. Call once at boot,
// after board_display_start() (i.e. after LVGL is up).
esp_err_t power_init(void);

// Enter/exit TRACKING. enter: gate the redundant UI task (LVGL/touch stay live;
// the panel is blanked by the UI layer; CPU stays full-speed). exit: release the
// UI task.
void power_enter_tracking(void);
void power_exit_tracking(void);

// Called at the top of the UI task loop; blocks while TRACKING so the redundant UI
// task stops waking every frame (LVGL/touch still run on the esp_lvgl_port task),
// returns immediately in ACTIVE.
void power_ui_gate_wait(void);

bool power_is_tracking(void);
