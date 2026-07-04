# `max30102` — MAX3010x PPG / HR / SpO2 / HRV driver

One ESP-IDF driver for the **MAX30102** (RED + IR) and **MAX30101** (RED + IR +
GREEN) optical sensors. They are register-compatible and both answer at I²C
`0x57` with `PART_ID 0x15`, so the driver is told which one you have via
`config.variant`. Full API + rationale are documented in
[`include/max30102.h`](include/max30102.h).

The driver returns **clean raw samples + status**; all filtering / beat
detection / HR-SpO2-HRV math lives in the `ppg` component.

## Capabilities

- 50 Hz … 3200 Hz sampling, 15–18-bit resolution.
- RED / IR / GREEN via SpO2 or Multi-LED mode.
- Every AFE knob: per-channel LED current, ADC full-scale range, LED pulse
  width, on-chip sample averaging.
- 32-sample FIFO handling **with overflow detection** (`read_fifo_ex` reports
  dropped samples so HRV timing is never silently corrupted).
- Low power: software shutdown + hardware **proximity mode** (dark until a
  wrist approaches).
- Interrupt-driven reads (A_FULL / data-ready) with an optional ISR callback.
- Die-temperature readout (SpO2 wavelength compensation — *not* skin temp).
- One-call presets: `hr_lowpower`, `spo2`, `hrv`.

## Quick start

```c
#include "max30102.h"

// 1) Configure. A zero-initialised struct already gives RED+IR SpO2; here we
//    ask for 400 Hz, no averaging — the HRV-fidelity setup.
max30102_config_t cfg;
max30102_default_config(&cfg, MAX3010X_MAX30102);   // or MAX3010X_MAX30101
cfg.sample_rate_hz = 400;
cfg.led_current_red = cfg.led_current_ir = 40;
cfg.int_gpio = -1;                                   // poll (no INT wired here)
ESP_ERROR_CHECK(max30102_init(bus, &cfg));

// 2) Drain the FIFO periodically (faster than the ~80 ms fill @400 Hz).
max30102_sample_t fifo[32];
size_t n; uint32_t dropped;
if (max30102_read_fifo_ex(fifo, 32, &n, &dropped) == ESP_OK) {
    if (dropped) /* flag this window's HRV as suspect */;
    for (size_t i = 0; i < n; i++) {
        // fifo[i].ir / .red / .green  -> feed ppg_process()
    }
}
```

## Common jobs

```c
max30102_preset_hrv();          // 400 Hz, no averaging (RMSSD)
max30102_preset_spo2();         // RED+IR, 411 µs pulse
max30102_preset_hr_lowpower();  // single IR slot + averaging, low current

max30102_shutdown(true);        // sleep between duty-cycle windows (~0.7 µA)

// Wake-on-wrist without the CPU: dark until something reflects enough light.
max30102_config_proximity(/*threshold*/ 0x20, /*pilot_pa*/ 0x1A);

// Green-channel HR on the MAX30101 (Multi-LED mode):
const max30102_slot_t slots[4] = { MAX30102_SLOT_GREEN, MAX30102_SLOT_IR,
                                   MAX30102_SLOT_NONE,  MAX30102_SLOT_NONE };
max30102_set_mode(MAX30102_MODE_MULTI, slots);   // fifo[i].green now populated
```

## Interrupt-driven (lower power) reads

```c
cfg.int_gpio = GPIO_NUM_x;                 // wire the sensor INT here
max30102_init(bus, &cfg);                  // arms A_FULL + installs the ISR
max30102_set_isr_callback(my_cb, sem);     // my_cb gives a semaphore from ISR
// task: wait on sem -> max30102_read_fifo_ex(...) -> light-sleep between.
```

## Notes

- **Averaging destroys HRV.** Keep `sample_avg = MAX30102_AVG_1` for any HRV
  window; averaging is only for low-power HR.
- **MAX30101 green LED mapping** (LED1/2/3 ↔ colour) is assumed RED/IR/GREEN and
  is flagged `TODO(max30101)` in the source — confirm against the datasheet when
  the part arrives. The in-hand MAX30102 has no green channel, so it's inert now.
- Backward compatible: the historical `max30102_*` names and the
  `{ .red, .ir }` sample fields are unchanged; `.green` was added additively.
