#!/usr/bin/env python3
"""Read a Sleep-Tracker night log (the CSV written by components/sd_logger).

The firmware writes one header row + one row per 30 s epoch to
    /sdcard/sleeptrk/YYYYMMDD_HHMMSS.csv
and a flight recorder to
    /sdcard/sleeptrk/events.log   (t_unix,kind,detail ; '#'-prefixed = comment)

This is the offline half of the Phase-2 exit gate ("opens cleanly in a
notebook/spreadsheet"). It is deliberately tiny — pandas does the parsing; a
torn final line from a power cut mid-append is simply skipped.

    python read_night.py 20260702_233000.csv
    python read_night.py 20260702_233000.csv --clean cleaned.csv

Note on time: t_unix is the RTC wall clock treated as UTC (tz_offset_min=0 in the
event header is our honesty marker). We render it as *naive* wall-clock time — we
do NOT claim it is real UTC, so no timezone conversion is applied.
"""
import argparse
import sys
from datetime import datetime, timezone

try:
    import pandas as pd
except ImportError:
    sys.exit("This tool needs pandas:  pip install pandas")

# Must match enum in components/sleep_core/include/sleep_core.h
FLAG_NAMES = [
    (1 << 0, "WRIST_OFF"),
    (1 << 1, "MOTION"),
    (1 << 2, "DESAT"),
    (1 << 3, "SENSOR_LOST"),
    (1 << 4, "NO_PPG"),
    (1 << 5, "NO_CARDIAC"),
    (1 << 6, "RTC_UNSYNCED"),
    (1 << 7, "SQI_PROXY"),
    (1 << 8, "HRV_VALID"),
    (1 << 9, "BATT_INVALID"),
]
NO_CARDIAC = 1 << 5

# Columns that are meaningless when NO_CARDIAC is set (PPG was off / no signal).
CARDIAC_COLS = ["hr_mean", "hr_min", "hr_max", "rmssd_ms", "spo2_pct", "sqi", "beat_accept"]


def decode_flags(flags: int) -> str:
    return "|".join(name for bit, name in FLAG_NAMES if flags & bit) or "-"


def naive_wall(t_unix: int) -> str:
    # Render the "wall clock treated as UTC" value back as wall clock, without
    # asserting a real timezone.
    return datetime.fromtimestamp(int(t_unix), tz=timezone.utc).strftime("%Y-%m-%d %H:%M:%S")


def main() -> None:
    ap = argparse.ArgumentParser(description="Read a Sleep-Tracker night CSV log.")
    ap.add_argument("csv", help="path to a YYYYMMDD_HHMMSS.csv epoch log")
    ap.add_argument("--clean", metavar="OUT", help="write a cleaned CSV (cardiac cols nulled where NO_CARDIAC)")
    args = ap.parse_args()

    # engine='python' + on_bad_lines='skip' drops a torn final line safely.
    df = pd.read_csv(args.csv, engine="python", on_bad_lines="skip")
    # Coerce all columns to numeric and drop any non-numeric rows — this discards
    # a spliced-in header row (from the rare night.csv "a" fallback) so the grid
    # math below can't crash on it.
    df = df.apply(pd.to_numeric, errors="coerce")
    df = df.dropna(subset=["seq", "t_unix", "flags"]).reset_index(drop=True)
    if df.empty:
        sys.exit("no epoch rows (empty or header-only log)")

    # Grid invariant: t_unix == first_t_unix + 30 * seq  (per file).
    t0 = int(df["t_unix"].iloc[0])
    expected = t0 + 30 * df["seq"].astype(int)
    gaps = int((df["t_unix"].astype(int) != expected).sum())

    # Null cardiac columns where there was no usable cardiac data.
    no_cardiac = (df["flags"].astype(int) & NO_CARDIAC) != 0
    for col in CARDIAC_COLS:
        if col in df.columns:
            df.loc[no_cardiac, col] = pd.NA
    # spo2_pct == 0 is a "not measured" sentinel (SpO2 is never legitimately 0),
    # not a real reading — null it so it can't drag the min/mean down.
    df.loc[df["spo2_pct"] == 0, "spo2_pct"] = pd.NA

    dur_min = (int(df["t_unix"].iloc[-1]) - t0 + 30) / 60.0
    cardiac = df[~no_cardiac]

    print(f"file        : {args.csv}")
    print(f"start       : {naive_wall(t0)}  (wall clock, tz-naive)")
    print(f"epochs      : {len(df)}  (~{dur_min:.1f} min)")
    print(f"grid gaps   : {gaps}  (rows where t_unix != t0 + 30*seq)")
    print(f"cardiac eps : {len(cardiac)}  ({100*len(cardiac)/len(df):.0f}% had PPG data)")
    if not cardiac.empty:
        print(f"HR          : mean {cardiac['hr_mean'].mean():.0f}  "
              f"min {cardiac['hr_min'].min():.0f}  max {cardiac['hr_max'].max():.0f} bpm")
        spo2 = cardiac["spo2_pct"].dropna()
        if not spo2.empty:
            print(f"SpO2        : mean {spo2.mean():.0f}  min {spo2.min():.0f} %")
        hrv = cardiac.loc[(cardiac["flags"].astype(int) & (1 << 8)) != 0, "rmssd_ms"].dropna()
        if not hrv.empty:
            print(f"RMSSD       : {len(hrv)} valid epochs, mean {hrv.mean():.0f} ms")

    # Flag tally.
    tally = {}
    for flags in df["flags"].astype(int):
        for bit, name in FLAG_NAMES:
            if flags & bit:
                tally[name] = tally.get(name, 0) + 1
    if tally:
        print("flags       : " + ", ".join(f"{k}={v}" for k, v in sorted(tally.items())))

    if args.clean:
        df.to_csv(args.clean, index=False)
        print(f"wrote cleaned CSV -> {args.clean}")


if __name__ == "__main__":
    main()
