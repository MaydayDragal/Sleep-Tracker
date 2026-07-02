# Feature List

Brainstormed feature set for the wrist-worn sleep tracker (ESP32-S3-Touch-AMOLED-1.8 + MAX30102). Organized by category, with a priority tier on each item:

- **[P0]** core — the device isn't a sleep tracker without it (maps to Phases 0–3 in PLAN.md)
- **[P1]** should-have — planned, but the device is useful before it lands
- **[P2]** stretch — valuable, build when the foundation is solid
- **[HW]** requires additional hardware beyond the board + MAX30102

---

## 1. Sleep Tracking (core)

- **[P0] Sleep/wake detection** per 30 s epoch (actigraphy-based, HR-assisted)
- **[P0] Sleep staging estimate** — awake / light / deep / REM from movement + HR/HRV
- **[P0] Hypnogram** — full-night stage timeline
- **[P0] Sleep summary metrics** — total sleep time, time in bed, sleep efficiency, sleep-onset latency, WASO (wake after sleep onset), number of awakenings
- **[P0] Sleep score** — single 0–100 number blending duration, efficiency, continuity, and stage balance
- **[P0] Manual session start/stop** — explicit "start sleep" control for v1
- **[P1] Automatic sleep detection** — sensor-driven session start/end, no button needed
- **[P1] Nap detection** — short daytime sessions scored separately from nights
- **[P2] Restlessness index** — per-night tossing/turning intensity and position-change count

## 2. Biometrics

- **[P0] Continuous overnight heart rate** — per-epoch mean/min/max
- **[P0] Resting heart rate** — nightly RHR, the single most useful trend number
- **[P0] SpO2 monitoring** — per-epoch estimate, nightly min/mean
- **[P0] Signal-quality index** — every biometric tagged with confidence; bad windows discarded, not logged as fact
- **[P0] HRV (core goal)** — reliable beat-to-beat IBI capture → RMSSD/SDNN/pNN50 per clean window and nightly average; sub-sample peak timing, beat-acceptance gating, ECG-validated. See PLAN.md §3.3
- **[P2] Frequency-domain HRV** — LF/HF from ~5-min clean windows (needs longer continuous captures)
- **[P1] SpO2 desaturation events** — count and duration of dips below a threshold (informational, not diagnostic)
- **[P1] Respiratory rate** — breaths/min derived from PPG baseline modulation
- **[P2] Live vitals screen** — real-time HR/SpO2/waveform view (doubles as a sensor-placement checker)
- **[P2] Abnormality flags** — "resting HR 15% above your baseline this week"-style nudges, clearly labeled non-medical

## 3. Movement & Activity

- **[P0] Actigraphy pipeline** — band-passed accel magnitude → per-epoch activity counts
- **[P1] Wake-on-motion** — IMU interrupt wakes the system from low-power states
- **[P1] Sleep-position change log** — orientation shifts through the night
- **[P2] Daytime step counting** — QMI8658 pedometer; makes it a passable daytime watch too
- **[P2] Wrist-off detection** — accel stillness + PPG contact loss ends/pauses a session instead of logging garbage

## 4. Audio (ES8311 codec + dual mics + speaker)

- **[P1] Smart alarm sounds** — gentle, escalating tones through the speaker
- **[P2] Snore detection** — mic energy/pattern analysis, logged per epoch, aligned with the hypnogram and SpO2 dips
- **[P2] Ambient noise logging** — nightly noise-level profile ("your sleep degraded when noise spiked at 3 am")
- **[P2] White/brown noise player** — sleep-onset soundscapes with auto-off once asleep
- **[P2] Sound-event capture** — optionally save short clips around loud events (sleep talking, snoring); off by default, local-only, privacy-first

## 5. Alarms & Waking

- **[P0] Basic alarm** — fixed-time alarm with touch dismiss
- **[P1] Smart alarm window** — wake during the lightest sleep in the ~30 min before the deadline
- **[P1] Gradual wake** — quiet start, slow escalation; AMOLED sunrise-style brightening
- **[P2] Wake confirmation** — require a gesture/interaction, re-arm if the user falls back asleep
- **[P2] Bedtime reminder** — nudge toward a consistent sleep schedule
- **[HW] Vibration alarm** — silent wake needs a haptic motor added via expansion pads (great with a partner in the bed)

## 6. Watch & Daily UX

- **[P0] Watch face** — time, date, battery, last night's score
- **[P0] Tracking screen** — minimal dimmed clock during sleep, "recording" indicator
- **[P0] Morning report** — score card → hypnogram → HR/SpO2 charts, swipeable
- **[P0] Settings** — alarm, brightness, start/stop mode, time
- **[P1] Tilt-to-wake / tap-to-wake** — display stays off until summoned
- **[P1] 7-night history** — on-device trend view of scores and key metrics
- **[P2] Always-on display mode** — dim clock (AMOLED-friendly, black background), with burn-in mitigation (pixel shifting)
- **[P2] Do-not-disturb / theater mode** — guarantee zero light and sound during the night
- **[P2] Multiple watch faces** — because it's fun

## 7. Data, Storage & Sync

