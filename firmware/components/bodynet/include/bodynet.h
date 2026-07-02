#pragma once
#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

// Body-sensor network — the wrist unit acts as a BLE central for external
// wireless sensors (PLAN.md §2.4):
//   * 1..N WitMotion WT9011DCL 9-axis IMUs (BLE 5.0) — worn on the torso (and
//     optionally limbs). Onboard AHRS outputs Euler angles directly, so
//     sleep-position classification is just thresholding — no fusion here.
//   * Polar H10 — HRV reference (VALIDATION.md §3), shares this plumbing.
//
// WT9011DCL data: Nordic-UART-style GATT; frames are 0x55-framed, flag 0x61
// carries accel + angular velocity + Euler angle in one packet. Confirm exact
// service/characteristic UUIDs against the WitMotion datasheet at bring-up.

typedef enum {
    BODY_POS_UNKNOWN = 0,
    BODY_POS_BACK,      // supine
    BODY_POS_LEFT,      // left lateral
    BODY_POS_RIGHT,     // right lateral
    BODY_POS_BELLY,     // prone
    BODY_POS_UPRIGHT,   // sitting / out of bed (from pitch)
} body_position_t;

typedef enum {
    BODY_ROLE_TORSO = 0,   // authoritative position source
    BODY_ROLE_LEFT_LEG,
    BODY_ROLE_RIGHT_LEG,
    BODY_ROLE_OTHER,
} body_role_t;

typedef struct {
    uint8_t         mac[6];
    body_role_t     role;
    bool            connected;
    float           roll_deg;     // Euler angle from the WT9011DCL AHRS
    float           pitch_deg;
    float           yaw_deg;
    float           move_count;   // accumulated movement since last read
    uint8_t         battery_pct;
    uint32_t        last_seen_s;  // for mid-night dropout detection
} body_sensor_state_t;

// Start the BLE central (does nothing harmful if no sensors are paired).
esp_err_t bodynet_init(void);

// Enter pairing: scan for a WT9011DCL, bond, and remember it. Persisted across
// reboots so a configured sensor auto-reconnects each night.
esp_err_t bodynet_pair_start(void);
esp_err_t bodynet_assign_role(const uint8_t mac[6], body_role_t role);

// Enumerate paired/known sensors.
int       bodynet_sensor_count(void);
esp_err_t bodynet_get_sensor(int idx, body_sensor_state_t *out);

// Fused sleep position from the torso-role sensor (BODY_POS_UNKNOWN if none
// paired or the torso sensor is disconnected).
body_position_t bodynet_sleep_position(void);
