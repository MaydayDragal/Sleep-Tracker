#pragma once
// =============================================================================
//  MAX3010x optical pulse-oximeter / heart-rate sensor driver  (ESP-IDF, I2C)
//
//  One driver for the whole Analog Devices / Maxim MAX3010x family used by this
//  project:
//    * MAX30102 — RED + IR LEDs, one photodiode.           (the part in hand)
//    * MAX30101 — RED + IR + GREEN LEDs, one photodiode.    (planned upgrade)
//  They are register-compatible; the MAX30101 simply adds a green LED channel
//  (LED3) and exposes it through Multi-LED mode. Both answer at I2C `0x57` and
//  report PART_ID `0x15` — the part ID does NOT distinguish them, so tell the
//  driver which one you have via `max30102_config_t.variant`.
//
//  The historical `max30102_*` prefix is kept for source compatibility with the
//  existing `ppg` / `sd_logger` / app call sites; treat it as "MAX3010x".
//
//  ---------------------------------------------------------------------------
//  What this driver gives you (the sensor's full capability, cleanly):
//    - HR / SpO2 / HRV acquisition at 50 Hz .. 3200 Hz, 15..18-bit resolution.
//    - RED / IR / GREEN channels via SpO2 or Multi-LED mode.
//    - Per-channel LED current, ADC full-scale range, LED pulse width, and
//      on-chip sample averaging — every knob the AFE exposes.
//    - Full 32-sample FIFO handling WITH overflow detection (reports dropped
//      samples instead of silently corrupting HR/HRV timing).
//    - Low-power tools: software shutdown, and hardware **proximity mode** that
//      keeps the sensor dark until a wrist/finger approaches, then wakes.
//    - Interrupt-driven reads (FIFO-almost-full / data-ready) with an optional
//      ISR callback, so the CPU can light-sleep between FIFO drains.
//    - Die-temperature readout (for SpO2 wavelength compensation).
//    - One-call presets: HR-low-power, SpO2, and HRV.
//
//  Design split: this driver returns *clean raw samples* + status/metadata. All
//  filtering, beat detection, HR/SpO2/HRV math lives in the `ppg` component.
//
//  Datasheets: datasheets/ (MAX30102, MAX30101). Register addresses/bitfields
//  below follow those datasheets. See PLAN.md §2.2 and §3.3.
// =============================================================================

#include "esp_err.h"
#include "driver/i2c_master.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// -----------------------------------------------------------------------------
//  Enumerations
// -----------------------------------------------------------------------------

/// Which physical part is attached. Selects whether the GREEN LED (LED3) exists.
typedef enum {
    MAX3010X_MAX30102 = 0,   ///< default: RED + IR (no green)
    MAX3010X_MAX30101 = 1,   ///< RED + IR + GREEN
} max30102_variant_t;

/// Acquisition mode. Values are the raw MODE_CONFIG[2:0] codes.
/// `0` (unset) is treated as SPO2 so zero-initialised configs "just work".
typedef enum {
    MAX30102_MODE_HR    = 0x02,  ///< 1 channel (single LED per datasheet). Prefer MULTI for explicit LED choice.
    MAX30102_MODE_SPO2  = 0x03,  ///< 2 channels: RED then IR
    MAX30102_MODE_MULTI = 0x07,  ///< up to 4 configurable time slots (only way to use GREEN)
} max30102_mode_t;

/// Multi-LED time-slot assignment (which LED fires in each of up to 4 slots).
typedef enum {
    MAX30102_SLOT_NONE  = 0,     ///< slot disabled
    MAX30102_SLOT_RED   = 1,     ///< LED1 (red)
    MAX30102_SLOT_IR    = 2,     ///< LED2 (IR)
    MAX30102_SLOT_GREEN = 3,     ///< LED3 (green) — MAX30101 only
} max30102_slot_t;

