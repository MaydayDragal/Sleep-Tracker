#pragma once
#include "esp_err.h"
#include "driver/i2c_master.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// MAX3010x reflective pulse-oximeter / PPG sensor family — external, on the
// shared I2C bus @ 0x57. ONE driver covers both parts on the roadmap: they share
// the register map and both report PART_ID 0x15, differing only in fitted LEDs.
//   MAX30102 — RED (660 nm) + IR (880 nm). SpO2 + fingertip HR. On the board today.
//   MAX30101 — adds a GREEN (537 nm) LED. Green is the channel of choice for
//              WRIST reflective PPG: its shallow penetration and strong
//              hemoglobin absorption make it markedly more motion-robust than IR
//              at the wrist. That is the entire reason the '101 exists, and it is
//              what closes the "finger-not-wrist PPG" gap (see PLAN.md §2.2).
//
// Channels are selected with cfg.channels. RED|IR runs the part in SpO2 mode
// (0x03) — bit-identical to the legacy driver. Any set that includes GREEN (or
// any non-{RED,IR} set) runs Multi-LED mode (0x07) with one time-slot per
// channel; every enabled channel is returned per FIFO sample. Optional on-chip
// sample-averaging (SMP_AVE) trades output rate for SNR and I2C/CPU wake-ups; the
// die-temperature sensor is exposed for SpO2 wavelength compensation.
//
// The FIFO is polled (write/read pointers) as before; an INT line is supported
// for future power savings but this board does not wire one (cfg.int_gpio = -1).

// Channel select bits for max30102_config_t.channels (OR together).
typedef enum {
    MAX3010X_CH_RED   = 1u << 0,   // LED1, 660 nm — SpO2 + fingertip HR
    MAX3010X_CH_IR    = 1u << 1,   // LED2, 880 nm — SpO2 + fingertip HR
    MAX3010X_CH_GREEN = 1u << 2,   // LED3, 537 nm — wrist HR (MAX30101 only)
} max3010x_channel_t;

// One FIFO sample. Channels not enabled in the active config read back 0.
// NOTE: `green` is appended after the legacy {red, ir} — existing field-wise
// consumers (ppg.c, sd_logger.c) are unaffected.
typedef struct {
    uint32_t red;     // RED LED channel   (SpO2)
    uint32_t ir;      // IR LED channel    (SpO2 / fingertip HR)
    uint32_t green;   // GREEN LED channel (wrist HR; 0 on a MAX30102)
} max30102_sample_t;

typedef struct {
    uint8_t  channels;         // OR of max3010x_channel_t. 0 => RED|IR (SpO2 default)
    uint16_t sample_rate_hz;   // per-channel rate: 50/100/200/400/800 (see .c table)
    uint8_t  led_current_red;  // 0..255, ~0.2 mA/step (datasheet Table 8)
    uint8_t  led_current_ir;
    uint8_t  led_current_green;// used only when GREEN is enabled
    uint8_t  smp_ave;          // on-chip averaging: 1/2/4/8/16/32 (0 or 1 => none).
                               // Output rate = sample_rate_hz / smp_ave — feed THAT
                               // to ppg_set_rate(). See max30102_output_rate_hz().
    int      int_gpio;         // A_FULL/PPG_RDY interrupt line; -1 to poll the FIFO
} max30102_config_t;

// Attach to the shared I2C bus and configure channels / sampling / LED currents.
// cfg == NULL falls back to RED|IR @ 100 Hz, so legacy callers keep working.
esp_err_t max30102_init(i2c_master_bus_handle_t bus, const max30102_config_t *cfg);

// Drain up to `max` samples from the FIFO; sets *out_count to samples read. Every
// enabled channel is unpacked into its field; disabled channels are left 0.
esp_err_t max30102_read_fifo(max30102_sample_t *out, size_t max, size_t *out_count);

// Change the per-channel sample rate at runtime (rewrites SPO2_CONFIG + flushes
// the FIFO). Valid rates: 50/100/200/400/800 Hz (else falls back to 100 Hz). Used
// by the HR/HRV duty-cycle and the Settings "HR sample rate" control.
esp_err_t max30102_set_sample_rate(uint16_t hz);

// Effective FIFO OUTPUT rate = configured sample rate / SMP_AVE. This is the rate
// the consumer actually sees, and the value to pass to ppg_set_rate(). Equal to
// the configured rate when averaging is off.
uint16_t max30102_output_rate_hz(void);

// Number of samples lost to FIFO overflow since the last read_fifo() call (the
// OVF_COUNTER). Non-zero means the drain loop isn't keeping up — surface it to
// diagnose "does the loop actually keep up at 400 Hz?".
uint8_t max30102_last_overflow(void);

// One-shot die-temperature read (blocking, ~30 ms per the datasheet). The red
// LED's wavelength drifts with temperature, which biases SpO2; sampling this
// every few seconds lets the SpO2 stage compensate. Returns ESP_ERR_TIMEOUT if
// the conversion doesn't complete.
esp_err_t max30102_read_die_temp(float *celsius);

// Low-power shutdown between duty-cycle windows (PLAN.md §2.3). Restores the last
// configured mode (SpO2 or Multi-LED) on wake.
esp_err_t max30102_shutdown(bool enable);
