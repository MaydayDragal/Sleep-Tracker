# Sleep Tracker — Project Plan

A wrist-worn sleep tracker built on the **Waveshare ESP32-C6-Touch-AMOLED-1.8** development board with an external **MAX30102** pulse-oximetry sensor. The device measures heart rate, SpO2, and movement overnight, scores sleep locally, and presents results on the onboard AMOLED touch display.

- Board wiki: https://docs.waveshare.com/ESP32-C6-Touch-AMOLED-1.8
- Board firmware/examples: https://github.com/waveshareteam/ESP32-C6-Touch-AMOLED-1.8

---

## 1. Goals

**Core (v1):**
- Continuous overnight recording of heart rate (HR), blood-oxygen (SpO2), and wrist movement (actigraphy).
- On-device sleep/wake detection and basic sleep staging (awake / light / deep / REM-estimate) in 30-second epochs.
- Morning summary on the AMOLED display: total sleep time, sleep efficiency, hypnogram, resting HR, min SpO2.
- Data logged to microSD so nights are never lost and can be analyzed offline.

**Stretch (v2+):**
- Smart alarm — wake the user during a light-sleep window before the set alarm time (ES8311 speaker).
- BLE sync to a phone/PC companion app; optional Wi-Fi upload.
- HRV trends (RMSSD), respiratory-rate estimation from the PPG waveform.
- Snore/ambient-noise detection using the onboard microphones.

**Non-goals:** medical-grade accuracy. Wrist SpO2 and consumer sleep staging are estimates; this is a personal-wellness/hobby device.

---

## 2. Hardware Architecture

### 2.1 What the board gives us

| Subsystem | Part | Bus | Role in this project |
|---|---|---|---|
| MCU | ESP32-C6 (RISC-V @160 MHz, Wi-Fi 6, BLE 5, 802.15.4) | — | Everything; BLE for sync |
| Display | 1.8" AMOLED 368×448, SH8601 | QSPI | UI, morning report |
| Touch | FT3168 / FT6146 | I2C | UI input |
| IMU | QMI8658 (6-axis accel + gyro) | I2C | Movement/actigraphy, wake-on-motion, tilt-to-wake |
| RTC | PCF85063 (backup-battery pads) | I2C | Timekeeping across resets, epoch timestamps |
| PMU | AXP2101 | I2C | Battery charging, voltage/fuel readings, power rails |
| Audio | ES8311 codec + mics + speaker | I2S/I2C | Smart alarm; snore detection (stretch) |
| Storage | 16 MB NOR flash + microSD slot | SPI/SDIO | Firmware + night logs |
| Battery | MX1.25 header (3.7 V Li-Po) | — | Untethered overnight use |
| Expansion | 11 solder pads: I2C, UART, USB, 5V, 3V3, GND | — | **MAX30102 attaches here** |

Memory note: ESP32-C6 has **512 KB SRAM and no PSRAM**. A full 368×448 RGB565 frame buffer is ~330 KB, so LVGL must run with a partial draw buffer (e.g. 1/8–1/4 screen). This is a known constraint, not a blocker — Waveshare's own demos do this.

### 2.2 MAX30102 integration

The MAX30102 is an I2C device (address `0x57`) with red + IR LEDs and a photodiode (PPG). It shares the board's I2C bus via the expansion pads:

```
MAX30102 breakout      ESP32-C6-Touch-AMOLED-1.8 pads
  VIN  ─────────────────  3V3
  GND  ─────────────────  GND
  SDA  ─────────────────  I2C SDA pad
  SCL  ─────────────────  I2C SCL pad
  INT  ─────────────────  (optional) spare GPIO — FIFO-ready interrupt
```

I2C address map (no conflicts): ES8311 `0x18`, AXP2101 `0x34`, FT3168 `0x38`, PCF85063 `0x51`, MAX30102 `0x57`, QMI8658 `0x6A/0x6B`.

Wiring the INT line is strongly recommended: the MAX30102 has a 32-sample FIFO, and an interrupt-driven driver lets the CPU stay in light sleep between FIFO drains instead of polling.

