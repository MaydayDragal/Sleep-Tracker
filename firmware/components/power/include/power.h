#pragma once
#include "esp_err.h"
#include <stdbool.h>

// Runtime power manager for the two operating modes (PLAN.md §2.3, §4):
//
//   ACTIVE   — on charger / watch face: display on, LVGL running, CPU pinned at
//              full speed, no light sleep. The live UI updates.
//   TRACKING — recording a night off-charger: display off, LVGL port stopped,
//              the UI task gated (blocked), CPU allowed to enter automatic
//              tickless light sleep between the sensor task's duty-cycle wakes.
//
// The trigger (VBUS unplug = start, plug = stop) and the duty-cycle loop live in
// the sensor task; this component owns the display/LVGL/PM-config side effects
// and the UI gate. Wake-on-motion is intentionally NOT implemented — the
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

// Enter/exit TRACKING. enter: gate the UI task, stop LVGL, blank the panel,
// allow light sleep. exit: restore full speed, panel, LVGL, and release the UI.
void power_enter_tracking(void);
void power_exit_tracking(void);

// Called at the top of the UI task loop; blocks while TRACKING so the UI task
// stops waking (letting tickless idle engage), returns immediately in ACTIVE.
void power_ui_gate_wait(void);

bool power_is_tracking(void);
