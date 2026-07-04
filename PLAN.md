# Sleep Tracker — Project Plan

A wrist-worn sleep tracker built on the **Waveshare ESP32-S3-Touch-AMOLED-1.8** development board with an external **MAX3010x** pulse-oximetry sensor (**MAX30102** for initial bring-up, transitioning to the **MAX30101**). The device records heart rate, SpO2, and movement overnight to microSD and presents results on the onboard AMOLED touch display. Sleep scoring runs today as an offline prototype in [`tools/`](tools/); the on-device scorer is Phase-3 work (§5).

- Board wiki: https://www.waveshare.com/wiki/ESP32-S3-Touch-AMOLED-1.8
- Board docs: https://docs.waveshare.com/ESP32-S3-Touch-AMOLED-1.8
- Board firmware/examples: https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.8

> **Board choice (decided):** targeting the **ESP32-S3** variant to prioritize sample rate and full-fidelity data logging — its 8 MB PSRAM and dual-core CPU let us buffer and store raw overnight waveforms, not just epoch summaries. The pin-compatible **ESP32-C6** sibling (same display/touch/IMU/RTC/PMU/audio, but 512 KB SRAM, no PSRAM, lower power draw) is kept as a **fallback if overnight power draw proves too high**. The firmware BSP is structured to make that swap cheap (see §3).

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
- **HRV (nice-to-have, power-permitting)** — the device already runs PPG for HR/SpO2, so beat-to-beat IBI capture → RMSSD/SDNN can ride on top *if the power budget supports the longer clean-capture windows it needs* (decided by the measured overnight drain, not assumed). Not a prerequisite for anything else — if it's too costly or too noisy on the wrist, we thin or drop it before touching core tracking. Frequency-domain HRV (LF/HF) and respiratory-rate estimation build on the same capture. Details in §3.3.
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
| Display | 1.8" AMOLED 368×448, **CO5300** (on-hardware) | QSPI | UI, morning report |
| Touch | FT3168 / FT6146 | I2C | UI input |
| IMU | QMI8658 (6-axis accel + gyro) | I2C | Movement/actigraphy (polled accel; wake-on-motion INT not routed on this board) |
| RTC | PCF85063 (backup-battery pads) | I2C | Timekeeping across resets, epoch timestamps |
| PMU | AXP2101 | I2C | Battery charging, voltage/fuel readings, power rails |
| Audio | ES8311 codec + mics + speaker | I2S/I2C | Smart alarm; snore detection (stretch) |
| Storage | 16 MB NOR flash + microSD slot | SPI/SDIO | Firmware + night logs |
| Battery | MX1.25 header (3.7 V Li-Po) | — | Untethered overnight use |
| Expansion | 11 solder pads: I2C, UART, USB, 5V, 3V3, GND | — | **MAX3010x PPG sensor attaches here** |

Memory note: the ESP32-S3R8 has **512 KB internal SRAM plus 8 MB PSRAM**. That headroom is why we chose it — a full 368×448 RGB565 frame buffer (~330 KB) fits comfortably *and* we can hold hours of raw PPG + accel samples in RAM before flushing to SD, making full-waveform overnight logging the default rather than a compromise. (On the C6 fallback there is no PSRAM: LVGL would need partial draw buffers and logging would drop to epoch summaries — the reason the S3 is the primary target.)

### 2.2 PPG sensor integration (MAX3010x)

The external PPG sensor is an Analog Devices **MAX3010x**-family part on the shared I2C bus (address `0x57`):

- **MAX30102** — used for **initial bring-up** (red + IR LEDs, one photodiode). This is the part in hand.
- **MAX30101** — the **planned** sensor (arriving shortly). Register-compatible sibling at the same `0x57` address, but it **adds a green LED** — the wavelength commercial wrist trackers use because it's far less motion-sensitive at the wrist — while keeping red + IR for SpO2. Coming on the TinyCircuits **AST1041** Pulse-Oximetry Wireling (see [`datasheets/`](datasheets/)).

Because they're register-compatible, one driver covers both; the transition is a hardware swap, not a firmware rewrite. Sensor upgrade options beyond the MAX3010x (MAXM86161, MAX86141) are catalogued in [`datasheets/`](datasheets/).

```
MAX3010x breakout / AST1041 Wireling   ESP32-S3-Touch-AMOLED-1.8 pads
  VIN / VCC  ─────────────────────────  3V3
  GND        ─────────────────────────  GND
  SDA        ─────────────────────────  I2C SDA pad
  SCL        ─────────────────────────  I2C SCL pad
  INT        ─────────────────────────  (optional) spare GPIO — FIFO-ready interrupt
```

I2C address map (confirmed by on-board scan, Phase 0): ES8311 `0x18`, **TCA9554 `0x20`** (IO-expander, drives LCD/touch reset), AXP2101 `0x34`, FT3168 `0x38`, PCF85063 `0x51`, MAX3010x `0x57`, QMI8658 `0x6B` (SA0 high on this board).

The current driver **polls** the 32-sample FIFO (the INT line is unused on this board). Wiring INT is a future power optimization — it would let the CPU stay in light sleep between FIFO drains — but it is not required and is not wired today. **On the AST1041 Wireling, confirm whether the 5-pin connector even exposes INT** before pursuing it; if not, it means soldering to the chip's INT pad (noted in `datasheets/`).

