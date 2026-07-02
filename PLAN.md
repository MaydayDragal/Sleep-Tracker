# Sleep Tracker — Project Plan

A wrist-worn sleep tracker built on the **Waveshare ESP32-S3-Touch-AMOLED-1.8** development board with an external **MAX30102** pulse-oximetry sensor. The device measures heart rate, SpO2, and movement overnight, scores sleep locally, and presents results on the onboard AMOLED touch display.

- Board wiki: https://www.waveshare.com/wiki/ESP32-S3-Touch-AMOLED-1.8
- Board docs: https://docs.waveshare.com/ESP32-S3-Touch-AMOLED-1.8
- Board firmware/examples: https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.8

> **Board choice (decided):** targeting the **ESP32-S3** variant to prioritize sample rate and full-fidelity data logging — its 8 MB PSRAM and dual-core CPU let us buffer and store raw overnight waveforms, not just epoch summaries. The pin-compatible **ESP32-C6** sibling (same display/touch/IMU/RTC/PMU/audio, but 512 KB SRAM, no PSRAM, lower power draw) is kept as a **fallback if overnight power draw proves too high**. The firmware BSP is structured to make that swap cheap (see §3).

---

## 1. Goals

**Core (v1):**
- Continuous overnight recording of heart rate (HR), blood-oxygen (SpO2), and wrist movement (actigraphy).
- **Reliable HRV measurement** — beat-to-beat interval (IBI) capture accurate enough for trustworthy RMSSD/SDNN, validated against an ECG chest strap. This is a first-class goal and drives the PPG sampling design (see §3.3).
- On-device sleep/wake detection and basic sleep staging (awake / light / deep / REM-estimate) in 30-second epochs.
- Morning summary on the AMOLED display: total sleep time, sleep efficiency, hypnogram, resting HR, min SpO2.
- Data logged to microSD so nights are never lost and can be analyzed offline.

**Stretch (v2+):**
- Smart alarm — wake the user during a light-sleep window before the set alarm time (ES8311 speaker).
- BLE sync to a phone/PC companion app; optional Wi-Fi upload.
- Frequency-domain HRV (LF/HF), respiratory-rate estimation from the PPG waveform.
- Snore/ambient-noise detection using the onboard microphones.

**Long-term (v3+): Home Assistant + CPAP integration.**
- Publish nightly sleep/HR/HRV/SpO2 data to Home Assistant over MQTT (with auto-discovery), and pull the user's CPAP therapy data (AHI, mask leak, pressure, usage) back so the wrist shows a **combined night summary — device metrics plus CPAP data — on the AMOLED**.
- Unlocks cross-device insights neither device can produce alone (e.g. positional-apnea detection from wrist IMU + CPAP events). Full design in **[INTEGRATION.md](INTEGRATION.md)**.

**Non-goals:** medical-grade accuracy. Wrist SpO2 and consumer sleep staging are estimates; this is a personal-wellness/hobby device. (CPAP data is surfaced for personal insight/automation, not clinical diagnosis.)

---

## 2. Hardware Architecture

### 2.1 What the board gives us

| Subsystem | Part | Bus | Role in this project |
|---|---|---|---|
| MCU | ESP32-S3R8 (dual-core LX7 @240 MHz, Wi-Fi 4, BLE 5, **8 MB PSRAM**, 16 MB flash, native USB) | — | Everything; a core for sensing + a core for UI/logging; BLE/Wi-Fi for sync |
| Display | 1.8" AMOLED 368×448, SH8601 | QSPI | UI, morning report |
| Touch | FT3168 / FT6146 | I2C | UI input |
| IMU | QMI8658 (6-axis accel + gyro) | I2C | Movement/actigraphy, wake-on-motion, tilt-to-wake |
| RTC | PCF85063 (backup-battery pads) | I2C | Timekeeping across resets, epoch timestamps |
| PMU | AXP2101 | I2C | Battery charging, voltage/fuel readings, power rails |
| Audio | ES8311 codec + mics + speaker | I2S/I2C | Smart alarm; snore detection (stretch) |
| Storage | 16 MB NOR flash + microSD slot | SPI/SDIO | Firmware + night logs |
| Battery | MX1.25 header (3.7 V Li-Po) | — | Untethered overnight use |
| Expansion | 11 solder pads: I2C, UART, USB, 5V, 3V3, GND | — | **MAX30102 attaches here** |

Memory note: the ESP32-S3R8 has **512 KB internal SRAM plus 8 MB PSRAM**. That headroom is why we chose it — a full 368×448 RGB565 frame buffer (~330 KB) fits comfortably *and* we can hold hours of raw PPG + accel samples in RAM before flushing to SD, making full-waveform overnight logging the default rather than a compromise. (On the C6 fallback there is no PSRAM: LVGL would need partial draw buffers and logging would drop to epoch summaries — the reason the S3 is the primary target.)