/// ADC full-scale range (SPO2_CONFIG ADC_RGE). `_DEFAULT` maps to 4096 nA.
typedef enum {
    MAX30102_ADC_DEFAULT = 0,    ///< -> 4096 nA
    MAX30102_ADC_2048NA  = 1,    ///< LSB 7.81 pA
    MAX30102_ADC_4096NA  = 2,    ///< LSB 15.63 pA
    MAX30102_ADC_8192NA  = 3,    ///< LSB 31.25 pA
    MAX30102_ADC_16384NA = 4,    ///< LSB 62.5 pA
} max30102_adc_range_t;

/// LED pulse width / ADC resolution (SPO2_CONFIG LED_PW). Wider = more light
/// captured (better SNR) but a longer slot, so it caps the max sample rate.
typedef enum {
    MAX30102_PW_AUTO        = 0, ///< pick the widest pulse that fits rate × channels
    MAX30102_PW_69US_15BIT  = 1,
    MAX30102_PW_118US_16BIT = 2,
    MAX30102_PW_215US_17BIT = 3,
    MAX30102_PW_411US_18BIT = 4,
} max30102_pulse_width_t;

/// On-chip sample averaging (FIFO_CONFIG SMP_AVE). AVERAGING SMOOTHS HR BUT
/// DESTROYS HRV BEAT TIMING — keep it at 1 for any HRV window.
typedef enum {
    MAX30102_AVG_1  = 0,         ///< no averaging (required for HRV)
    MAX30102_AVG_2  = 1,
    MAX30102_AVG_4  = 2,
    MAX30102_AVG_8  = 3,
    MAX30102_AVG_16 = 4,
    MAX30102_AVG_32 = 5,
} max30102_sample_avg_t;

// -----------------------------------------------------------------------------
//  Data
// -----------------------------------------------------------------------------

/// One FIFO sample. Each channel is an 18-bit ADC count (0..262143). `green` is
/// 0 unless a GREEN slot is active (MAX30101 + Multi-LED). Channels not part of
/// the current mode read 0.
typedef struct {
    uint32_t red;    ///< RED channel  (SpO2)
    uint32_t ir;     ///< IR channel   (HR / HRV / SpO2)
    uint32_t green;  ///< GREEN channel (HR at the wrist; MAX30101 only)
} max30102_sample_t;

/// Interrupt-source flags (mirror of INT_STATUS_1/2; usable as a bit mask for
/// max30102_config_interrupts() and returned by max30102_read_interrupt_status()).
typedef enum {
    MAX30102_INT_A_FULL       = 1u << 7, ///< FIFO reached the almost-full level
    MAX30102_INT_DATA_RDY     = 1u << 6, ///< a new sample is ready (PPG_RDY)
    MAX30102_INT_ALC_OVF      = 1u << 5, ///< ambient-light cancellation overflow
    MAX30102_INT_PROXIMITY    = 1u << 4, ///< proximity threshold crossed (wake)
    MAX30102_INT_PWR_RDY      = 1u << 0, ///< power-ready (after brown-out/boot)
    MAX30102_INT_DIE_TEMP_RDY = 1u << 8, ///< die-temp conversion done (INT_STATUS_2)
} max30102_int_flag_t;

/// Full configuration. Every field past the first four is optional: a
/// zero-initialised struct yields a sane RED+IR SpO2 setup (matching the
/// project's historical defaults), so existing `{ .sample_rate_hz = ... }`
/// initialisers keep working. Use max30102_default_config() for a good baseline.
typedef struct {
    // --- core (kept for backward compatibility) ---
    uint16_t sample_rate_hz;        ///< 50/100/200/400/800/1000/1600/3200 (0 -> 100)
    uint8_t  led_current_red;       ///< 0..255, ~0.2 mA/step (0 -> ~7 mA default)
    uint8_t  led_current_ir;        ///< 0..255 (0 -> ~7 mA default)
    int      int_gpio;              ///< INT line GPIO, or -1 to poll

    // --- extended (all optional; 0 == default) ---
    max30102_variant_t     variant;          ///< MAX30102 (default) or MAX30101
    max30102_mode_t        mode;             ///< 0 -> SPO2
    uint8_t                led_current_green;///< MAX30101 green LED current
    max30102_adc_range_t   adc_range;        ///< 0 -> 4096 nA
    max30102_pulse_width_t pulse_width;      ///< 0 -> AUTO (widest that fits)
    max30102_sample_avg_t  sample_avg;       ///< 0 -> no averaging (HRV-safe)
    uint8_t                int_a_full_filled;///< samples buffered when A_FULL fires (17..32; 0 -> 24)
    max30102_slot_t        slots[4];         ///< MULTI mode slot order; ignored otherwise
} max30102_config_t;

