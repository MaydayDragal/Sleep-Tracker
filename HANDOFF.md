# Handoff — Sleep-Tracker

*Snapshot for the next session. Updated 2026-07-03 (after the docs-cleanup pass). Convert relative dates before trusting them.*

## TL;DR — where the project is

A wrist-worn sleep tracker on the Waveshare ESP32-S3-Touch-AMOLED-1.8 + external MAX30102 PPG. **Through Phase 4 (UI shell) on hardware**, with Phase 2 recording bench-validated over USB and Phase 3 scoring existing only offline. All prior PRs are merged to `main`; the working tree is clean.

- **Phases 0–1** — done + hardware-validated (boot, I2C enumerate, CO5300+LVGL+touch, RTC/IMU/battery, live HR/SpO2 + rolling RMSSD).
- **Phase 2 (recording)** — bench-validated **over USB** (SD mounts, 30 s epochs write, tickless light-sleep engages). **The "8 h on battery" exit gate is still OPEN.**
- **Phase 3 (scoring)** — **offline prototype only** (`tools/score_night.py`, ~98% sleep/wake on synthetic nights). On-device scoring is a `TODO(phase3)` in `sleep_core.c`.
- **Phase 4 (UI)** — 11-tile LVGL watch app runs on hardware; 4 tiles are live, 5 render a baked-in **sample** night.
- **Phase 2.5 (`bodynet`)** and **Phase 5 (`sync`)** — stubs.

Read [PLAN.md §7 "Current status"](PLAN.md) for the authoritative per-phase state, and [firmware/README.md](firmware/README.md) for the firmware/UI detail.

## ⚠️ Read before starting (conventions — enforced in [CLAUDE.md](CLAUDE.md))

1. **`git fetch origin main` and branch off `origin/main`, not local `main`.** PRs merge on GitHub and the local clone isn't auto-pulled — it silently falls many PRs behind. (This bit us: a whole feature was built on a stale base and had to be rebased.)
2. **Keep [`docs/watch-ui-mockup.html`](docs/watch-ui-mockup.html) in sync** whenever `firmware/components/ui/**` changes; commit the mockup in the same PR. A committed Stop hook (`.claude/settings.json`) nags on drift.

## Component status (what's real vs stub)

| Component | State |
|---|---|
| `board` `max30102` `ppg` `actigraphy` `rtc` `pmu` `power` `sd_logger` `nettime` | **Real**, on hardware. `ppg` = despike + band-pass + adaptive beat detect + **real composite SQI** + live rolling-window RMSSD. ACTIVE runs continuously at a **user-selectable PPG rate (50/100/200/400/800 Hz, Settings)** — no auto HR/HRV switching. `sd_logger` writes the 15-col epoch CSV (now incl. **battery voltage `vbat_mv`** — trustworthy vs the 0% gauge) **plus raw PPG to SD**, PSRAM-buffered, one block per PPG window (`*_ppg.bin`; reader `tools/read_raw_ppg.py`). `nettime` sets the RTC from NTP at boot (brief WiFi, then off; creds in a gitignored `wifi_config.h`). |
| `sleep_core` | **Real** epoch assembler + button-driven session FSM + per-epoch RMSSD. **On-device scoring = `TODO(phase3)`** (`do_stop()`). |
| `ui` | **Real** 11-tile watch UI + **on-screen Start/Stop tracking button** + HR-sample-rate setting + dev-only PPG-debug tile + display-sleep/double-tap-wake. Report/position/history tiles are **sample data**. |
| `bodynet` | **Stub** (Phase 2.5). CSV `body_position`/`body_activity` columns are always 0. |
| `sync` | **Stub** (Phase 5). |

## Next steps (priority order)

1. **Close the Phase-2 "8 h on battery" gate (hardware-blocked).** Confirm a Li-Po is actually on the MX1.25 header — the AXP2101 gauge reads **0%** (flagged `BATT_INVALID`), which most likely means no cell attached. Then run a real overnight session and spikes **S3/S4** (SD throughput + overnight mAh via an *external* coulomb counter — not the AXP2101 model gauge). See [VALIDATION.md](VALIDATION.md).
2. **Port the offline scorer on-device.** Move `tools/score_night.py`'s pipeline into `sleep_core`'s morning re-pass (`do_stop()` TODO), tune the synthetic-tuned thresholds on real recorded nights, and wire the **report/position/history UI tiles to real SD logs** (replacing the sample night). This is the biggest single UX unlock.
3. **Phase 2.5 `bodynet`.** NimBLE central for the WT9011DCL torso sensor → authoritative sleep position; fills the always-0 CSV columns and unblocks position-segmented metrics.

## Hardware ground truth & gotchas

- **Flash:** `pio run -d firmware -e esp32s3-amoled -t upload -t monitor` (PlatformIO CLI at `C:\Users\Mayday\.platformio\penv\Scripts\platformio.exe`). Board = **COM6** (native USB-Serial/JTAG). If writes time out / the port flaps → **swap the USB-C cable first** (a bad cable has cost hours).
- **Build:** after editing `sdkconfig.defaults`, run `rm firmware/sdkconfig.esp32s3-amoled` and rebuild — the LVGL font + `CONFIG_LV_USE_CLIB_MALLOC` symbols only take effect on a clean regen. Do **not** bump PlatformIO to platform 7.0.x. Details in [firmware/README.md](firmware/README.md).
- **PPG is finger-on-sensor, not wrist-validated** — motion artifact / ambient-light leak at the wrist is the #1 open data-quality risk. LED current is finger-tuned; the MAX30101 (adds a green LED for wrist PPG) is planned but not in hand.
- **Wake-on-motion is dropped** (QMI8658 INT not routed on this board) — TRACKING uses timer-wake only. Auto light-sleep on battery is unverified (SDMMC-PM-lock question).
- **Session trigger = on-screen Start/Stop button** (a Tracking-tile toggle + a watch-face shortcut), wired to `sleep_core_request_start/stop()`. Recording keeps LVGL/touch live (panel blanks, double-tap to wake) so the Stop button stays reachable — the deep LVGL-stop light-sleep path is deferred (it needs a hardware wake source to coexist with an on-screen button). Automatic sleep detection is the later step.

## Still-open questions

Strap/enclosure design; confirming the WT9011DCL BLE service/char UUIDs + `0x61` frame; the MAX30101 swap when it arrives. (Raw-PPG-to-SD is now built — during a recording the firmware buffers raw samples in PSRAM and flushes one block per PPG window to `<ts>_ppg.bin`; reader `tools/read_raw_ppg.py`. Raw PPG also still streams over USB in ACTIVE for live tuning.)

## Where things live

- **Roadmap + status:** [PLAN.md](PLAN.md) · **Firmware/build/UI:** [firmware/README.md](firmware/README.md) · **Offline scorer + PPG loop:** [tools/README.md](tools/README.md) · **De-risking spikes:** [VALIDATION.md](VALIDATION.md) · **Feature backlog:** [FEATURES.md](FEATURES.md) · **HA/CPAP (v3+):** [INTEGRATION.md](INTEGRATION.md) · **UI design ref:** [docs/watch-ui-mockup.html](docs/watch-ui-mockup.html) · **Conventions:** [CLAUDE.md](CLAUDE.md).
