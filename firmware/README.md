# Sleep Tracker — Firmware

ESP-IDF firmware for the wrist sleep tracker on the **Waveshare ESP32-S3-Touch-AMOLED-1.8** (+ external MAX3010x PPG sensor: MAX30102 now → MAX30101 planned).

**Status — Phases 0-1 done + hardware-validated; Phase 2 recording bench-validated over USB; Phase 4 UI shell on hardware; on-device scoring still offline:** the board boots on real hardware — all onboard I²C devices enumerate, the CO5300 AMOLED comes up, LVGL renders, and touch works (the vendor BSP omits the FT3168 reset, so `board/` releases it via the TCA9554 expander — see the note under **Layout**). Board bring-up delegates to Waveshare's **managed BSP component** (`waveshare/esp32_s3_touch_amoled_1_8`); [`components/board/`](components/board/) is a thin seam over it. The Phase 1 sensors are live: the PCF85063 RTC ticks, the QMI8658 reads ~1 g at rest, the AXP2101 reports battery, and the external MAX30102 yields live heart rate (~81 bpm) + SpO2 (98–99%) plus a live rolling-window RMSSD. **Phase 2 recording is bench-validated over USB on ESP-IDF v6.0.2:** the microSD mounts, an on-screen **Start** opens the log, and continuous 30 s epochs write to the card. In TRACKING the panel blanks (the dominant load) but LVGL/touch stay live so the on-screen **Stop** button remains reachable — so the CPU stays full-speed (dropping into DFS/light-sleep glitched the still-live QSPI panel back on mid-session); deep CPU sleep is deferred, needing a hardware wake source. The *8 h on-battery* overnight run is still open (no Li-Po attached; the AXP2101 gauge reads 0%).

**UI (Phase 4):** an **11-tile swipeable LVGL app** (`components/ui`) — watch face (with a **Start-tracking** shortcut), live vitals (HR / SpO2 / real HRV / SQI + a **live PPG pulse waveform**), a tracking tile carrying the **Start/Stop button**, score, hypnogram, heart & O2, position, history, alarm, a scrollable settings list, and a dev-only **PPG-debug tile** (tuning graph + rate/HR/HRV/SQI) — plus **display-sleep + double-tap-to-wake**. The watch / live-vitals / tracking / PPG-debug tiles read live sensors; the score / hypnogram / heart & O2 / position / history tiles render a baked-in **sample** night pending on-device scoring (a `TODO(phase3)` in `sleep_core` — an offline prototype in `tools/score_night.py`) and Phase 2.5 body sensors. `bodynet` and `sync` remain stubs. Milestones: [`../PLAN.md`](../PLAN.md); interactive design reference: [`../docs/watch-ui-mockup.html`](../docs/watch-ui-mockup.html).

## Build / flash / monitor

This project builds under **PlatformIO** (recommended, via VS Code) *or* native **ESP-IDF** — both use the same `main/` + `components/` tree.

### PlatformIO (VS Code) — recommended

1. Install the **PlatformIO IDE** extension in VS Code.
2. Open the **`firmware/`** folder as the PlatformIO project (it contains `platformio.ini`).
3. Use the PlatformIO toolbar / status bar: **Build** (✓), **Upload** (→), **Monitor** (🔌). Or from a terminal:

```powershell
pio run                    # build
pio run -t upload          # flash (upload_port = COM6 in platformio.ini)
pio device monitor         # serial monitor @ 115200
```

- `platformio.ini` pins `platform = espressif32@6.11.0` (**ESP-IDF 5.4.1 / GCC 14.2**), `board = esp32-s3-devkitc-1`, 16 MB flash, and the `partitions.csv` table; PSRAM/CPU/PM come from `sdkconfig.defaults`. **Do not bump to platform 7.0.x** (IDF 6.0.1 / GCC 15.2) — it hits a compiler ICE building ESP-IDF's `esp_lcd` RGB driver.
- Build workarounds baked in and required to compile the managed BSP + `esp_lcd` on this toolchain:
  - `sdkconfig.defaults` sets `CONFIG_COMPILER_OPTIMIZATION_SIZE=y` (`-Os`) — the default `-Og` triggers a GCC 14.2 internal-compiler-error (IRA segfault) in `esp_lcd_panel_rgb.c`.
  - the top-level `CMakeLists.txt` appends `-Wno-error=format` — the managed Waveshare BSP logs a `uint32_t` with `%02X`, fatal under `-Werror=format`.
