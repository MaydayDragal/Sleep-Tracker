# Datasheets

Reference datasheets for the main ICs on the **Waveshare ESP32-S3-Touch-AMOLED-1.8** dev board and the project's external sensors. PDFs present in this folder are linked directly; the rest link to the manufacturer's page (some vendors — Analog Devices, X-Powers, Everest, FocalTech — block automated download, so grab those manually if you want local copies).

## Development board

| Item | Notes | Datasheet |
|---|---|---|
| Waveshare ESP32-S3-Touch-AMOLED-1.8 | Board overview, connectors, expansion pads | [board PDF](Waveshare_ESP32-S3-Touch-AMOLED-1.8_board.pdf) · [wiki](https://www.waveshare.com/wiki/ESP32-S3-Touch-AMOLED-1.8) |

## On-board ICs

| IC | Role | Bus / addr | Datasheet |
|---|---|---|---|
| **ESP32-S3R8** (Espressif) | MCU — dual-core LX7 @240 MHz, 8 MB PSRAM, 16 MB flash, Wi-Fi 4 + BLE 5 | — | [datasheet PDF](ESP32-S3_Espressif_datasheet.pdf) · [TRM](https://www.espressif.com/sites/default/files/documentation/esp32-s3_technical_reference_manual_en.pdf) |
| **CO5300** (⚠️ **on-hardware, not SH8601**) | 1.8" AMOLED display driver (368×448) | QSPI | driver: `espressif/esp_lcd_co5300` (via managed BSP) |
| **FT3168** (FocalTech) | Capacitive touch controller (some batches: FT6146) | I²C `0x38` | [FocalTech](https://www.focaltech-electronics.com/) · via board wiki |
| **TCA9554** (⚠️ **on-hardware, not in original docs**) | IO-expander — drives LCD/touch reset | I²C `0x20` | via managed BSP (`esp_io_expander_tca9554`) |
| **QMI8658** (QST) | 6-axis IMU (accel + gyro) — wrist actigraphy | I²C **`0x6B`** (SA0 high on this board) | [datasheet PDF](QMI8658_QST.pdf) · [QST](https://www.qstcorp.com/) |
| **PCF85063A** (NXP) | Real-time clock (backup-battery pads) | I²C `0x51` | [datasheet PDF](PCF85063A_NXP.pdf) · [NXP](https://www.nxp.com/products/PCF85063A) |
| **AXP2101** (X-Powers) | Power-management IC — charging, fuel gauge, rails | I²C `0x34` | [X-Powers](http://www.x-powers.com/en.php/Info/product_detail/article_id/95) |
| **ES8311** (Everest) | Low-power audio codec (mic + speaker) | I²C `0x18` / I²S | [Everest Semi](http://www.everest-semi.com/) |

## External sensors (this project)

| Part | Role | Bus / addr | Datasheet |
|---|---|---|---|
| **MAX30102** (Analog Devices) | PPG HR/SpO2 sensor — **current** (red+IR) | I²C `0x57` | [Analog Devices](https://www.analog.com/en/products/max30102.html) |
| **MAX30101** (Analog Devices) | PPG HR/SpO2 sensor — **planned** (adds green LED) | I²C `0x57` | [Analog Devices](https://www.analog.com/en/products/max30101.html) |
| **AST1041** (TinyCircuits) | Pulse-Oximetry **Wireling** carrier for the MAX30101 | I²C (5-pin Wireling) | [board PDF](AST1041_TinyCircuits_PulseOximetry_MAX30101_Wireling.pdf) · [product](https://tinycircuits.com/products/pulse-oximetry-sensor-wireling) |
| **WT9011DCL** (WitMotion) | BLE 9-axis IMU — body-position sensor (PLAN.md §2.4) | BLE 5.0 | [datasheet PDF](WT9011DCL_WitMotion.pdf) · [product](https://witmotion-sensor.com/products/wt9011dcl-bluetooth5-0-compact-size-accelerometer-inclinometer-sensor) |
| **Polar H10** | Chest ECG — HRV ground-truth reference (VALIDATION.md §3) | BLE (HR service `0x180D`) | [Polar](https://www.polar.com/en/sensors/h10-heart-rate-sensor) |

## Candidate sensor upgrades (optional, discussed but not adopted)

| Part | Why | Interface | Datasheet |
|---|---|---|---|
| **MAXM86161** (Analog Devices) | Integrated wrist/ear optical module — better ambient rejection, stays on I²C | I²C | [Analog Devices](https://www.analog.com/en/products/maxm86161.html) |
| **MAX86141** (Analog Devices) | Wrist-grade AFE — dual photodiode (best motion rejection) | SPI | [Analog Devices](https://www.analog.com/en/products/max86141.html) |

---

**I²C bus map — confirmed by on-board scan (Phase 0, 2026-07-02):** ES8311 `0x18` · **TCA9554 `0x20`** · AXP2101 `0x34` · FT3168 `0x38` *(touch — released via TCA9554 pins EXIO0–2 at display start)* · PCF85063 `0x51` · MAX30101/30102 `0x57` *(external — MAX30102 now attached & verified)* · QMI8658 **`0x6B`**. The boot-time scan runs before the touch reset, so it shows six devices (`0x18/0x20/0x34/0x51/0x57/0x6B`); the FT3168 joins at `0x38` once `board_display_start()` pulses the expander reset. The display is a **CO5300** over QSPI (some early Waveshare material named an SH8601; the CO5300 is what's on this board revision).

**Note on the AST1041 (MAX30101 Wireling):** the current driver **polls** the FIFO (the INT line is unused on this board). If the 5-pin Wireling connector breaks out the MAX30101 **INT** line, it would enable an optional interrupt-driven, lower-power read path; if not exposed, that would mean soldering to the chip's INT pad.
