# tools/ — offline analysis for Sleep-Tracker night logs

Python tooling for the night logs the firmware writes to
`/sdcard/sleeptrk/YYYYMMDD_HHMMSS.csv` (see `components/sd_logger`). This is the
offline half of the plan: read logs, and prototype the **Phase-3 sleep scoring**
(PLAN.md §3) against real *and* synthetic nights — so the algorithm can be built
and measured before the hardware overnight run exists.

## Requirements

Python 3 + pandas:

```
py -m pip install pandas       # Windows launcher; or python3 -m pip install pandas
```

## Contents

| File | What it does |
|---|---|
| `sleeplog.py` | Shared schema: the 14-column CSV layout, flag bits, position enum, and a robust loader. Single source of truth mirroring `sd_logger.c` + `sleep_core.h`. |
| `read_night.py` | Quick validator/summariser — "does the log open cleanly?" (the Phase-2 exit-gate check). Tolerates a torn final line from a mid-append power cut. |
| `gen_synthetic_night.py` | Generates a realistic night in the exact firmware schema, and can emit the **ground-truth hypnogram** so the scorer's accuracy is measurable. |
| `score_night.py` | Prototype of the Phase-3 morning re-pass: sleep/wake, staging, metrics, a 0–100 sleep score, an SVG hypnogram, and validation against ground truth. |

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