// -----------------------------------------------------------------------------
//  Lifecycle
// -----------------------------------------------------------------------------

/// Fill `cfg` with a good baseline for `variant` (SpO2, 100 Hz, no averaging,
/// 4096 nA, auto pulse width, moderate LED current, polled). Start here, then
/// override what you need.
void max30102_default_config(max30102_config_t *cfg, max30102_variant_t variant);

/// Attach to the shared I2C bus, verify the part, soft-reset, and apply `cfg`.
/// Returns ESP_ERR_NOT_FOUND if no MAX3010x answers at 0x57.
esp_err_t max30102_init(i2c_master_bus_handle_t bus, const max30102_config_t *cfg);

/// Detach from the bus and (if installed) remove the INT ISR. Safe to re-init after.
esp_err_t max30102_deinit(void);

/// Software power-on reset (RESET bit). All registers return to POR defaults;
/// re-apply configuration afterwards (max30102_init does this for you).
esp_err_t max30102_reset(void);

/// Read PART_ID (expect 0x15) and REV_ID. Either pointer may be NULL. Doubles
/// as a presence / self-test check.
esp_err_t max30102_read_id(uint8_t *part_id, uint8_t *rev_id);

// -----------------------------------------------------------------------------
//  Data acquisition
// -----------------------------------------------------------------------------

/// Drain up to `max` samples from the FIFO into `out`; sets *out_count.
/// Backward-compatible entry point (overflow count discarded). Prefer *_ex.
esp_err_t max30102_read_fifo(max30102_sample_t *out, size_t max, size_t *out_count);

/// Like max30102_read_fifo(), but also reports how many samples the hardware
/// FIFO overflowed/dropped since the last read (from OVF_COUNTER). A non-zero
/// `*dropped` means a timing discontinuity — callers should treat the following
/// window's HRV as suspect. Correctly disambiguates the "write==read pointer"
/// full-vs-empty case using the overflow counter.
esp_err_t max30102_read_fifo_ex(max30102_sample_t *out, size_t max,
                                size_t *out_count, uint32_t *dropped);

/// Number of unread samples currently in the FIFO (0..32).
esp_err_t max30102_samples_available(size_t *count);

/// Reset the FIFO write/read/overflow pointers (discard buffered samples).
esp_err_t max30102_flush_fifo(void);

// -----------------------------------------------------------------------------
//  Runtime configuration
// -----------------------------------------------------------------------------

/// Change the sample rate at runtime; re-derives the pulse width when AUTO and
/// flushes the FIFO (so the consumer never mixes rates). Valid: 50/100/200/400/
/// 800/1000/1600/3200 Hz; other values fall back to 100 Hz.
esp_err_t max30102_set_sample_rate(uint16_t hz);

/// Set LED drive currents (0..255, ~0.2 mA/step). Pass 0xFFFF-style "keep"? No —
/// each value is written as-is; green is ignored on the MAX30102.
esp_err_t max30102_set_led_current(uint8_t red, uint8_t ir, uint8_t green);

/// Set the ADC full-scale range.
esp_err_t max30102_set_adc_range(max30102_adc_range_t range);

/// Set the LED pulse width / ADC resolution (AUTO picks the widest that fits).
esp_err_t max30102_set_pulse_width(max30102_pulse_width_t pw);

/// Set on-chip sample averaging (keep at MAX30102_AVG_1 while collecting HRV).
esp_err_t max30102_set_sample_averaging(max30102_sample_avg_t avg);