**Mechanical:** the sensor must sit flush and snug against the top of the wrist (elastic strap, light pressure, no gap — motion artifact and ambient light leakage are the #1 cause of bad PPG). Enclosure/strap design is tracked as its own work item in Phase 0.

### 2.3 Power budget (drives many decisions)

Rough overnight (8 h) estimate with display off:

| Consumer | Strategy | Est. avg current |
|---|---|---|
| ESP32-S3 | Light sleep between sensor windows, radio off; PSRAM self-refresh in sleep | 2–8 mA |
| MAX3010x | Duty-cycled: ~30–60 s of PPG every 5 min, low LED current | 0.2–1 mA |
| QMI8658 | Accel-only low-power mode (~50 Hz), gyro off | <0.1 mA |
| AMOLED | Off during sleep; tap-to-wake | ~0 |

That lands roughly in the 3–10 mA average range. The S3 is thirstier than the C6 (dual-core, PSRAM self-refresh in sleep is the main adder), so a **500–1000 mAh Li-Po** is the recommended cell rather than the stock 250 mAh — both fit the same MX1.25 header. The optional **HRV** feature is the main power-hungry add-on — it wants longer continuous clean-capture windows — so **whether we enable HRV is decided by the measured power budget** (spike S4, VALIDATION.md): if the chosen cell comfortably covers a night *with* those windows, we keep them; if not, HRV is thinned or dropped first, before core tracking is touched. The sampling scheduler (Phase 2) makes this a configurable trade-off. **If overnight drain proves unacceptable even with the bigger cell, the C6 fallback is the lever** — same firmware, lower-power SoC, at the cost of raw-waveform logging.

### 2.4 External BLE sensor network (WT9011DCL body sensors + H10)

The wrist unit also acts as a **BLE central** for external wireless sensors it does not physically contain:

- **1..N × WitMotion WT9011DCL** — BLE 5.0 9-axis IMUs with an onboard AHRS that outputs Euler **angles directly** (0.2° X/Y accuracy). Worn on the **torso** (and optionally limbs) to capture true body orientation and movement.
- **Polar H10** — chest ECG, used as an HRV ground-truth reference (see [VALIDATION.md](VALIDATION.md)) and opportunistically for gold-standard beats.

**Why a body sensor at all — the wrist can't do position.** The onboard QMI8658 measures *wrist* orientation, which is a poor proxy for how the *body* is lying (your wrist can point anywhere while you're on your back). A torso-mounted WT9011DCL reports trunk roll/pitch directly, so **sleep position (back / left / right / belly) becomes authoritative** rather than guessed. That is the enabling piece for correlating CPAP events and SpO2 dips against position (INTEGRATION.md §6) — the marquee combined insight ("your AHI is worse on your back"), now robust.

**Position from angle (simple, because the sensor fuses for us).** The WT9011DCL's `0x61` data frame carries acceleration, angular velocity, and Euler angle in one packet. Classification is just thresholding the torso **roll** angle (supine ≈ face-up, prone ≈ ±180°, left/right ≈ ∓90°), with **pitch** distinguishing upright/sitting. No sensor-fusion math on our side.

**Practical constraints to design around:**

| Constraint | Detail | Consequence |
|---|---|---|
| Per-sensor battery | 130 mAh, ~14 mA active → **~8 h ≈ one night** per charge | Nightly charging is part of the UX; low output rate (~1–5 Hz for position) helps modestly; app must flag a sensor that died mid-night |
| BLE connection budget | S3 shares one 2.4 GHz radio between Wi-Fi (HA/MQTT) and BLE; NimBLE supports several links but they compete | Keep body-sensor **connection intervals long** (position is slow-changing) to bound bandwidth/power; cap the number of paired sensors; treat H10 as dev/opportunistic |
| Wireless time alignment | Each sensor has its own clock; the hub timestamps on receipt | Fine for slow position/movement; don't rely on it for sub-100 ms event alignment |
| Placement/compliance | Extra straps to don nightly | Torso sensor is the high-value default; limb sensors are an opt-in extension (limb-movement detection) |

Pairing is persistent (bond + remembered MAC/role), so a configured sensor rejoins automatically each night. Sensor **role/placement** (torso, left-leg, …) is stored per device so position logic knows which stream is the trunk.

---

## 3. Firmware Stack