- LVGL config the watch UI depends on (also in `sdkconfig.defaults`):
  - larger fonts `CONFIG_LV_FONT_MONTSERRAT_12/20/28/36/48=y` — the default is only 14 (a big clock/score needs these, else `lv_font_montserrat_48 undeclared`).
  - `CONFIG_LV_USE_CLIB_MALLOC=y` — the default builtin LVGL pool is only 64 KB (`CONFIG_LV_MEM_SIZE_KILOBYTES=64`) and the ~200-widget UI **exhausts it mid-build** → `lv_malloc` returns NULL and `lv_obj_add_style` hangs → task-watchdog reboot loop. CLIB malloc routes LVGL to the system heap (and frees ~64 KB of static RAM).
  - ⚠️ these are choice/dependency-gated Kconfig symbols — after editing `sdkconfig.defaults`, run **`rm firmware/sdkconfig.esp32s3-amoled`** and rebuild so they actually take effect (a plain rebuild silently keeps the old values).
- The board's USB-C is the S3 **native USB-Serial/JTAG** (shows up as **COM6** here) — used for both flashing and logs (`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG`). If upload can't auto-enter download mode, hold **BOOT**, tap **RESET**, release BOOT, and retry.
- Update `upload_port`/`monitor_port` in `platformio.ini` if the board enumerates on a different COM port.

### Native ESP-IDF (alternative)

Requires [ESP-IDF **v5.3+**](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/get-started/) (the managed Waveshare BSP needs ≥5.3). Verified building, flashing, and running on **5.4** and on **v6.0.2** (native, via the ESP-IDF Installation Manager / EIM, GCC 15.2) — on 6.0.2 the `-Os` workaround below still applies, and the RGB-driver ICE the PlatformIO note flags for 6.0.1 did **not** occur on native 6.0.2. Export the environment (`. $IDF_PATH/export.sh`, or `export.ps1` on Windows), then apply the same `-Os` / `-Wno-error=format` workarounds noted above:

```bash
cd firmware
idf.py set-target esp32s3     # applies sdkconfig.defaults (PSRAM, 16 MB flash, custom partitions)
idf.py build
idf.py -p <PORT> flash monitor
```

A build boots, brings up the shared I2C bus, and starts the two tasks (`sensor`
on core 1, `ui` on core 0): the sensor task reads the RTC, IMU, battery, and
MAX30102 live and drives the LVGL watch UI + SD epoch logging, while `bodynet`
(Phase 2.5) and `sync` (Phase 5) still log stubs.

## Layout

```
firmware/
├── CMakeLists.txt            # top-level project
├── sdkconfig.defaults        # esp32s3 + octal PSRAM + 16 MB flash + custom partitions + PM
├── partitions.csv            # app + storage (night logs go to microSD, not flash)
├── main/                     # app_main: dual-core task setup (sense | UI)
└── components/
    ├── board/                # board seam over the managed Waveshare BSP; ONLY board-specific code   [phase0/2]
    ├── max30102/             # MAX3010x PPG driver (MAX30102 → 30101)                                 [phase1]
    ├── ppg/                  # despike + band-pass, beat detect, IBI/HR/SpO2/SQI, live RMSSD          [phase1]
    ├── actigraphy/           # QMI8658 wrist activity counts (accel-only)                             [phase1]
    ├── rtc/                  # PCF85063A real-time clock driver                                       [phase0/1]
    ├── pmu/                  # AXP2101 PMU: VBUS / charge state / battery % + voltage                  [phase0/1]
    ├── power/                # ACTIVE/TRACKING: panel off + UI-gate (CPU full-speed; deep sleep deferred) [phase2]
    ├── sleep_core/           # epoch assembler + session FSM + per-epoch RMSSD (scoring = TODO phase3) [phase2/3]
    ├── sd_logger/            # crash-safe 15-col CSV logger + PSRAM-buffered raw-PPG side-file         [phase2]
    ├── nettime/              # transient WiFi + SNTP → set RTC at boot, then WiFi off                 [phase2]
    ├── bodynet/              # BLE central: WT9011DCL body sensors + H10 — STUB                       [phase2.5]
    ├── ui/                   # LVGL watch UI: 11-tile swipeable app                                   [phase0/4]
    └── sync/                 # BLE GATT log-pull → MQTT to HA + CPAP — STUB                           [phase5]
```

