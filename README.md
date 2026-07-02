# Sleep-Tracker

A wrist-worn sleep tracker built on the [Waveshare ESP32-S3-Touch-AMOLED-1.8](https://docs.waveshare.com/ESP32-S3-Touch-AMOLED-1.8) with an external MAX3010x pulse-oximetry sensor (MAX30102 for initial bring-up, transitioning to the MAX30101). It records heart rate, SpO2, and movement overnight (plus HRV where the signal and power budget allow), scores sleep on-device, and shows a morning report on the AMOLED touch display. (The pin-compatible ESP32-C6 variant is a documented power-optimization fallback — see PLAN.md.)

**Start here:** read [PLAN.md](PLAN.md) for the overview and staged roadmap, then do the Phase 0 board bring-up ([firmware/README.md](firmware/README.md)), then run the [VALIDATION.md](VALIDATION.md) de-risking spikes (power, SD throughput, sensor coupling). Spike **S1** checks whether the *optional* HRV feature is worth pursuing — it doesn't gate anything else.

## Documentation map (reading order)

| Doc | What it covers | When you need it |
|---|---|---|
| **[PLAN.md](PLAN.md)** | Full plan: hardware, firmware stack, sleep-scoring, power budget, and the phased milestones (§5) | Read first — the context for everything else |
| **[VALIDATION.md](VALIDATION.md)** | De-risking spikes (power, SD, coupling) + the optional Polar H10 HRV-validation protocol | The first hands-on work once Phase 0 bring-up is done |
| **[`firmware/`](firmware/)** | ESP-IDF v5.x scaffold (ESP32-S3) — [build instructions](firmware/README.md) | When you start writing/flashing code |
| **[FEATURES.md](FEATURES.md)** | Prioritized feature list (P0 core → P2 stretch → HW add-ons) | Reference — the backlog |
| **[INTEGRATION.md](INTEGRATION.md)** | Long-term Home Assistant + CPAP integration design | Long-term reference — not needed for Phases 0–4 |
| **[datasheets/](datasheets/)** | Datasheets for the board's main ICs + the project sensors, with the I²C bus map | Hardware reference — bring-up, wiring, driver work |

## Development stages at a glance

The project is built in staged milestones (full detail + exit criteria in [PLAN.md §5](PLAN.md#5-milestones)):

- **Phase 0 — Board bring-up:** boots into an LVGL screen, all I2C devices enumerate, runs untethered. *(Working on hardware — display + LVGL + touch + I2C enumeration all up via the managed Waveshare BSP. See [firmware/README.md](firmware/README.md).)*
- *→ De-risk checks:* run VALIDATION spikes (power, SD, coupling); **S1** tells you whether the *optional* HRV feature is feasible — it doesn't gate the build.
- **Phase 1 — Sensor drivers:** live HR / SpO2 / movement on screen (HRV optional, power-permitting).
- **Phase 2 — Recording pipeline:** records a full 8-hour night to microSD on battery.
- **Phase 2.5 — Body-sensor network:** a paired torso WT9011DCL logs authoritative sleep position all night.
- **Phase 3 — Sleep scoring:** hypnogram + score, position-resolved summaries (optional HRV check against ECG).
- **Phase 4 — UI polish:** watch face, morning report, settings, 7-night history.
- **Phase 5 — Stretch:** smart alarm, BLE/companion sync, respiratory rate, snore detection.
- **v3+ — Integration:** Home Assistant + CPAP combined summary (separate track, see [INTEGRATION.md](INTEGRATION.md) §7).
