#!/usr/bin/env python3
"""Capture + rank the firmware PPG LED-current x averaging sweep.

Flash the diagnostic build and run the sweep, holding a fingertip still on the
sensor for the whole ~5 min run:

    pio run -d firmware -e esp32s3-ppg-sweep -t upload
    py tools/ppg_sweep.py --port COM6

The sweep firmware (main/sleep_tracker_main.c, SLEEPTRK_PPG_SWEEP) prints a header
then one CSV row per (LED current x SMP_AVE) combo, terminated by "SWEEP,DONE":

    SWEEP,led,ave,out_hz,n,ir_dc,ir_max,clip_pct,snr_db,sqi,hr,valid

This reads those rows, saves them to a CSV, and ranks the combos to recommend the
best LED current + averaging. "Best" = highest signal quality (SQI, which folds in
perfusion x amplitude-consistency x regularity x AN6410 SNR) among combos that do
NOT clip the ADC and where the pipeline locked a plausible HR. It also flags the
clipping onset so you keep headroom.
"""
import argparse
import csv
import time

import serial

COLS = ["led", "ave", "out_hz", "n", "ir_dc", "ir_max",
        "clip_pct", "snr_db", "sqi", "hr", "valid"]
CLIP_MAX = 0.5     # %: reject combos clipping more than this
HR_LO, HR_HI = 30, 220


def capture(port, baud, timeout_s):
    """Read SWEEP rows off the port until 'SWEEP,DONE' or timeout."""
    ser = serial.Serial(port, baud, timeout=0.5)
    # The sweep prints once at boot, so hard-reset (EN via RTS) to (re)start it.
    ser.setDTR(False)
    ser.setRTS(True)
    time.sleep(0.1)
    ser.setRTS(False)
    time.sleep(0.1)
    ser.reset_input_buffer()
    rows, buf, end = [], b"", time.time() + timeout_s
    print(f"listening on {port} (hold a fingertip still; ~5 min)...")
    while time.time() < end:
        buf += ser.read(4096)
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            s = line.decode("ascii", "ignore").strip()
            if not s.startswith("SWEEP,"):
                continue
            body = s[len("SWEEP,"):]
            if body == "DONE":
                ser.close()
                print("sweep complete.")
                return rows
            if body.startswith("led,"):     # header row
                continue
            p = body.split(",")
            if len(p) != len(COLS):
                continue
            try:
                r = {
                    "led": int(p[0]), "ave": int(p[1]), "out_hz": int(p[2]),
                    "n": int(p[3]), "ir_dc": float(p[4]), "ir_max": int(p[5]),
                    "clip_pct": float(p[6]), "snr_db": float(p[7]),
                    "sqi": float(p[8]), "hr": float(p[9]), "valid": int(p[10]),
                }
            except ValueError:
                continue
            rows.append(r)
            print(f"  led={r['led']:>3} ave={r['ave']:>2} out={r['out_hz']:>3}Hz "
                  f"dc={r['ir_dc']:>7.0f} clip={r['clip_pct']:>5.2f}% "
                  f"snr={r['snr_db']:>5.1f}dB sqi={r['sqi']:.3f} hr={r['hr']:.0f}")
    ser.close()
    print("timed out before SWEEP,DONE.")
    return rows


def usable(r):
    return (r["clip_pct"] <= CLIP_MAX and r["valid"] == 1
            and HR_LO <= r["hr"] <= HR_HI)


def rank(rows):
    if not rows:
        print("\nno rows captured.")
        return

    ok = [r for r in rows if usable(r)]
    print(f"\n{len(ok)}/{len(rows)} combos usable "
          f"(clip<={CLIP_MAX}%, valid HR).")

    # Clipping onset per LED (lowest LED that clips) — stay below it for headroom.
    clippers = sorted({r["led"] for r in rows if r["clip_pct"] > CLIP_MAX})
    if clippers:
        print(f"clipping starts at LED >= {clippers[0]} "
              f"(ir_max approached full scale). Keep LED below this.")
    else:
        print("no clipping seen across the swept LED range.")

    if not ok:
        print("no non-clipping combo locked a valid HR — check finger contact.")
        return

    best = sorted(ok, key=lambda r: (r["sqi"], r["snr_db"]), reverse=True)
    print("\nTop combos by SQI (then SNR):")
    print(f"  {'led':>3} {'ave':>3} {'out_hz':>6} {'sqi':>6} {'snr_db':>7} "
          f"{'ir_dc':>8} {'clip%':>6} {'hr':>4}")
    for r in best[:8]:
        print(f"  {r['led']:>3} {r['ave']:>3} {r['out_hz']:>6} {r['sqi']:>6.3f} "
              f"{r['snr_db']:>7.1f} {r['ir_dc']:>8.0f} {r['clip_pct']:>6.2f} "
              f"{r['hr']:>4.0f}")

    top = best[0]
    # For HR/HRV you also want temporal resolution: prefer the best combo whose
    # output rate stays >= 50 Hz (ave <= 8 at the 400 Hz base) if it's close.
    hires = [r for r in ok if r["out_hz"] >= 50]
    hr_best = sorted(hires, key=lambda r: (r["sqi"], r["snr_db"]),
                     reverse=True)[0] if hires else None

    print("\nRECOMMENDATION")
    print(f"  Peak SQI overall : led={top['led']}, ave={top['ave']} "
          f"(out {top['out_hz']} Hz, sqi {top['sqi']:.3f}, snr {top['snr_db']:.1f} dB)")
    if hr_best and (hr_best["led"], hr_best["ave"]) != (top["led"], top["ave"]):
        print(f"  Best for HR/HRV  : led={hr_best['led']}, ave={hr_best['ave']} "
              f"(out {hr_best['out_hz']} Hz, sqi {hr_best['sqi']:.3f}) — keeps "
              f">=50 Hz timing resolution")
    print(f"  Wire it in main's ppg_cfg: led_current_red/ir = {hr_best['led'] if hr_best else top['led']}, "
          f"smp_ave = {hr_best['ave'] if hr_best else top['ave']}.")
    print("  Note: higher averaging raises SQI/SNR but lowers the output rate — "
          "keep out_hz >= ~50 Hz for usable HRV timing.")


def main():
    ap = argparse.ArgumentParser(description="Capture + rank the PPG sweep.")
    ap.add_argument("--port", default="COM6")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--timeout", type=float, default=420.0,
                    help="max seconds to wait for SWEEP,DONE")
    ap.add_argument("--out", default="ppg_sweep.csv")
    a = ap.parse_args()

    rows = capture(a.port, a.baud, a.timeout)
    if rows:
        with open(a.out, "w", newline="") as f:
            w = csv.DictWriter(f, fieldnames=COLS)
            w.writeheader()
            w.writerows(rows)
        print(f"\nwrote {len(rows)} rows -> {a.out}")
    rank(rows)


if __name__ == "__main__":
    main()