- **Framework: ESP-IDF** (not Arduino) — built and run on **v6.0.2** (native, GCC 15.2) and **5.4.1** (PlatformIO `espressif32@6.11.0`, GCC 14.2); needs ≥5.3 for the managed Waveshare BSP. We need fine-grained power management (light-sleep with I2C/GPIO wake), the I2S driver for the ES8311, native USB (mass-storage offload / CDC streaming), and FreeRTOS dual-core task control. Waveshare publishes ESP-IDF examples for this exact board that we can lift drivers from.
- **Threading:** pin sensor acquisition (MAX3010x + QMI8658) to one core and UI/SD-logging to the other, so an LVGL redraw or a slow SD flush never drops samples — a concrete payoff of the dual-core S3.
- **UI: LVGL 9** with the `esp_lcd` **CO5300** QSPI driver (via the managed Waveshare BSP); full frame buffer in PSRAM (partial buffers only on the C6 fallback).
- **Components** (each its own ESP-IDF component under `components/`):
  - `board/` — board seam: **delegates** to Waveshare's managed BSP (`waveshare/esp32_s3_touch_amoled_1_8`) for display/touch/SD/PMU/expander bring-up, and re-exports a small `board_*` API. **All board-specific code is confined here** so an S3→C6 swap stays local. (Renamed from `bsp/` to cede the `bsp_` namespace to the vendor BSP.)
  - `max30102/` — MAX3010x PPG driver (MAX30102/30101, register-compatible): polled FIFO drain (32-deep; INT line unused on this board), configurable sample rate (50/100/200/400 Hz), LED-current control, shutdown/duty-cycle.
  - `ppg/` — signal processing: DC removal, band-pass, beat detection with sub-sample peak refinement → HR + inter-beat intervals (IBIs); IBI artifact/ectopic correction; ratio-of-ratios → SpO2; signal-quality index (SQI) to reject motion-corrupted windows.
  - `actigraphy/` — QMI8658 (wrist) driver config + per-epoch activity counts (band-passed accel magnitude). Wrist movement only; body position comes from `bodynet`.
  - `bodynet/` — **BLE central** for 1..N WT9011DCL body sensors (+ the H10 reference): pairing/bonding, per-sensor role/placement, angle→sleep-position classification, per-sensor movement. *(Stub — Phase 2.5; the CSV's body-position/movement columns are present but 0 until this lands.)*
  - `sleep_core/` — epoch assembler + night session state machine + opportunistic per-epoch RMSSD. **On-device sleep scoring is `TODO(phase3)`** — the algorithm currently lives offline in `tools/`. The CSV writer is a separate `sd_logger` component.
  - `sd_logger/` — crash-safe 14-column CSV epoch logger (fsync-per-epoch, torn-tail tolerant, card-pull remount/resume); implements the `sleep_core` persistence vtable.
  - `power/` — ACTIVE vs TRACKING power profiles (DFS + automatic tickless light-sleep via `esp_pm_configure`).
  - `rtc/` · `pmu/` — hand-rolled register-level PCF85063A and AXP2101 drivers.
  - `ui/` — LVGL 11-tile watch UI (incl. a live-vitals tile and a dev-only PPG-debug tile).
  - `sync/` — BLE GATT service + MQTT to Home Assistant (stretch). *(Stub — Phase 5.)*

### 3.1 Data flow

```
QMI8658 (50 Hz accel) ─────► wrist activity counts ┐
MAX3010x (400 Hz PPG) ──────► HR, IBIs, SpO2, SQI   ┤
WT9011DCL ×N (BLE, ~1-5 Hz) ► body position + movement ┼─► 30 s epoch record ─► ring buffer ─► microSD
PCF85063 ───────────────────► timestamps            ┘                            │
                                                                                 ▼
                                                                  sleep scorer (on epoch close +
                                                                  full-night re-pass in the morning)
```

**Epoch record (one 14-column CSV row):** timestamp, wrist activity count, **body position (back/left/right/belly)**, **body-movement count**, mean/min/max HR, RMSSD, SpO2, SQI-proxy, beat-acceptance %, battery %, flags. A full night is ~1000 rows ≈ tens of KB — trivial for SD. The schema is defined once in [`tools/sleeplog.py`](tools/sleeplog.py) (mirroring `sd_logger.c`); the body position/movement columns are present but 0 until Phase 2.5. *(Full-rate raw PPG currently streams over USB for offline tuning rather than to SD; raw-to-SD logging is a design goal, not yet built.)*

### 3.2 Sleep scoring approach

1. **Sleep/wake per epoch:** actigraphy first — a Cole-Kripke-style weighted moving window over activity counts. This is the well-trodden, robust baseline.
2. **Stage refinement with cardiac data:** within sleep, use HR drop below nightly baseline + low variability → deep; elevated HR variability/irregularity with near-zero movement → REM-estimate; otherwise light. Applied as a full-night smoothing pass in the morning (avoids committing to noisy real-time calls).
3. **SpO2** is reported as min/mean and a "dips below 90%" count — informational, not used for staging.
4. Because raw-ish data lands on the SD card, the algorithm can be re-tuned offline against the recorded nights (and optionally against a reference device) without reflashing mid-project.

### 3.3 HRV — a power-permitting nice-to-have (and what doing it well takes)

HRV is a **nice-to-have, not a requirement** — we enable it only when the measured power budget (§2.3) supports the longer clean-capture windows it needs, and **nothing else in the project waits on it**. That said, HRV is the most timing-sensitive thing the device *could* measure: RMSSD is computed from the *differences* between consecutive beat intervals, so it's dominated by small errors in each beat's timestamp — errors that HR (a smoothed average over many beats) completely hides. So *if* we enable HRV, doing it well constrains several design choices:

1. **Timing resolution beats raw sample rate.** RMSSD in relaxed sleep is often 20–60 ms. To resolve that we need per-beat timing error well under ~5 ms. A 200 Hz PPG stream gives only 5 ms sample spacing, so it needs sub-sample peak interpolation (parabolic/quadratic fit around the detected peak, or cross-correlation against a beat template) to recover sub-millisecond timing between samples. Sample at **200 Hz minimum, 400 Hz preferred** (the MAX3010x supports it) — higher rate makes interpolation more accurate and cheaper.
2. **Fiducial-point consistency.** Pick one repeatable feature per beat and use it for every beat — the systolic-upstroke maximum of the first derivative is more stable against pulse-shape changes than the raw waveform peak. Consistency matters more than which point.
3. **Beat classification is essential to HRV specifically.** Missed/extra beats and motion artifacts create huge fake IBI jumps that wreck RMSSD, so if HRV is enabled the pipeline should classify each IBI (normal / ectopic / artifact) and interpolate or exclude bad beats before any HRV math. Report the **percentage of accepted beats** alongside every HRV value — an RMSSD from a 40%-accepted window is not trustworthy and should be flagged or hidden.
4. **HRV only from clean, still windows.** Wrist PPG HRV is only credible during motionless, high-SQI stretches. Gate HRV on: activity count near zero (IMU), SQI above threshold, and a minimum count of consecutive accepted beats (e.g. ≥2 min / ~120 beats for a stable RMSSD). Prefer quality over coverage — report HRV for the windows that qualify and leave gaps elsewhere rather than emitting noise.
5. **Store the IBI series, not just the summary.** Log the full accepted-IBI list (or the raw PPG) to SD so RMSSD/SDNN/pNN50 and frequency-domain metrics (LF/HF, needs ~5 min windows) can be recomputed and re-tuned offline. The S3's PSRAM makes keeping the raw stream the default.
6. **Duty-cycling vs. HRV is a real tension.** Short 30–60 s PPG bursts save power but may not contain a long enough clean run for a stable RMSSD. Resolution: run a **nightly HRV mode** that captures longer continuous windows during deep/stable sleep (e.g. a 5-min clean capture per sleep cycle) rather than relying on the short duty-cycle bursts. The sampling scheduler (Phase 2) exposes this as a configurable trade-off.
7. **Sanity-check against ground truth (optional).** If HRV is enabled, it's worth checking against an ECG chest strap (e.g. Polar H10, which exposes RR intervals over BLE) on the same wrist/session — good Bland-Altman agreement on RMSSD lets us surface HRV with confidence, poor agreement means we label it low-confidence or hide it. This is a quality check for the HRV feature, **not a gate on any other feature** — core sleep tracking ships regardless.

**Reported HRV metrics:** RMSSD (primary, robust for overnight), SDNN, pNN50, mean IBI, and per-value beat-acceptance % + coverage. Frequency-domain LF/HF is a P2 stretch (needs the longer clean windows from item 6).

---

## 4. Device States & UX

```
        tap / button                      tap Start (on-screen button; auto-detect later)
IDLE  ────────────────►  WATCH FACE  ───────────────────────────►  TRACKING
(display off,             (time, battery,                          (panel off, sensors duty-cycling,
 tap-to-wake armed)        last night's score)                      double-tap wakes to the Stop button)
                                ▲                                        │ tap Stop / alarm
                                └──────────  MORNING REPORT  ◄───────────┘
                                             (score, hypnogram, HR/SpO2 charts)
```

Screens for v1: **watch face**, **tracking (minimal clock + "tracking" glyph)**, **morning report** (score card → swipe to hypnogram → swipe to HR/SpO2 graphs), **settings** (alarm time, brightness, start/stop mode).

---

## 5. Milestones

**The arc at a glance** — read this first, then the per-phase detail below. Each phase has a one-line goal and an exit gate you can check against.

| Phase | Goal | Exit gate |
|---|---|---|
| **0 — Bring-up** | Get the board alive: BSP + display/LVGL up, MAX3010x on the I2C bus | Boots to an LVGL screen; all I2C devices enumerate; runs untethered |
| *→ de-risk checks* | *Before building, run VALIDATION.md spikes S1–S5 (power, SD, coupling)* | *Power/SD/coupling hold up; S1 tells you if the optional HRV feature is feasible* |
| **1 — Sensor drivers** | Turn raw sensors into live vitals (HR / SpO2 / movement) | Live vitals on screen; RTC + battery gauge working |
| **2 — Recording** | Log a full night to microSD on battery (HRV capture optional, power-permitting) | Records an 8 h night; log opens cleanly |
| **2.5 — Body sensors** | Pair WT9011DCL over BLE → authoritative sleep position | Torso sensor reports correct position all night; lands in the SD log |
| **3 — Sleep scoring** | Score sleep, break metrics down by position (optionally validate HRV vs ECG) | Hypnogram/score match a reference; per-night position breakdown |
| **4 — UI polish** | The four v1 screens + 7-night history, usable on-device | A real night's report renders end-to-end from an SD log *(shell of all 11 screens built & running on HW; report data still sample)* |
| **5 — Stretch** | Smart alarm, BLE sync, respiratory rate, snore detection | No hard gate — pick items opportunistically |
| **v3+ — Integration** | HA/MQTT + combined wrist+CPAP summary | Separate track I0–I5 — see [INTEGRATION.md](INTEGRATION.md) §7 |

### Phase 0 — Bring-up & skeleton (foundation)
**Goal:** get the board alive and wearable — pixels on screen, every device answering, running on battery.
- [x] Repo scaffolding: ESP-IDF project + `sdkconfig.defaults`. *(A CI build via GitHub Actions is still unbuilt.)*
- [x] Port Waveshare BSP: display + touch + LVGL up; PMU, RTC, IMU verified over I2C, microSD detected (FAT mount + crash-safe writer landed in Phase 2).
- [x] Wire the MAX3010x PPG sensor (MAX30102 first, then MAX30101 — same `0x57` address) to the I2C pads; confirm it ACKs alongside the onboard devices.
- [ ] First-pass strap/enclosure so the unit can actually be worn (even if crude).
- **Exit criteria:** board boots into an LVGL screen, all I2C devices enumerate, battery charges and runs untethered.

### Phase 1 — Sensor drivers
**Goal:** turn the raw sensors into trustworthy live vitals and a per-beat IBI series.
- **Before building, run the de-risking spikes** in [VALIDATION.md](VALIDATION.md) §1 (power, SD throughput, sensor coupling). Spike **S1** checks whether the *optional* HRV feature is feasible on your wrist — it does **not** gate this phase; a poor S1 just means HRV is deferred or dropped while HR/SpO2/movement proceed.
- [x] MAX3010x driver: polled FIFO (INT unused on this board), configurable sample rate (50/100/200/400 Hz) / LED current, shutdown/duty-cycle mode.
- [x] PPG pipeline: despike + band-pass filtering, adaptive-threshold beat detection → live HR; SpO2 ratio-of-ratios; **real composite SQI** + a live rolling-window RMSSD.
- [x] QMI8658 low-power accel mode + activity counts. *(Wake-on-motion interrupt dropped — the QMI8658 INT is not routed on this board; TRACKING uses timer-wake.)*
- [x] RTC set/read; battery gauge via AXP2101.
- [ ] (Optional, if pursuing HRV) Sub-sample peak interpolation → per-beat IBI series with timing error < 5 ms; IBI artifact/ectopic classifier + beat-acceptance %. (Basic beat detection for HR is already covered above; this is the HRV-grade precision.)
- [ ] (Optional, if pursuing HRV) Dev-only `refmon` component: subscribe to the Polar H10 RR intervals over BLE and show them next to the wrist RMSSD, timestamped by one device clock (VALIDATION.md §3) — a live reference for tuning HRV. It also stands up the BLE-central plumbing that `bodynet` reuses in Phase 2.5.
- **Exit criteria:** a live "vitals" screen showing plausible HR (±5 bpm vs finger check), SpO2, and movement level. (If HRV is being pursued, a live RMSSD tracking the H10 in a still 2-min window is a nice bonus check — not required to pass this phase.)

### Phase 2 — Recording pipeline
**Goal:** record a full night to microSD on battery (with an optional, power-permitting nightly HRV capture mode).
- [x] Epoch assembler + SD writer (append-only CSV, crash-safe: fsync-per-epoch, torn-tail-tolerant; `events.log` flight recorder; card-pull remount+resume). *Run on hardware: the SD card mounts, a session opens the log file, and continuous 30 s epochs are written to the card — bench-validated over USB (correct t_unix grid, populated activity/flags).*
- [x] Night session state machine (manual start/stop). **Trigger = on-screen Start/Stop button** (Tracking tile toggle + a watch-face shortcut), wired to `sleep_core_request_start/stop()`; auto-detect later.
- [x] Power work: panel blanked + redundant UI-task gated during a distinct TRACKING mode, MAX30102 duty-cycled (45 s PPG window / 5 min). **LVGL/touch are kept live in TRACKING** so the on-screen Stop button stays reachable — so the CPU stays at the ACTIVE profile (full speed, no light sleep): dropping into DFS/automatic light-sleep glitched the still-live QSPI panel back on mid-session. Deep CPU light-sleep needs a hardware wake source to coexist with an on-screen button and is deferred; the panel-off is the real TRACKING saving. **Wake-on-motion dropped** — the QMI8658 INT is not routed on this board; timer-wake only. *Overnight drain still UNMEASURED (spike S4) — needs a Li-Po on the header first.*
- [ ] (Optional, power-permitting) Nightly HRV mode: capture longer continuous clean windows (~5 min/sleep cycle) for stable RMSSD instead of short duty-cycle bursts, and log the accepted-IBI series to SD. *Scaffolded as a separate binary side-file; default off.* Enable only if the measured overnight drain leaves room.
- **Exit criteria:** device records a full 8-hour night on battery and the log opens cleanly in a notebook/spreadsheet. *The "opens cleanly" half is met (`tools/read_night.py`), and the **flash + microSD bench run is done** — a session started from the on-screen button writes continuous 30 s epochs to the card on hardware. The "8 h on battery" half still needs: a **Li-Po on the header** (gauge reads 0% — likely no cell attached), then an overnight run + spikes S3/S4 (SD throughput + overnight mAh via an external coulomb counter). Overnight power now also depends on the deferred deep-sleep path (LVGL is kept live for the on-screen Stop button — see the Power-work note above).*

### Phase 2.5 — Body-sensor network (WT9011DCL over BLE)
**Goal:** pair external BLE body sensors to get authoritative sleep position (back/left/right/belly).
- [ ] `bodynet` BLE-central: scan, pair/bond, and persist a WT9011DCL by MAC + role/placement; auto-reconnect on the next night.
- [ ] Parse the WitMotion `0x61` frame (accel + angular velocity + Euler angle); expose per-sensor angle + a movement count.
- [ ] Torso roll/pitch → **sleep position** (back / left / right / belly); log position + body-movement into the epoch record.
- [ ] Scale to N sensors; long connection intervals; surface a per-sensor battery/last-seen status and a pairing screen in the UI.
- [ ] (Reuses the BLE-central plumbing first stood up for the H10 `refmon` in Phase 1 — VALIDATION.md §3.)
- **Exit criteria:** a paired torso sensor reports correct position through position changes across a night, survives a disconnect/reconnect, and its position/movement land in the SD log alongside the wrist/PPG data.

### Phase 3 — Sleep scoring
**Goal:** score the night and break metrics down by body position (optionally validate HRV against ECG).
*Status: an offline prototype of the whole scoring pipeline (sleep/wake + staging + metrics + score) lives in `tools/` and is validated on synthetic nights (~98% sleep/wake, ~85–90% 4-stage vs ground truth). Remaining: tune thresholds on real recorded nights, then port the re-pass into on-device `sleep_core`.*
- [ ] Actigraphy sleep/wake classifier; tune on recorded nights. *(Prototyped in `tools/score_night.py` — Cole-Kripke-style; needs real-night tuning + on-device port.)*
- [ ] Morning re-pass: stage refinement from HR/HRV, summary metrics, sleep score.
- [ ] **Position-resolved summaries** — time-in-position (supine vs lateral vs prone), and position-segmented HR/SpO2/HRV, laying the groundwork for the positional-apnea correlation with CPAP data (INTEGRATION.md §6).
- [x] Offline analysis scripts (`tools/`, Python): `read_night.py` (log validator), `gen_synthetic_night.py` (synthetic nights + ground-truth hypnogram), and `score_night.py` (sleep/wake + staging + metrics + sleep score + SVG hypnogram + ground-truth validation). *Developed and verified against synthetic nights; the `--truth` loop is the harness for tuning on real data.*
- [ ] (Optional, only if HRV is enabled) **HRV sanity-check against ECG** — record simultaneous sessions vs. a Polar H10, compare RMSSD/SDNN with Bland-Altman analysis; good agreement lets us surface HRV with confidence, poor agreement means we label it low-confidence or hide it. **Full protocol in [VALIDATION.md](VALIDATION.md).**
- **Exit criteria:** hypnogram + score for a real night that roughly matches subjective experience / a reference tracker, **and** a per-night position breakdown with position-segmented SpO2. (If HRV is enabled, ECG agreement on clean windows is a bonus quality check, not a gate.)

### Phase 4 — UI polish
**Goal:** make the on-device experience complete and pleasant.
- [x] **Watch UI shell** — an 11-tile swipeable LVGL app running on hardware (watch face · live vitals · tracking · score · hypnogram · heart & O2 · position · history · alarm · settings · PPG-debug), dark AMOLED theme + the CVD-validated sleep-stage palette. Interactive design reference: [docs/watch-ui-mockup.html](docs/watch-ui-mockup.html).
- [x] **Live tiles wired to sensors** — watch face (RTC time/date + battery), live vitals (HR / SpO2 / real HRV / SQI + a live PPG pulse waveform from `ppg_copy_waveform()`), tracking clock, and a **PPG-debug tile** (the tuning graph + rate/HR/HRV/SQI, kept from the pre-watch-UI display). Settings is a scrollable list of ~14 finger-sized controls.
- [x] **Display sleep + double-tap-to-wake** — blank the AMOLED (Settings → *Sleep display*) and wake with a double-tap anywhere; touch stays live at brightness 0.
- [ ] Wire the **morning-report / position / history** tiles to real data — they render a representative sample night today (needs the Phase 3 scorer ported on-device + Phase 2.5 body sensors).
- [ ] Brightness/AOD handling, 7-night history from real logs.
- **Exit criteria:** all v1 screens navigable on-device (**met — as a shell**), and a real night's report renders end-to-end from an SD log (**pending on-device scoring**).
- *Build note:* the rich UI needs larger Montserrat fonts and LVGL's C-lib allocator (the default 64 KB pool is exhausted by ~200 widgets) — both in `sdkconfig.defaults`; see [firmware/README.md](firmware/README.md).

### Phase 5 — Stretch features
**Goal:** add the nice-to-haves once the core device is solid.
- [ ] Smart alarm (light-sleep window + ES8311 speaker tones).
- [ ] BLE GATT sync + simple companion (Web Bluetooth page or Python script) to pull logs.
- [ ] Respiratory rate from PPG; snore detection from mics; Wi-Fi upload/dashboard.
- **No hard gate:** stretch items are picked opportunistically — ship whichever land and defer the rest. (Home Assistant + CPAP integration continues as the separate v3+ track I0–I5 in [INTEGRATION.md](INTEGRATION.md) §7.)

### 5.1 Optimizations & techniques to bank

Engineering techniques to apply as the relevant components are built — most improve *both* data quality and power, which is why they're worth designing in early rather than bolting on:

- **Motion-gated PPG (cross-sensor).** Use the IMU to blank/flag PPG during movement. Improves HRV quality (drop corrupted beats at the source) *and* saves power (skip processing corrupted windows). The single highest-leverage optimization — it ties the two core sensors together.
- **Adaptive LED current / AGC on the MAX3010x.** Auto-tune LED drive to keep the PPG in the ADC's sweet spot across skin tone and contact quality. Better SQI than a fixed current, and less power than running the LEDs hot "to be safe."
- **Template-matching beat detection.** Cross-correlate each pulse against a per-user beat template for a stable fiducial point — more robust timing than a raw peak, which directly helps RMSSD (VALIDATION.md §2).
- **Double-buffered DMA SD writes from PSRAM.** Ping-pong ring buffers so sample acquisition never blocks on an SD flush (de-risked by spike S3).
- **On-the-fly raw compression** (delta + RLE / simple predictive) to shrink the raw-PPG log without losing fidelity, if S3 shows SD volume/throughput is tight.
- **Offload motion to the QMI8658.** Its hardware FIFO / pedometer / wake-on-motion engine could let the MCU sleep between events instead of polling — but **wake-on-motion is unavailable on this board** (the QMI8658 INT line is not routed); the driver currently polls and TRACKING uses timer-wake.
- **Compute-on-wake batching.** Keep heavy work (full-night re-scoring, radio sync) off during the night; do it in the morning on wake to protect the overnight power budget.
- **RTC-stamped event log** for every mode/state transition — cheap, and invaluable when debugging why a night looks wrong.

---

## 6. Risks & Open Questions

| Risk | Mitigation |
|---|---|
| Wrist PPG is noisy (motion, contact pressure, ambient light) | SQI gating — discard bad windows rather than log garbage; strap design matters as much as code |
| Wrist SpO2 accuracy is inherently poor | Report trends/min values, label as estimate |
| HRV is hard on wrist PPG — timing errors dominate RMSSD | HRV is a nice-to-have, not a requirement: pursue it only if the power budget allows (§2.3) and the signal supports it (sub-sample interpolation, beat-acceptance gating, still high-SQI windows); if it's too costly or noisy, thin or drop it — core tracking is unaffected (see §3.3) |
| S3 draws more than the C6 → overnight battery life | 500–1000 mAh cell + duty-cycling; C6 is the documented fallback SoC if drain is still too high |
| Memory pressure from display + DSP + raw logging | S3's 8 MB PSRAM covers it; keep PPG processing streaming and flush raw buffers to SD periodically |
| 250 mAh stock battery too small for the S3 | Use a 500–1000 mAh cell on the same MX1.25 connector |
| Touch controller variant differs by batch (FT3168 vs FT6146) | Probe at runtime; both are FocalTech and near-identical over I2C |
| Which exact GPIOs the expansion I2C pads map to | Confirmed in Phase 0 — the external MAX30102 now ACKs at `0x57` on the shared I2C pads alongside the onboard devices |
| Body sensors only last ~one night (130 mAh) | Nightly charging as UX; low output rate; flag a sensor that dies mid-night rather than silently gap |
| BLE (N body sensors + H10) contends with Wi-Fi on one radio | Long connection intervals (position is slow); cap paired-sensor count; H10 as dev/opportunistic; verify NimBLE max-connections config |
| Wireless sensors aren't clock-synced to the hub | Hub-receive timestamps suffice for slow position/movement; don't use them for sub-100 ms event alignment |
| WT9011DCL BLE UUIDs/frame details vary by firmware | Confirm service/char UUIDs + `0x61` framing against the WitMotion datasheet during Phase 2.5 bring-up |

Open questions to settle as we implement (defaults chosen so work can start):
1. **Session start:** *decided — on-screen Start/Stop button* (a Tracking-tile toggle + a watch-face shortcut, wired to `sleep_core_request_start/stop()`); auto-detection later.
2. **Log format:** *decided — crash-safe 14-column CSV* (append-only, fsync-per-epoch, torn-tail tolerant); schema in `tools/sleeplog.py`, reader `tools/read_night.py`.
3. **Strap/enclosure:** 3D-printed case with sensor window vs. sewing the breakout into a strap pocket. *(Still open.)*
4. **Body-sensor count/placement:** default is a single torso sensor for position; limb sensors (limb-movement detection) are an opt-in extension. How many to support as a hard cap?

---

## 7. Repository Layout

```
Sleep-Tracker/
├── PLAN.md                  # this document
├── README.md                # entry point + doc map
├── FEATURES.md              # prioritized feature list
├── INTEGRATION.md           # Home Assistant + CPAP design
├── VALIDATION.md            # de-risking spikes S1–S5
├── CLAUDE.md                # repo working conventions
├── datasheets/              # part datasheets + I²C bus map
├── firmware/                # ESP-IDF project — boots + runs on hardware (see firmware/README.md)
│   ├── CMakeLists.txt
│   ├── platformio.ini       # espressif32@6.11.0 (IDF 5.4.1) — do not bump to 7.0.x
│   ├── sdkconfig.defaults   # esp32s3 + octal PSRAM + 16 MB flash + custom partitions + PM
│   ├── partitions.csv
│   ├── main/                # app_main: dual-core task setup (sense | UI); button-driven start/stop
│   └── components/
│       ├── board/           # board seam → delegates to managed Waveshare BSP (only board-specific code)
│       ├── max30102/  ppg/  actigraphy/   # sensing + PPG pipeline (HR/SpO2/SQI/live RMSSD)
│       ├── rtc/  pmu/        # PCF85063A + AXP2101 register-level drivers
│       ├── sleep_core/      # epoch assembler + session SM + per-epoch RMSSD (on-device scoring = TODO phase3)
│       ├── sd_logger/       # crash-safe 14-column CSV epoch logger
│       ├── power/           # ACTIVE vs TRACKING profiles (DFS + tickless light-sleep)
│       ├── bodynet/         # BLE central: WT9011DCL body sensors + H10  — STUB (Phase 2.5)
│       └── ui/  sync/       # LVGL 11-tile watch UI; sync (BLE/MQTT → HA + CPAP) — STUB (Phase 5)
├── tools/                   # Python: sleeplog, read_night, gen_synthetic_night, score_night, capture_ppg, analyze_ppg
├── docs/                    # watch-ui-mockup.html + per-subsystem notes
└── hardware/                # wiring diagrams, strap/enclosure files (planned — not yet created)
```

**Current status** (per-phase detail + exit gates in §5):

- **Phases 0–1 — done and verified on hardware.** The board boots (dual-core: `sensor` on core 1, `ui` on core 0); all onboard I2C devices enumerate (ES8311 `0x18`, TCA9554 `0x20`, AXP2101 `0x34`, FT3168 `0x38`, PCF85063 `0x51`, QMI8658 `0x6B`) plus the external MAX30102 at `0x57`; the CO5300 AMOLED + LVGL come up and touch works. Board bring-up delegates to Waveshare's managed BSP via `components/board/`, which also releases the LCD/touch resets the vendor BSP leaves asserted (via the TCA9554). Hand-rolled register-level drivers (`rtc`, `pmu`, `actigraphy`, `max30102`) read live, and the PPG/DSP pipeline yields live HR (~81 bpm) + SpO2 (98–99%) plus a live rolling-window RMSSD. Builds and runs on ESP-IDF v6.0.2 (native) and 5.4.1 (PlatformIO).
- **Phase 2 — recording bench-validated on hardware; exit gate partially met.** The epoch assembler + button-triggered session FSM (`sleep_core`), crash-safe CSV logger (`sd_logger`), and TRACKING power mode (`power`) are written, reviewed, and validated over USB: the SD card mounts, an on-screen **Start** opens the log, and continuous 30 s epochs write to the card. Raw PPG is also logged to SD during a recording — buffered in PSRAM and flushed one block per PPG window to `<ts>_ppg.bin` (few large sequential writes to spare SD wear; reader `tools/read_raw_ppg.py`) — verified on hardware (a 45 s window flushed one ~108 KB block). Still open: the **8 h-on-battery night** — needs a Li-Po on the header (the AXP2101 gauge reads 0%, flagged `BATT_INVALID`) plus spikes S3/S4, and the deferred deep-sleep path (LVGL is kept live in TRACKING for the on-screen Stop button, so full light-sleep needs a hardware wake source). Wake-on-motion was dropped (QMI8658 INT unrouted; timer-wake only).
- **Phase 4 — watch UI shell running on hardware.** An 11-tile swipeable LVGL app (`components/ui`): watch · live vitals · tracking · score · hypnogram · heart & O2 · position · history · alarm · settings · PPG-debug, with display-sleep + double-tap-to-wake. Tracking is driven by an **on-screen Start/Stop button** (Tracking tile + a watch-face shortcut), and Settings carries a selectable **HR sample rate (50–800 Hz)**. The watch / live vitals / tracking / PPG-debug tiles show live sensor data; the score / hypnogram / heart & O2 / position / history tiles render a representative **sample** night pending on-device scoring + Phase 2.5 body sensors. Details in [firmware/README.md](firmware/README.md); interactive design reference: [docs/watch-ui-mockup.html](docs/watch-ui-mockup.html).
- **Phase 3 (scoring) — offline prototype only.** The full pipeline lives in `tools/score_night.py` (validated on synthetic nights, ~98% sleep/wake, ~85–90% 4-stage); on-device scoring is a `TODO(phase3)` in `sleep_core.c`.
- **Not started:** `bodynet` (Phase 2.5 body sensors) and `sync` (Phase 5) are stubs; the CSV's body-position/movement columns are always 0.

**Next steps:** (1) attach a Li-Po and run an overnight session + spikes S3/S4 to close the "8 h on battery" gate; (2) port the offline scorer into on-device `sleep_core` and wire the report/position/history tiles to real logs; (3) build out the Phase 2.5 body-sensor network.

**Next steps:** (1) flash + microSD bench run — **done**: epochs write to the card on hardware and light-sleep engages; (2) attach a **Li-Po on the header** (gauge reads 0%), then run an overnight session + spikes S3/S4 (SD throughput + overnight mAh on an external coulomb counter) to close the "8 h on battery" exit gate; (3) Phase 2.5 body sensors / Phase 3 scoring (the offline scorer in `tools/` is ready to port on-device).
