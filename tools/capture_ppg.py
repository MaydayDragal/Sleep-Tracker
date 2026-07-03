#!/usr/bin/env python3
"""Capture raw PPG streamed by the firmware over serial into a CSV.

The debug firmware (PPG_RAW_STREAM in main) prints one line per sample:
    R,<ir>,<red>
This reads them off the port for the capture window and writes ir,red columns
that analyze_ppg.py can consume.

    py capture_ppg.py --port COM3 --secs 30 --out raw.csv

Rest a fingertip on the MAX30102 during the capture for a real pulse.
"""
import argparse
import time

import serial


def main():
    ap = argparse.ArgumentParser(description="Capture raw PPG over serial.")
    ap.add_argument("--port", default="COM3")
    ap.add_argument("--secs", type=float, default=30.0)
    ap.add_argument("--out", default="raw_ppg.csv")
    ap.add_argument("--baud", type=int, default=115200)
    a = ap.parse_args()

    ser = serial.Serial(a.port, a.baud, timeout=0.3)
    rows = []
    buf = b""
    end = time.time() + a.secs
    while time.time() < end:
        buf += ser.read(8192)
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            s = line.decode("ascii", "ignore").strip()
            if s.startswith("R,"):
                p = s.split(",")
                if len(p) == 3:
                    try:
                        rows.append((int(p[1]), int(p[2])))
                    except ValueError:
                        pass
    ser.close()

    with open(a.out, "w", newline="") as f:
        f.write("ir,red\n")
        for ir, red in rows:
            f.write(f"{ir},{red}\n")
    fs = len(rows) / a.secs if a.secs else 0
    print(f"captured {len(rows)} samples in {a.secs:.0f}s (~{fs:.0f} Hz) -> {a.out}")


if __name__ == "__main__":
    main()