> **Why `board/` doesn't use the vendor `bsp_display_start()`:** the managed Waveshare BSP (v2.0.3) never releases the FT3168 touch reset via the TCA9554 IO-expander (both `LCD_RST` and `TOUCH_RST` are `NC`), so touch is mute at `0x38` and the vendor `bsp_display_start()` `ESP_ERROR_CHECK`s touch init → panic loop. `board_display_start()` instead: (1) pulses TCA9554 pins `EXIO0–2` low→high to release the LCD/touch resets (matching Waveshare's own FT3168 example), (2) brings up the CO5300 + LVGL directly via `bsp_display_new()` + `esp_lvgl_port`, and (3) adds touch **non-fatally** (a probe miss degrades to display-only instead of a boot loop). Result: display **and** touch work.

## Time / NTP (optional)

The RTC is set at boot from an NTP server if WiFi is configured, then WiFi is turned back off (it is not kept running). To enable it, copy [`components/nettime/wifi_config.example.h`](components/nettime/wifi_config.example.h) → `components/nettime/wifi_config.h` (gitignored) and fill in your **2.4 GHz** SSID/password and `SLEEPTRK_UTC_OFFSET_MIN` (local = UTC + minutes). Without that file NTP is skipped and the RTC keeps its battery-backed time (or the built-in seed date on a cold start). The clock is a naive wall clock (`tz_offset_min=0` in the log header), so the UTC offset just makes the watch face show local time.

> **Critical ordering:** `nettime_fetch()` runs at the very start of `app_main()`, **before `board_init()` brings up the I2C bus** — WiFi's interrupt latency will glitch an in-flight I2C transaction and *wedge the shared bus* (which hangs the display flush and kills touch) if the two ever overlap. WiFi must be fully done and off before any I2C device is touched; `nettime_fetch()` returns the time as a value and the sensor task writes it to the RTC afterward. (This bit us once — a concurrent WiFi/I2C bring-up froze the UI with a `taskLVGL` watchdog on `wait_for_flushing`.)

## Board abstraction (S3 ↔ C6)

All board-specific code lives in `components/board`, which delegates to the
managed Waveshare BSP. The ESP32-C6 variant is the documented power-optimization
fallback (PLAN.md §2); a swap should be confined to `board` plus a `sdkconfig`
target change. GPIO/peripheral pins are no longer hand-maintained here — they
come from the managed BSP and were confirmed on hardware by the boot-time I²C
scan in `board_init()` (see [`components/board/include/board.h`](components/board/include/board.h) for the address map).

## Managed dependencies

The board component pulls Waveshare's BSP from the Espressif Component Registry
via [`components/board/idf_component.yml`](components/board/idf_component.yml):

```yaml
dependencies:
  waveshare/esp32_s3_touch_amoled_1_8: "^2.0.3"
```

That transitively brings LVGL 9, `esp_lvgl_port`, the CO5300 display driver
(`esp_lcd_co5300`), FT/CST touch drivers, and the TCA9554 expander driver. The
component manager fetches them into `managed_components/` on build (git-ignored);
`dependencies.lock` pins the resolved versions. No manual `add-dependency` needed.