**Mechanical:** the sensor must sit flush and snug against the top of the wrist (elastic strap, light pressure, no gap — motion artifact and ambient light leakage are the #1 cause of bad PPG). Enclosure/strap design is tracked as its own work item in Phase 0.

### 2.3 Power budget (drives many decisions)

Rough overnight (8 h) estimate with display off:

| Consumer | Strategy | Est. avg current |
|---|---|---|
| ESP32-C6 | Light sleep between sensor windows, radio off | 0.5–3 mA |
| MAX30102 | Duty-cycled: ~30–60 s of PPG every 5 min, low LED current | 0.2–1 mA |
| QMI8658 | Accel-only low-power mode (~50 Hz), gyro off | <0.1 mA |
| AMOLED | Off during sleep; tilt/touch-to-wake | ~0 |

That lands in the 1–4 mA average range → a 250 mAh cell survives a night with margin, but a **500–1000 mAh Li-Po** is the comfortable choice and is a cheap upgrade via the same MX1.25 header. Continuous (non-duty-cycled) PPG is the main thing that blows the budget; the sampling scheduler in Phase 3 is where we tune this.

---

## 3. Firmware Stack

- **Framework: ESP-IDF 5.x** (not Arduino). We need fine-grained power management (light-sleep with I2C/GPIO wake), the I2S driver for the ES8311, and FreeRTOS task control. Waveshare publishes ESP-IDF examples for this exact board that we can lift drivers from.
- **UI: LVGL 9** with the `esp_lcd` SH8601 QSPI driver and a partial frame buffer.
- **Components** (each its own ESP-IDF component under `components/`):
  - `bsp/` — board support: pins, buses, display, touch, PMU, RTC bring-up (adapted from Waveshare's repo).
  - `max30102/` — FIFO-interrupt driver, LED-current control, shutdown/wake.
  - `ppg/` — signal processing: DC removal, band-pass, beat detection → HR + inter-beat intervals (IBIs); ratio-of-ratios → SpO2; signal-quality index (SQI) to reject motion-corrupted windows.
  - `actigraphy/` — QMI8658 driver config + per-epoch activity counts (band-passed accel magnitude).
  - `sleep_core/` — epoch assembler, storage writer, sleep-scoring algorithm, night session state machine.
  - `ui/` — LVGL screens.
  - `sync/` — BLE GATT service (stretch).

### 3.1 Data flow

```
QMI8658 (50 Hz accel) ──► activity counts ┐
MAX30102 (50–100 Hz PPG) ─► HR, IBIs, SpO2, SQI ┼──► 30 s epoch record ──► ring buffer ──► microSD (CSV/binary)
PCF85063 ────────────────► timestamps      ┘                                 │
                                                                             ▼
                                                              sleep scorer (on epoch close +
                                                              full-night re-pass in the morning)
```

**Epoch record (~32 bytes):** timestamp, activity count, mean/min/max HR, RMSSD, SpO2, SQI, battery %, flags. A full night is ~1000 epochs ≈ 32 KB — trivial for SD, and even the 16 MB flash could hold months as a fallback.

### 3.2 Sleep scoring approach

1. **Sleep/wake per epoch:** actigraphy first — a Cole-Kripke-style weighted moving window over activity counts. This is the well-trodden, robust baseline.
2. **Stage refinement with cardiac data:** within sleep, use HR drop below nightly baseline + low variability → deep; elevated HR variability/irregularity with near-zero movement → REM-estimate; otherwise light. Applied as a full-night smoothing pass in the morning (avoids committing to noisy real-time calls).
3. **SpO2** is reported as min/mean and a "dips below 90%" count — informational, not used for staging.
4. Because raw-ish data lands on the SD card, the algorithm can be re-tuned offline against the recorded nights (and optionally against a reference device) without reflashing mid-project.

---

## 4. Device States & UX

```
        tilt/touch/button                 user taps "Start sleep" (or auto-detect)
IDLE  ────────────────►  WATCH FACE  ───────────────────────────►  TRACKING
(display off,             (time, battery,                          (display off, sensors duty-cycling,
 accel wake armed)         last night's score)                      tilt-to-wake shows minimal clock)
                                ▲                                        │ wake detected / user stops / alarm
                                └──────────  MORNING REPORT  ◄───────────┘
                                             (score, hypnogram, HR/SpO2 charts)
```

Screens for v1: **watch face**, **tracking (minimal clock + "tracking" glyph)**, **morning report** (score card → swipe to hypnogram → swipe to HR/SpO2 graphs), **settings** (alarm time, brightness, start/stop mode).

---

## 5. Milestones

### Phase 0 — Bring-up & skeleton (foundation)
- [ ] Repo scaffolding: ESP-IDF project, CI build (GitHub Actions with `espressif/idf` container), `sdkconfig.defaults`.
- [ ] Port Waveshare BSP: display + touch + LVGL "hello world"; verify PMU, RTC, IMU, SD over I2C/SPI.
- [ ] Wire MAX30102 to the I2C pads; confirm it ACKs at `0x57` alongside the onboard devices.
- [ ] First-pass strap/enclosure so the unit can actually be worn (even if crude).
- **Exit criteria:** board boots into an LVGL screen, all I2C devices enumerate, battery charges and runs untethered.

### Phase 1 — Sensor drivers
- [ ] MAX30102 driver: FIFO + INT, configurable sample rate/LED current, shutdown mode.
- [ ] PPG pipeline: filtering, beat detection → live HR on screen; SpO2 ratio-of-ratios; SQI.
- [ ] QMI8658 in low-power accel mode + activity counts; wake-on-motion interrupt.
- [ ] RTC set/read; battery gauge via AXP2101.
- **Exit criteria:** a live "vitals" screen showing plausible HR (±5 bpm vs finger check), SpO2, and movement level.

### Phase 2 — Recording pipeline
- [ ] Epoch assembler + ring buffer + SD writer (append-only, crash-safe).
- [ ] Night session state machine (manual start/stop first; auto-detect later).
- [ ] Power work: display off, light sleep between sensor windows, duty-cycle scheduler. Measure real overnight battery drain.
- **Exit criteria:** device records a full 8-hour night on battery and the log opens cleanly in a notebook/spreadsheet.

### Phase 3 — Sleep scoring
- [ ] Actigraphy sleep/wake classifier; tune on recorded nights.
- [ ] Morning re-pass: stage refinement from HR/HRV, summary metrics, sleep score.
- [ ] Offline analysis scripts (`tools/`, Python) to visualize logs and iterate on parameters.
- **Exit criteria:** hypnogram + score for a real night that roughly matches subjective experience / a reference tracker.

### Phase 4 — UI polish
- [ ] Watch face, morning report with hypnogram + HR/SpO2 charts, settings, history of last 7 nights.
- [ ] Tilt-to-wake, brightness/AOD handling.

### Phase 5 — Stretch features
- [ ] Smart alarm (light-sleep window + ES8311 speaker tones).
- [ ] BLE GATT sync + simple companion (Web Bluetooth page or Python script) to pull logs.
- [ ] Respiratory rate from PPG; snore detection from mics; Wi-Fi upload/dashboard.

---

## 6. Risks & Open Questions

| Risk | Mitigation |
|---|---|
| Wrist PPG is noisy (motion, contact pressure, ambient light) | SQI gating — discard bad windows rather than log garbage; strap design matters as much as code |
| Wrist SpO2 accuracy is inherently poor | Report trends/min values, label as estimate |
| 512 KB SRAM with a big display + DSP + logging | Partial LVGL buffers; keep PPG processing streaming (no large FFTs needed for v1) |
| 250 mAh battery may be tight for continuous modes | Duty-cycling by default; upgrade cell via same connector |
| Touch controller variant differs by batch (FT3168 vs FT6146) | Probe at runtime; both are FocalTech and near-identical over I2C |
| Which exact GPIOs the expansion I2C pads map to | Confirm from the Rev1.1 schematic in Waveshare's repo during Phase 0 |

Open questions to settle as we implement (defaults chosen so work can start):
1. **Session start:** manual "Start sleep" button for v1; auto-detection later. 
2. **Log format:** binary records + a tiny converter script (CSV is fine too if we prefer eyeballing files).
3. **Strap/enclosure:** 3D-printed case with sensor window vs. sewing the breakout into a strap pocket.

---

## 7. Proposed Repository Layout

```
Sleep-Tracker/
├── PLAN.md                  # this document
├── firmware/                # ESP-IDF project
│   ├── main/
│   ├── components/
│   │   ├── bsp/  max30102/  ppg/  actigraphy/  sleep_core/  ui/  sync/
│   └── sdkconfig.defaults
├── hardware/                # wiring diagrams, strap/enclosure files
├── tools/                   # Python log analysis / algorithm tuning
└── docs/                    # per-subsystem notes as they solidify
```

**Next step:** Phase 0 — scaffold the ESP-IDF project and port the Waveshare BSP so we have pixels on screen and every I2C device answering.
