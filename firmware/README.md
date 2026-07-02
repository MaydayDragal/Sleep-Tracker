# Sleep Tracker — Firmware

ESP-IDF firmware for the wrist sleep tracker on the **Waveshare ESP32-S3-Touch-AMOLED-1.8** (+ external MAX3010x PPG sensor: MAX30102 now → MAX30101 planned).

**Status (Phase 0 bring-up working):** the board boots on real hardware — all onboard I²C devices enumerate, the CO5300 AMOLED comes up, and LVGL renders. Board bring-up (display/touch/SD/PMU/expander) is delegated to Waveshare's **managed BSP component** (`waveshare/esp32_s3_touch_amoled_1_8`); the project's [`components/board/`](components/board/) is a thin seam over it. The sensor/DSP/UI *feature* bodies are still stubbed and tagged `TODO(phaseN)` per the milestones in [`../PLAN.md`](../PLAN.md). Touch is temporarily deferred — see the note under **Layout**.

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
- Two build workarounds are baked in and required to compile the managed BSP + `esp_lcd` on this toolchain:
  - `sdkconfig.defaults` sets `CONFIG_COMPILER_OPTIMIZATION_SIZE=y` (`-Os`) — the default `-Og` triggers a GCC 14.2 internal-compiler-error (IRA segfault) in `esp_lcd_panel_rgb.c`.
  - the top-level `CMakeLists.txt` appends `-Wno-error=format` — the managed Waveshare BSP logs a `uint32_t` with `%02X`, fatal under `-Werror=format`.
- The board's USB-C is the S3 **native USB-Serial/JTAG** (shows up as **COM6** here) — used for both flashing and logs (`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG`). If upload can't auto-enter download mode, hold **BOOT**, tap **RESET**, release BOOT, and retry.
- Update `upload_port`/`monitor_port` in `platformio.ini` if the board enumerates on a different COM port.

### Native ESP-IDF (alternative)

Requires [ESP-IDF **v5.3+**](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/get-started/) (the managed Waveshare BSP needs ≥5.3; we build on 5.4) exported (`. $IDF_PATH/export.sh`). Apply the same `-Os` / `-Wno-error=format` workarounds noted above:

```bash
cd firmware
idf.py set-target esp32s3     # applies sdkconfig.defaults (PSRAM, 16 MB flash, custom partitions)
idf.py build
idf.py -p <PORT> flash monitor
```

At this stage a build boots, brings up the shared I2C bus, and starts the two
tasks (`sensor` pinned to core 1, `ui` on core 0), logging stubs for the
not-yet-implemented pieces. That's the intended Phase 0 starting point.

## Layout

```
firmware/
├── CMakeLists.txt            # top-level project
├── sdkconfig.defaults        # esp32s3 + octal PSRAM + 16 MB flash + custom partitions + PM
├── partitions.csv            # app + storage (night logs go to microSD, not flash)
├── main/                     # app_main: dual-core task setup (sense | UI)
└── components/
    ├── board/               # board seam — delegates to the managed Waveshare BSP; ONLY board-specific code [phase0/2]
    ├── max30102/             # MAX3010x PPG driver (MAX30102→30101)    [phase1]
    ├── ppg/                  # filtering, beat detect, IBI/HR/SpO2/SQI [phase1]
    ├── actigraphy/           # QMI8658 wrist activity counts           [phase1]
    ├── bodynet/              # BLE central: WT9011DCL body sensors+H10 [phase2.5]
    ├── sleep_core/           # epoch record, session SM, HRV, scoring  [phase2-3]
    ├── ui/                   # LVGL screens (on board's display)       [phase0/4]
    └── sync/                 # BLE GATT log-pull (P5) → MQTT to HA+CPAP [phase5/integration]
```

> **Touch deferred (Phase-0 TODO):** the managed Waveshare BSP (v2.0.3) never releases the FT3168 touch reset via the TCA9554 IO-expander (both `LCD_RST` and `TOUCH_RST` are `NC`), so touch is mute at `0x38` and the vendor `bsp_display_start()` panics on it. `board_display_start()` therefore brings up the CO5300 display + LVGL directly (via `bsp_display_new()` + `esp_lvgl_port`) and skips touch. Re-enabling touch requires driving the correct TCA9554 reset pin — confirm its mapping from the schematic / Waveshare factory demo.

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
