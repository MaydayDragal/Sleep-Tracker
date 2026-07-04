# tools/ — offline analysis for Sleep-Tracker

Python tooling for two offline jobs: (1) read/validate the night logs the
firmware writes to `/sdcard/sleeptrk/YYYYMMDD_HHMMSS.csv` (see
`components/sd_logger`) and prototype the **Phase-3 sleep scoring** (PLAN.md §3)
against real *and* synthetic nights — so the algorithm can be built and measured
before the hardware overnight run exists; and (2) capture raw PPG over USB and
develop the beat detector offline against NeuroKit2 before porting the winner to
the on-device `components/ppg`.

## Requirements

Two tool groups, two dependency sets:

```
# Night-log + scoring scripts (sleeplog, read_night, gen_synthetic_night, score_night)
py -m pip install pandas          # Windows launcher; or python3 -m pip install pandas

# PPG-tuning loop (capture_ppg, analyze_ppg)
py -m pip install pyserial numpy scipy neurokit2
```

(pandas is an optional import in `sleeplog`/`read_night`/`analyze_ppg` — they degrade gracefully if it's absent.)

## Contents

| File | What it does |
|---|---|
| `sleeplog.py` | Shared schema: the 14-column CSV layout, flag bits, position enum, and a robust loader. Single source of truth mirroring `sd_logger.c` + `sleep_core.h`. |
| `read_night.py` | Quick validator/summariser — "does the log open cleanly?" (the Phase-2 exit-gate check). Tolerates a torn final line from a mid-append power cut. |
| `read_raw_ppg.py` | Parses the raw-PPG binary side-file (`*_ppg.bin`) the firmware writes per recording — one block per PPG window, buffered in PSRAM then flushed to SD to spare card wear. Summarises blocks/samples; `--csv` dumps `t_unix,red,ir`. Skips a torn trailing block from a mid-flush power cut. |
| `gen_synthetic_night.py` | Generates a realistic night in the exact firmware schema; `--truth` emits the **ground-truth hypnogram** and `--hrv` populates the optional RMSSD/HRV_VALID columns, so the scorer's accuracy is measurable without a real overnight run. |
| `score_night.py` | Prototype of the Phase-3 morning re-pass: sleep/wake, staging, metrics, a 0–100 sleep score, an SVG hypnogram, and validation against ground truth. **Offline only — not yet ported on-device.** |
| `capture_ppg.py` | Captures the firmware's `R,<ir>,<red>` raw-PPG USB stream (enabled by `PPG_RAW_STREAM=1` in `main`) for a window into an `ir,red` CSV that `analyze_ppg.py` consumes. |
| `analyze_ppg.py` | Offline PPG analysis: band-pass + beat detection via **scipy and NeuroKit2**, HR, HRV (RMSSD), and a plot; `--demo` runs a synthetic sanity check. The offline half of the PPG-accuracy loop — develop/validate the beat detector here, then port to `components/ppg`. |

## Quickstart

```bash
# Generate a synthetic night + its ground-truth hypnogram
py gen_synthetic_night.py --out night.csv --truth truth.csv --seed 1

# Score it, render a hypnogram, and validate against the truth
py score_night.py night.csv --svg hypno.svg --truth truth.csv

# Or just inspect a real night's log
py read_night.py 20260702_233000.csv
```

## The scoring pipeline (`score_night.py`)

1. **Sleep/wake** — a Cole-Kripke-style weighted moving average over the dense
   wrist actigraphy `activity` column, thresholded (`--wake-threshold`).
2. **Staging** — light / deep / REM-estimate from HR relative to the night's
   sleeping baseline (`--deep-hr`, `--rem-hr`) plus movement (`--deep-act`,
   `--rem-act`), then bout-smoothed; early-night REM is suppressed. HR is
   forward-filled because PPG is duty-cycled (~one sample / 5 min).
3. **Metrics** — TST, efficiency, onset latency, WASO, awakenings, resting HR,
   SpO2 min/mean, ODI (desat events/hour), and a position breakdown with
   position-segmented HR/SpO2 (the groundwork for the positional-apnea
   correlation in INTEGRATION.md §6).
4. **Sleep score** — 0–100, blending duration, efficiency, continuity, and
   deep/REM restoration (weights documented in `sleep_score()`).

On the synthetic nights this currently lands around **~98% sleep/wake** and
**~85–90% 4-stage** agreement with ground truth.

## ⚠️ Thresholds must be tuned on real nights

The staging thresholds are **synthetic-tuned placeholders**. Actigraphy counts
are device-specific, so the CLI knobs (`--wake-threshold`, `--deep-*`,
`--rem-*`) must be re-tuned once real recorded nights exist (the Phase-3 "tune on
recorded nights" task). The `--truth` validation loop is exactly the harness for
that. Once tuned, this logic ports to the on-device `sleep_core` morning re-pass.