### 2.2 MAX30102 integration

The MAX30102 is an I2C device (address `0x57`) with red + IR LEDs and a photodiode (PPG). It shares the board's I2C bus via the expansion pads:

```
MAX30102 breakout      ESP32-S3-Touch-AMOLED-1.8 pads
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
| ESP32-S3 | Light sleep between sensor windows, radio off; PSRAM self-refresh in sleep | 2–8 mA |
| MAX30102 | Duty-cycled: ~30–60 s of PPG every 5 min, low LED current | 0.2–1 mA |
| QMI8658 | Accel-only low-power mode (~50 Hz), gyro off | <0.1 mA |
| AMOLED | Off during sleep; tilt/touch-to-wake | ~0 |

That lands roughly in the 3–10 mA average range. The S3 is thirstier than the C6 (dual-core, PSRAM self-refresh in sleep is the main adder), so a **500–1000 mAh Li-Po** is the recommended cell rather than the stock 250 mAh — both fit the same MX1.25 header. Continuous (non-duty-cycled) PPG and the nightly HRV capture windows are the biggest draws; the sampling scheduler (Phase 2) is where we trade fidelity against battery. **If overnight drain proves unacceptable even with the bigger cell, the C6 fallback is the lever** — same firmware, lower-power SoC, at the cost of raw-waveform logging.

---

## 3. Firmware Stack

- **Framework: ESP-IDF 5.x** (not Arduino). We need fine-grained power management (light-sleep with I2C/GPIO wake), the I2S driver for the ES8311, native USB (mass-storage offload / CDC streaming), and FreeRTOS dual-core task control. Waveshare publishes ESP-IDF examples for this exact board that we can lift drivers from.
- **Threading:** pin sensor acquisition (MAX30102 + QMI8658) to one core and UI/SD-logging to the other, so an LVGL redraw or a slow SD flush never drops samples — a concrete payoff of the dual-core S3.
- **UI: LVGL 9** with the `esp_lcd` SH8601 QSPI driver; full frame buffer in PSRAM (partial buffers only on the C6 fallback).
- **Components** (each its own ESP-IDF component under `components/`):
  - `bsp/` — board support: pins, buses, display, touch, PMU, RTC bring-up (adapted from Waveshare's repo). **All board-specific pin/peripheral defs live here** so an S3→C6 swap is confined to this one component.
  - `max30102/` — FIFO-interrupt driver, LED-current control, shutdown/wake.
  - `ppg/` — signal processing: DC removal, band-pass, beat detection with sub-sample peak refinement → HR + inter-beat intervals (IBIs); IBI artifact/ectopic correction; ratio-of-ratios → SpO2; signal-quality index (SQI) to reject motion-corrupted windows.
  - `actigraphy/` — QMI8658 driver config + per-epoch activity counts (band-passed accel magnitude).
  - `sleep_core/` — epoch assembler, storage writer, sleep-scoring algorithm, night session state machine.
  - `ui/` — LVGL screens.
  - `sync/` — BLE GATT service (stretch).

### 3.1 Data flow

```
QMI8658 (50 Hz accel) ──► activity counts ┐
MAX30102 (200 Hz PPG) ───► HR, IBIs, SpO2, SQI ┼──► 30 s epoch record ──► ring buffer ──► microSD (CS
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

### 3.3 Reliable HRV — requirements

HRV is the most timing-sensitive thing this device measures. RMSSD is computed from the *differences* between consecutive beat intervals, so it is dominated by small errors in each beat's timestamp — errors that HR (a smoothed average over many beats) completely hides. Getting it right constrains several design choices:

1. **Timing resolution beats raw sample rate.** RMSSD in relaxed sleep is often 20–60 ms. To resolve that we need per-beat timing error well under ~5 ms. A 200 Hz PPG stream gives only 5 ms sample spacing, so we **must** do sub-sample peak interpolation (parabolic/quadratic fit around the detected peak, or cross-correlation against a beat template) to recover sub-millisecond timing between samples. Sample at **200 Hz minimum, 400 Hz preferred** (MAX30102 supports it) — higher rate makes interpolation more accurate and cheaper.
2. **Fiducial-point consistency.** Pick one repeatable feature per beat and use it for every beat — the systolic-upstroke maximum of the first derivative is more stable against pulse-shape changes than the raw waveform peak. Consistency matters more than which point.
3. **Beat classification is mandatory, not optional.** Missed/extra beats and motion artifacts create huge fake IBI jumps that wreck RMSSD. The pipeline must classify each IBI (normal / ectopic / artifact) and either interpolate or exclude bad beats before any HRV math. Report the **percentage of accepted beats** alongside every HRV value — an RMSSD from a 40%-accepted window is not trustworthy and must be flagged.
4. **HRV only from clean, still windows.** Wrist PPG HRV is only credible during motionless, high-SQI stretches. Gate HRV on: activity count near zero (IMU), SQI above threshold, and a minimum count of consecutive accepted beats (e.g. ≥2 min / ~120 beats for a stable RMSSD). Prefer quality over coverage — report HRV for the windows that qualify and leave gaps elsewhere rather than emitting noise.
5. **Store the IBI series, not just the summary.** Log the full accepted-IBI list (or the raw PPG) to SD so RMSSD/SDNN/pNN50 and frequency-domain metrics (LF/HF, needs ~5 min windows) can be recomputed and re-tuned offline. The S3's PSRAM makes keeping the raw stream the default.
6. **Duty-cycling vs. HRV is a real tension.** Short 30–60 s PPG bursts save power but may not contain a long enough clean run for a stable RMSSD. Resolution: run a **nightly HRV mode** that captures longer continuous windows during deep/stable sleep (e.g. a 5-min clean capture per sleep cycle) rather than relying on the short duty-cycle bursts. The sampling scheduler (Phase 2) exposes this as a configurable trade-off.
7. **Validate against ground truth.** HRV is the one metric where "looks plausible" isn't enough. Validate the IBI series against an ECG chest strap (e.g. Polar H10, which exposes RR intervals over BLE) on the same wrist/session, and require Bland-Altman agreement on RMSSD before declaring HRV "reliable." This is a Phase 3 exit criterion.

**Reported HRV metrics:** RMSSD (primary, robust for overnight), SDNN, pNN50, mean IBI, and per-value beat-acceptance % + coverage. Frequency-domain LF/HF is a P2 stretch (needs the longer clean windows from item 6).

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
- [ ] Beat detection with sub-sample peak interpolation → per-beat IBI series with timing error < 5 ms; IBI artifact/ectopic classifier + beat-acceptance %.
- **Exit criteria:** a live "vitals" screen showing plausible HR (±5 bpm vs finger check), SpO2, movement level, and a live RMSSD that tracks a reference during a still 2-min window.

### Phase 2 — Recording pipeline
- [ ] Epoch assembler + ring buffer + SD writer (append-only, crash-safe).
- [ ] Night session state machine (manual start/stop first; auto-detect later).
- [ ] Power work: display off, light sleep between sensor windows, duty-cycle scheduler. Measure real overnight battery drain.
- [ ] Nightly HRV mode: capture longer continuous clean windows (~5 min/sleep cycle) for stable RMSSD instead of relying on short duty-cycle bursts. Log the full accepted-IBI series to SD.
- **Exit criteria:** device records a full 8-hour night on battery, the log opens cleanly in a notebook/spreadsheet, and the IBI series is complete enough to compute per-cycle RMSSD.

### Phase 3 — Sleep scoring
- [ ] Actigraphy sleep/wake classifier; tune on recorded nights.
- [ ] Morning re-pass: stage refinement from HR/HRV, summary metrics, sleep score.
- [ ] Offline analysis scripts (`tools/`, Python) to visualize logs and iterate on parameters.
- [ ] **HRV validation against ECG ground truth** — record simultaneous sessions vs. a Polar H10 (or equivalent RR-interval reference), compare RMSSD/SDNN with Bland-Altman analysis, and tune the beat detector/artifact filter until agreement is acceptable. **Full protocol in [VALIDATION.md](VALIDATION.md).**
- **Exit criteria:** hypnogram + score for a real night that roughly matches subjective experience / a reference tracker, **and** overnight RMSSD agrees with the ECG reference within a documented tolerance on clean windows.

### Phase 4 — UI polish
- [ ] Watch face, morning report with hypnogram + HR/SpO2 charts, settings, history of last 7 nights.
- [ ] Tilt-to-wake, brightness/AOD handling.

### Phase 5 — Stretch features
- [ ] Smart alarm (light-sleep window + ES8311 speaker tones).
- [ ] BLE GATT sync + simple companion (Web Bluetooth page or Python script) to pull logs.
- [ ] Respiratory rate from PPG; snore detection from mics; Wi-Fi upload/dashboard.

### 5.1 Optimizations & techniques to bank

Engineering techniques to apply as the relevant components are built — most improve *both* data quality and power, which is why they're worth designing in early rather than bolting on:

- **Motion-gated PPG (cross-sensor).** Use the IMU to blank/flag PPG during movement. Improves HRV quality (drop corrupted beats at the source) *and* saves power (skip processing corrupted windows). The single highest-leverage optimization — it ties the two core sensors together.
- **Adaptive LED current / AGC on the MAX30102.** Auto-tune LED drive to keep the PPG in the ADC's sweet spot across skin tone and contact quality. Better SQI than a fixed current, and less power than running the LEDs hot "to be safe."
- **Template-matching beat detection.** Cross-correlate each pulse against a per-user beat template for a stable fiducial point — more robust timing than a raw peak, which directly helps RMSSD (VALIDATION.md §2).
- **Double-buffered DMA SD writes from PSRAM.** Ping-pong ring buffers so sample acquisition never blocks on an SD flush (de-risked by spike S3).
- **On-the-fly raw compression** (delta + RLE / simple predictive) to shrink the raw-PPG log without losing fidelity, if S3 shows SD volume/throughput is tight.
- **Offload motion to the QMI8658.** Use its hardware FIFO, pedometer, and wake-on-motion engine so the MCU stays asleep between events instead of polling.
- **Compute-on-wake batching.** Keep heavy work (full-night re-scoring, radio sync) off during the night; do it in the morning on wake to protect the overnight power budget.
- **RTC-stamped event log** for every mode/state transition — cheap, and invaluable when debugging why a night looks wrong.

---

## 6. Risks & Open Questions

| Risk | Mitigation |
|---|---|
| Wrist PPG is noisy (motion, contact pressure, ambient light) | SQI gating — discard bad windows rather than log garbage; strap design matters as much as code |
| Wrist SpO2 accuracy is inherently poor | Report trends/min values, label as estimate |
| Reliable HRV is hard on wrist PPG — timing errors dominate RMSSD | Sub-sample peak interpolation, 200–400 Hz sampling, beat-acceptance gating, HRV only from still high-SQI windows, ECG-validated (see §3.3) |
| S3 draws more than the C6 → overnight battery life | 500–1000 mAh cell + duty-cycling; C6 is the documented fallback SoC if drain is still too high |
| Memory pressure from display + DSP + raw logging | S3's 8 MB PSRAM covers it; keep PPG processing streaming and flush raw buffers to SD periodically |
| 250 mAh stock battery too small for the S3 | Use a 500–1000 mAh cell on the same MX1.25 connector |
| Touch controller variant differs by batch (FT3168 vs FT6146) | Probe at runtime; both are FocalTech and near-identical over I2C |
| Which exact GPIOs the expansion I2C pads map to | Confirm from the Rev1.1 schematic in Waveshare's repo during Phase 0 |

Open questions to settle as we implement (defaults chosen so work can start):
1. **Session start:** manual "Start sleep" button for v1; auto-detection later. 
2. **Log format:** binary records + a tiny converter script (CSV is fine too if we prefer eyeballing files).
3. **Strap/enclosure:** 3D-printed case with sensor window vs. sewing the breakout into a strap pocket.

---

## 7. Repository Layout

```
Sleep-Tracker/
├── PLAN.md                  # this document
├── FEATURES.md              # prioritized feature list
├── INTEGRATION.md           # Home Assistant + CPAP design
├── firmware/                # ESP-IDF project (SCAFFOLDED — see firmware/README.md)
│   ├── CMakeLists.txt
│   ├── sdkconfig.defaults   # esp32s3 + octal PSRAM + 16 MB flash + custom partitions + PM
│   ├── partitions.csv
│   ├── main/                # app_main: dual-core task setup (sense | UI)
│   └── components/
│       ├── bsp/             # board pins/buses/PMU/RTC/SD — only board-specific code
│       ├── max30102/  ppg/  actigraphy/   # sensing + PPG/HRV pipeline
│       ├── sleep_core/      # epoch record, session SM, HRV, scoring
│       └── ui/  sync/       # LVGL screens; BLE/MQTT → HA + CPAP
├── hardware/                # wiring diagrams, strap/enclosure files (TODO)
├── tools/                   # Python log analysis / algorithm tuning (TODO)
└── docs/                    # per-subsystem notes as they solidify (TODO)
```

**Status:** the firmware scaffold is in place — valid ESP-IDF v5.x project, S3/PSRAM build config, component interfaces defined, and the dual-core task architecture wired in `app_main`. Bodies are stubbed and tagged `TODO(phaseN)`.

**Next step — Phase 0 on hardware:** confirm the expansion-pad GPIOs from the Rev1.1 schematic, fill in the `bsp` pins, port the Waveshare display/touch/LVGL bring-up, and get every I2C device (including the MAX30102 at 0x57) answering.