/// Switch acquisition mode. For MULTI, pass the 4 slot assignments (unused slots
/// = MAX30102_SLOT_NONE); for HR/SPO2 pass NULL.
esp_err_t max30102_set_mode(max30102_mode_t mode, const max30102_slot_t slots[4]);

// -----------------------------------------------------------------------------
//  Power
// -----------------------------------------------------------------------------

/// Enter (true) or leave (false) software shutdown. In shutdown the part keeps
/// its registers but stops converting and drops to ~0.7 µA — use it between
/// duty-cycle windows.
esp_err_t max30102_shutdown(bool enable);

/// Configure hardware proximity mode: the sensor pulses only the pilot LED at
/// `pilot_pa` current and stays quiet until reflected light exceeds `threshold`
/// (0..255), at which point it raises MAX30102_INT_PROXIMITY and begins normal
/// PPG. Lets the device idle at microamps until worn. Set threshold 0 to disable.
esp_err_t max30102_config_proximity(uint8_t threshold, uint8_t pilot_pa);

// -----------------------------------------------------------------------------
//  Die temperature (for SpO2 wavelength compensation — NOT skin temperature)
// -----------------------------------------------------------------------------

/// Trigger a single die-temperature conversion and return it in °C (±1 °C,
/// 0.0625 °C resolution). Blocks up to ~30 ms for the conversion.
esp_err_t max30102_read_temperature(float *celsius);

// -----------------------------------------------------------------------------
//  Interrupts
// -----------------------------------------------------------------------------

/// Enable the given interrupt sources (bitwise-OR of max30102_int_flag_t). Only
/// A_FULL, DATA_RDY, ALC_OVF, PROXIMITY (INT_ENABLE_1) and DIE_TEMP_RDY
/// (INT_ENABLE_2) are maskable; others are ignored.
esp_err_t max30102_config_interrupts(uint32_t flags);

/// Read and clear the interrupt status (INT_STATUS_1/2 are clear-on-read).
/// Returns the OR of the max30102_int_flag_t bits that were asserted.
esp_err_t max30102_read_interrupt_status(uint32_t *flags);

/// ISR-safe callback invoked from the INT-line GPIO ISR when `int_gpio >= 0`.
/// Keep it minimal (e.g. give a semaphore / notify a task). `arg` is passed back.
typedef void (*max30102_isr_cb_t)(void *arg);

/// Register (or clear, with cb=NULL) the data-ready ISR callback. The GPIO ISR
/// is installed by max30102_init() when int_gpio >= 0.
esp_err_t max30102_set_isr_callback(max30102_isr_cb_t cb, void *arg);

// -----------------------------------------------------------------------------
//  One-call presets (reconfigure a running sensor for a common job)
// -----------------------------------------------------------------------------

/// Low-power HR: single IR (or green on MAX30101) slot, on-chip averaging, a low
/// sample rate and modest LED current — cheapest continuous heart rate.
esp_err_t max30102_preset_hr_lowpower(void);

/// SpO2: RED + IR, 100 Hz, no averaging, 411 µs pulse for good red/IR SNR.
esp_err_t max30102_preset_spo2(void);

/// HRV: RED + IR at 400 Hz, NO averaging, 118 µs pulse — maximum beat-timing
/// fidelity for RMSSD (PLAN.md §3.3).
esp_err_t max30102_preset_hrv(void);

// -----------------------------------------------------------------------------
//  Low-level register access (for power users / debugging)
// -----------------------------------------------------------------------------

esp_err_t max30102_read_reg(uint8_t reg, uint8_t *val);
esp_err_t max30102_write_reg(uint8_t reg, uint8_t val);

/// Detected variant / active channel count (valid after init). `channels` is how
/// many 3-byte channels each FIFO sample carries (1..3). Either pointer may be NULL.
esp_err_t max30102_get_info(max30102_variant_t *variant, uint8_t *channels);

#ifdef __cplusplus
}
#endif
