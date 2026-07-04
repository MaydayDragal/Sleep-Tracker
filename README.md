# Sleep-Tracker

A wrist-worn sleep tracker built on the [Waveshare ESP32-S3-Touch-AMOLED-1.8](https://docs.waveshare.com/ESP32-S3-Touch-AMOLED-1.8) with an external MAX3010x pulse-oximetry sensor (MAX30102 for initial bring-up, transitioning to the MAX30101). It records heart rate, SpO2, and movement overnight to microSD (plus a live HRV estimate where signal and power budget allow), and is designed to score sleep on-device and show a morning report on the AMOLED touch display. Today the scorer runs as an offline prototype in [`tools/`](tools/); the on-device port and the live report are Phase 3/4 work (the report tiles currently show a representative sample night). (The pin-compatible ESP32-C6 variant is a documented power-optimization fallback — see PLAN.md.)

**Start here:** read [PLAN.md](PLAN.md) for the overview and staged roadmap, then do the Phase 0 board bring-up ([firmware/README.md](firmware/README.md)), then run the [VALIDATION.md](VALIDATION.md) de-risking spikes (power, SD throughput, sensor coupling).

## Documentation map (reading order)

| Doc | What it covers | When you need it |
|---|---|---|
| **[HANDOFF.md](HANDOFF.md)** | Current state, next steps, and gotchas — the resume point | Start of a new session |
| **[PLAN.md](PLAN.md)** | Full plan: hardware, firmware stack, sleep-scoring, power budget, and the phased milestones (§5) | Read first — the context for everything else |
| **[VALIDATION.md](VALIDATION.md)** | De-risking spikes (power, SD, coupling) + the optional Polar H10 HRV-validation protocol | The first hands-on work once Phase 0 bring-up is done |
| **[`firmware/`](firmware/)** | ESP-IDF firmware (ESP32-S3) — builds on native v6.0.2 and PlatformIO 5.4.1; [build instructions](firmware/README.md) | When you start writing/flashing code |
| **[FEATURES.md](FEATURES.md)** | Prioritized feature list (P0 core → P2 stretch → HW add-ons) | Reference — the backlog |
| **[INTEGRATION.md](INTEGRATION.md)** | Long-term Home Assistant + CPAP integration design | Long-term reference — not needed for Phases 0–4 |
| **[datasheets/](datasheets/)** | Datasheets for the board's main ICs + the project sensors, with the I²C bus map | Hardware reference — bring-up, wiring, driver work |
| **[docs/](docs/)** | Design references — the interactive [watch-UI mockup](docs/watch-ui-mockup.html) (all screens at the real 368×448) | UI / design reference |

## Development stages at a glance

The project is built in staged milestones (full detail + exit criteria in [PLAN.md §5](PLAN.md#5-milestones)):

- **Phase 0 — Board bring-up:** boots into an LVGL screen, all I2C devices enumerate, runs untethered. *(Done and verified on hardware, merged to main — display + LVGL + touch + I2C enumeration all up via the managed Waveshare BSP. See [firmware/README.md](firmware/README.md).)*
- *→ De-risk checks:* run VALIDATION spikes (power, SD, coupling). **S1** validates the fuller HRV path against a reference ECG — a basic live RMSSD already ships (power-permitting), so S1 doesn't gate the build.
- **Phase 1 — Sensor drivers:** live HR / SpO2 / movement on screen (HRV optional, power-permitting). *(Done and verified on hardware — RTC, IMU, battery, and the external MAX30102 PPG all read live; HR / SpO2 from the PPG, movement from the IMU.)*
- **Phase 2 — Recording pipeline:** records a full 8-hour night to microSD on battery. *(Pipeline built and bench-validated over USB on hardware — SD mounts, 30 s epochs write to the card, tickless light-sleep engages; the full 8 h-on-battery run is the remaining exit gate, blocked on attaching a Li-Po — the AXP2101 gauge reads 0%.)*
- **Phase 2.5 — Body-sensor network:** a paired torso WT9011DCL logs authoritative sleep position all night. *(Not started — `bodynet` is a stub; the body-position/movement CSV columns are present but always 0.)*
- **Phase 3 — Sleep scoring:** hypnogram + score, position-resolved summaries (optional HRV check against ECG). *(Offline prototype only — the full pipeline runs in `tools/score_night.py`, validated on synthetic nights; on-device scoring in `sleep_core` is still a `TODO(phase3)`.)*
- **Phase 4 — UI polish:** watch face, morning report, settings, 7-night history. *(Watch UI shell built & running on hardware — an 11-tile swipeable LVGL app with live watch/vitals/tracking + a PPG-debug tile, display-sleep + double-tap-to-wake; report/history tiles show a sample night pending on-device scoring. Interactive design mockup: [docs/watch-ui-mockup.html](docs/watch-ui-mockup.html).)*
- **Phase 5 — Stretch:** smart alarm, BLE/companion sync, respiratory rate, snore detection.
- **v3+ — Integration:** Home Assistant + CPAP combined summary (separate track, see [INTEGRATION.md](INTEGRATION.md) §7).
