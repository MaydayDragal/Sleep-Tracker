# Sleep Tracker — Firmware

ESP-IDF firmware for the wrist sleep tracker on the **Waveshare ESP32-S3-Touch-AMOLED-1.8** (+ external MAX30102). This is a **scaffold**: the project structure, build config, and component interfaces are in place; the driver/DSP/UI bodies are stubbed and tagged with `TODO(phaseN)` matching the milestones in [`../PLAN.md`](../PLAN.md).

## Prerequisites

- [ESP-IDF **v5.x**](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/get-started/) installed and exported (`. $IDF_PATH/export.sh`).

## Build / flash / monitor

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
    ├── bsp/                  # board pins/buses/display/touch/PMU/RTC/SD — the ONLY board-specific code [phase0/2]
    ├── max30102/             # PPG sensor driver (FIFO + INT)          [phase1]
    ├── ppg/                  # filtering, beat detect, IBI/HR/SpO2/SQI [phase1]
    ├── actigraphy/           # QMI8658 wrist activity counts           [phase1]
    ├── bodynet/              # BLE central: WT9011DCL body sensors+H10 [phase2.5]
    ├── sleep_core/           # epoch record, session SM, HRV, scoring  [phase2-3]
    ├── ui/                   # LVGL screens (on bsp's display/touch)   [phase0/4]
    └── sync/                 # BLE GATT log-pull (P5) → MQTT to HA+CPAP [phase5/integration]
```

## Board abstraction (S3 ↔ C6)

All board-specific pin and peripheral definitions live in `components/bsp`. The
ESP32-C6 variant is the documented power-optimization fallback (PLAN.md §2); a
swap should be confined to `bsp` plus a `sdkconfig` target change.

> **TODO(phase0):** the GPIO numbers in `components/bsp/include/bsp.h` are
> placeholders. Confirm them against the **Rev1.1 schematic** in the
> [Waveshare board repo](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.8)
> before running on hardware.

## Managed dependencies (added as phases land)

Not pulled yet, to keep the scaffold building offline. When implementing the UI:

```bash
idf.py add-dependency "lvgl/lvgl^9"
idf.py add-dependency "espressif/esp_lcd_sh8601"
# + a touch driver component for FT3168/FT6146
```

then add them to the relevant component's `REQUIRES`.
