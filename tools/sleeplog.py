#!/usr/bin/env python3
"""Shared helpers for the Sleep-Tracker offline tools.

Single source of truth for the on-SD night-log schema so the generator, the
scorer, and any future tool agree with the firmware. The column list and flag
bits MUST match components/sd_logger/sd_logger.c and
components/sleep_core/include/sleep_core.h.
"""
from __future__ import annotations
import sys

# Exact CSV column order the firmware writes (sd_logger.c CSV_HEADER).
COLUMNS = [
    "seq", "t_unix", "activity", "body_position", "body_activity",
    "hr_mean", "hr_min", "hr_max", "rmssd_ms", "spo2_pct", "sqi",
    "batt_pct", "vbat_mv", "beat_accept", "flags",
]

EPOCH_SECONDS = 30

# Flag bits — must match SLEEP_FLAG_* in sleep_core.h.
FLAG_WRIST_OFF    = 1 << 0
FLAG_MOTION       = 1 << 1
FLAG_DESAT        = 1 << 2   # SpO2 dipped below 90 %
FLAG_SENSOR_LOST  = 1 << 3
FLAG_NO_PPG       = 1 << 4
FLAG_NO_CARDIAC   = 1 << 5   # no usable HR/SpO2 this epoch (any cause)
FLAG_RTC_UNSYNCED = 1 << 6
FLAG_SQI_PROXY    = 1 << 7
FLAG_HRV_VALID    = 1 << 8   # rmssd_ms is from a qualified clean window
FLAG_BATT_INVALID = 1 << 9

FLAG_NAMES = [
    (FLAG_WRIST_OFF, "WRIST_OFF"), (FLAG_MOTION, "MOTION"),
    (FLAG_DESAT, "DESAT"), (FLAG_SENSOR_LOST, "SENSOR_LOST"),
    (FLAG_NO_PPG, "NO_PPG"), (FLAG_NO_CARDIAC, "NO_CARDIAC"),
    (FLAG_RTC_UNSYNCED, "RTC_UNSYNCED"), (FLAG_SQI_PROXY, "SQI_PROXY"),
    (FLAG_HRV_VALID, "HRV_VALID"), (FLAG_BATT_INVALID, "BATT_INVALID"),
]

# body_position_t (bodynet.h). 0 = unknown until Phase 2.5 body sensors land.
POSITIONS = {0: "unknown", 1: "back", 2: "left", 3: "right", 4: "belly", 5: "upright"}
# Positions counted as "in bed / lying" for position breakdowns.
LYING_POSITIONS = {1, 2, 3, 4}

CARDIAC_COLS = ["hr_mean", "hr_min", "hr_max", "rmssd_ms", "spo2_pct", "beat_accept"]


def decode_flags(flags: int) -> str:
    return "|".join(name for bit, name in FLAG_NAMES if flags & bit) or "-"


def load_epochs(path: str):
    """Load a night CSV into a cleaned DataFrame (pandas). Robust to a torn final
    line from a mid-append power cut and to a spliced-in header row."""
    try:
        import pandas as pd
    except ImportError:
        sys.exit("These tools need pandas:  py -m pip install pandas")

    df = pd.read_csv(path, engine="python", on_bad_lines="skip")
    df = df.apply(pd.to_numeric, errors="coerce")
    df = df.dropna(subset=["seq", "t_unix", "flags"]).reset_index(drop=True)
    if df.empty:
        sys.exit(f"{path}: no epoch rows (empty or header-only log)")
    for c in COLUMNS:
        if c in df.columns:
            df[c] = df[c].astype("Int64")

    no_cardiac = (df["flags"].astype(int) & FLAG_NO_CARDIAC) != 0
    for col in CARDIAC_COLS:
        if col in df.columns:
            df.loc[no_cardiac, col] = pd.NA
    # spo2 == 0 is a "not measured" sentinel, never a real reading.
    if "spo2_pct" in df.columns:
        df.loc[df["spo2_pct"] == 0, "spo2_pct"] = pd.NA
    return df


def write_epochs(rows, path: str) -> None:
    """Write rows (list of dicts keyed by COLUMNS) as the firmware CSV."""
    with open(path, "w", newline="") as f:
        f.write(",".join(COLUMNS) + "\n")
        for r in rows:
            f.write(",".join(str(int(r[c])) for c in COLUMNS) + "\n")