- **[P0] Crash-safe epoch logging to microSD** — append-only records; a power loss never corrupts a night
- **[P0] Raw waveform logging** — full-rate PPG + accel streams to SD (the S3's PSRAM makes this the default, not an option); every night becomes re-analyzable forever
- **[P0] Offline analysis toolkit** — Python scripts to plot nights, tune the scorer, and validate against reference devices
- **[P1] USB mass-storage offload** — plug into a PC, SD card appears as a drive
- **[P1] Live USB streaming mode** — raw PPG/accel over USB CDC for algorithm development
- **[P1] CSV export** — spreadsheet-friendly conversion of any night
- **[P2] BLE sync** — pull summaries/logs to a phone or PC without a cable
- **[P2] EDF export** — the sleep-research standard format; opens nights in real polysomnography tooling
- **[P2] Wi-Fi upload** — push nights to a self-hosted dashboard (Home Assistant / InfluxDB + Grafana)
- **[P2] NTP / BLE time sync** — RTC drift correction
- *Home Assistant + CPAP integration has its own section — see §11.*

## 8. Insights & Trends (needs multi-night history)

- **[P1] Weekly/monthly trends** — sleep duration, RHR, HRV, score over time
- **[P2] Sleep-debt tracking** — cumulative shortfall vs. a personal sleep goal
- **[P2] Consistency score** — how regular bed/wake times are (regularity rivals duration in importance)
- **[P2] Tagging & correlations** — morning tags (caffeine, alcohol, exercise, stress) correlated against sleep quality over weeks
- **[P2] Personal baselines** — all thresholds (HR dip, HRV bands) adapt to the wearer instead of population constants

## 9. Environment (mostly needs add-on sensors)

- **[P2] Noise environment profile** — from the onboard mics (see §4)
- **[HW] Room temperature & humidity** — BME280/SHT4x on the spare I2C pads; correlate room climate with sleep quality
- **[HW] Ambient light logging** — small I2C lux sensor; "your room isn't actually dark"

## 10. System & Platform

- **[P0] Power management** — display off while tracking, light sleep between sensor windows, duty-cycle scheduler with a "max-fidelity" continuous mode
- **[P0] Battery awareness** — charge level on screen; refuse to start a night below a threshold; low-battery graceful shutdown that closes the log cleanly
- **[P1] Charge reminder** — "charge me before tonight" nudge based on projected overnight drain
- **[P1] OTA firmware updates** — over Wi-Fi, so strap disassembly isn't needed to ship fixes
- **[P1] On-device diagnostics** — I2C device health, SD free space, sensor self-test screen
- **[P2] Privacy stance as a feature** — everything processed and stored locally; nothing leaves the device unless the user explicitly syncs
- **[P2] Multi-profile support** — probably overkill for a wrist-worn personal device, listed for completeness

## 11. Home Assistant & CPAP Ecosystem (long-term, v3+) — full design in [INTEGRATION.md](INTEGRATION.md)

- **[P2] MQTT publish + HA auto-discovery** — nightly sleep/HR/HRV/SpO2 summary appears automatically as Home Assistant sensors (history, dashboards, alerts for free)
- **[P2] Combined night summary on the wrist** — the headline: pull CPAP therapy data (AHI, mask leak, pressure, usage) back from HA and render it alongside device metrics on the AMOLED
- **[P2] Remote config from HA** — set alarm, thresholds, brightness from a phone/dashboard; publish live HR while awake
- **[P2] OTA over Wi-Fi via HA** — ties into §10 OTA
- **[P2] Positional-apnea detection** — cross wrist IMU sleep position with CPAP event timestamps ("AHI 4× higher on your back") — neither device can do this alone
- **[P2] Second-sensor efficacy check** — overlay CPAP residual-event times on wrist SpO2/HR traces to see if therapy is fully controlling events
- **[P2] Wrist-as-CPAP-oximeter** *(airbridge/airbreak path)* — the ESP32-S3 presents as a BLE/UDP oximeter so the CPAP records the wrist's SpO2/HR in its own EDF → *exact* event alignment and a real-time combined card (see INTEGRATION.md §2.1). Requires modifying CPAP firmware — opt-in, risk noted
- **[P2] Therapy → recovery trends** — HRV / deep-sleep / RHR vs. nightly AHI and CPAP usage over weeks
- **[P2] Mask-off / compliance automations** — "asleep but mask off" nudges, usage streaks, bedtime reminders (HA-side)

---

## Suggested build order (cross-reference PLAN.md phases)

| Tier | Contents | Lands in |
|---|---|---|
| P0 | Sections 1, 2, 3, 7 cores + basic alarm, watch face, morning report, power/battery basics | Phases 0–4 |
| P1 | Auto sleep detection, HRV, smart alarm, USB offload/streaming, tilt-to-wake, history, OTA | Phase 4–5 |
| P2 | Audio features, insights/correlations, BLE/Wi-Fi sync, EDF, AOD | Phase 5+ |
| Integration (§11) | HA/MQTT uplink → CPAP downlink → combined summary → cross-device insights | Phase 6 / v3+ (see INTEGRATION.md §7) |
| HW | Vibration motor, room climate sensor, lux sensor | Whenever the soldering iron is warm |
