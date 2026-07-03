#!/usr/bin/env python3
"""Generate a synthetic Sleep-Tracker night log in the exact firmware CSV schema.

Lets us develop and test the offline scoring pipeline (score_night.py) without a
real overnight recording — the Phase-2 8-hour battery run is hardware-blocked, so
this stands in for it. The generator builds a hidden "true" hypnogram and emits
the observable columns the device would log; score_night.py then has to *infer*
the hypnogram back, so the two are a closed test loop.

    py gen_synthetic_night.py --out night.csv
    py gen_synthetic_night.py --hours 8 --seed 7 --hrv --out night.csv

Models the real device behaviour: wrist actigraphy is dense (every 30 s epoch),
but PPG is duty-cycled (~45 s per 5 min), so most epochs carry NO_CARDIAC and only
~1-in-10 has HR/SpO2. body_position is populated here for a meaningful position
breakdown; pass --no-position to mimic today's logs (Phase 2.5 not yet on hardware).
"""
from __future__ import annotations
import argparse
import random

import sleeplog as S

# Hidden stages.
W, L, D, R = "W", "L", "D", "R"


def build_hypnogram(n_epochs: int, rng: random.Random) -> list[str]:
    """A plausible night: sleep-onset latency, then ~90-min cycles with deep
    front-loaded and REM back-loaded, plus a few brief awakenings."""
    ep_per_min = 60 // S.EPOCH_SECONDS  # 2 epochs/min
    stages: list[str] = []

    # Sleep-onset latency: 10-18 min awake.
    stages += [W] * (rng.randint(10, 18) * ep_per_min)

    # Per-cycle (deep_min, rem_min); light fills the rest of a ~90-min cycle.
    # Deep is front-loaded, REM back-loaded, light stays the plurality — giving
    # roughly physiological whole-night proportions (~15% deep, ~20% REM, ~55%
    # light, plus wake).
    schedule = [(25, 8), (20, 15), (15, 22), (9, 25), (6, 25)]
    cycle = 0
    while len(stages) < n_epochs:
        deep_min, rem_min = schedule[min(cycle, len(schedule) - 1)]
        light_total = max(20, 90 - deep_min - rem_min)
        la, lb = light_total // 2, light_total - light_total // 2
        block = (
            [L] * (la * ep_per_min)
            + [D] * (deep_min * ep_per_min)
            + [L] * (lb * ep_per_min)
            + [R] * (rem_min * ep_per_min)
        )
        # Occasional brief awakening at the cycle boundary (WASO).
        if cycle > 0 and rng.random() < 0.5:
            block += [W] * (rng.randint(1, 3) * ep_per_min)
        stages += block
        cycle += 1

    stages = stages[:n_epochs]
    # Taper to wake in the final ~4 min (natural morning awakening).
    for i in range(max(0, n_epochs - 4 * ep_per_min), n_epochs):
        if rng.random() < 0.5:
            stages[i] = W
    return stages


TRUE_STAGE = {W: "wake", L: "light", D: "deep", R: "rem"}


def gen(args) -> list[dict]:
    rng = random.Random(args.seed)
    n = int(args.hours * 3600 // S.EPOCH_SECONDS)
    stages = build_hypnogram(n, rng)

    awake_hr = rng.randint(58, 66)          # personal awake baseline
    duty = max(1, args.duty)                 # cardiac cadence in epochs (10 = 5 min)
    position = 1                             # start supine (back)
    rows: list[dict] = []

    for i, st in enumerate(stages):
        prev = stages[i - 1] if i else W
        transition = st != prev

        # --- wrist actigraphy (dense) ---
        if st == W:
            activity = rng.randint(120, 480)
        elif st == L:
            activity = rng.randint(0, 70)
        elif st == R:
            activity = rng.randint(0, 35)     # near-atonia, occasional twitch
        else:  # D
            activity = rng.randint(0, 15)
        if transition and st != D:
            activity += rng.randint(30, 120)  # positional shift / arousal on transition
        motion = activity > 90

        # --- position: more likely to change while awake / in light sleep ---
        if st in (W, L) and (transition or rng.random() < 0.03):
            position = rng.choice([1, 1, 2, 2, 3, 3, 4])  # belly rarer
        body_activity = activity if position != (rows[-1]["body_position"] if rows else 1) else rng.randint(0, 20)

        # --- cardiac (duty-cycled) ---
        cardiac = (i % duty == 0)
        flags = 0
        hr_mean = hr_min = hr_max = rmssd = spo2 = sqi = beat_accept = 0
        if cardiac:
            if st == D:
                base = awake_hr - rng.randint(11, 16)
                var = 2
            elif st == R:
                base = awake_hr - rng.randint(-3, 3)
                var = 6                      # REM: elevated, more variable
            elif st == L:
                base = awake_hr - rng.randint(7, 11)
                var = 3
            else:  # W
                base = awake_hr + rng.randint(0, 6)
                var = 5
            hr_mean = max(40, base)
            hr_min = max(38, hr_mean - var)
            hr_max = hr_mean + var + (rng.randint(0, 6) if st == W else 0)

            spo2 = rng.randint(96, 99)
            # Inject occasional desaturations, biased to REM + supine (apnea-like).
            if st in (R, L) and position == 1 and rng.random() < 0.14:
                spo2 = rng.randint(85, 90)   # supine + REM/light = apnea-like dips
            if spo2 < 90:
                flags |= S.FLAG_DESAT

            sqi = rng.randint(70, 98) if not motion else rng.randint(30, 70)
            flags |= S.FLAG_SQI_PROXY
            beat_accept = min(100, sqi + rng.randint(-5, 5))

            if args.hrv and st in (D, R, L) and not motion and sqi > 75:
                rmssd = {D: rng.randint(45, 75), R: rng.randint(25, 45),
                         L: rng.randint(35, 60)}[st]
                flags |= S.FLAG_HRV_VALID
        else:
            flags |= S.FLAG_NO_CARDIAC | S.FLAG_NO_PPG

        if motion:
            flags |= S.FLAG_MOTION

        # --- battery: linear-ish drain, ~4 %/h ---
        batt = max(0, round(100 - (i / n) * args.hours * 4.0))

        if args.no_position:
            position_out = 0
            body_activity = 0
        else:
            position_out = position

        rows.append({
            "seq": i,
            "t_unix": args.start + i * S.EPOCH_SECONDS,
            "activity": min(65535, activity),
            "body_position": position_out,
            "body_activity": min(65535, body_activity),
            "hr_mean": hr_mean, "hr_min": hr_min, "hr_max": hr_max,
            "rmssd_ms": rmssd, "spo2_pct": spo2, "sqi": sqi,
            "batt_pct": batt, "beat_accept": beat_accept, "flags": flags,
        })
    return rows, [TRUE_STAGE[s] for s in stages]


def main() -> None:
    ap = argparse.ArgumentParser(description="Generate a synthetic night log.")
    ap.add_argument("--out", default="synthetic_night.csv", help="output CSV path")
    ap.add_argument("--hours", type=float, default=8.0, help="time in bed (hours)")
    ap.add_argument("--seed", type=int, default=1, help="RNG seed (reproducible)")
    ap.add_argument("--duty", type=int, default=10, help="cardiac cadence in epochs (10 = one PPG window / 5 min)")
    ap.add_argument("--start", type=int, default=1_780_000_000, help="epoch time of first sample")
    ap.add_argument("--hrv", action="store_true", help="populate RMSSD/HRV_VALID (optional feature)")
    ap.add_argument("--no-position", action="store_true", help="emit body_position=0 (mimic pre-Phase-2.5 logs)")
    ap.add_argument("--truth", metavar="OUT", help="also write the ground-truth hypnogram (seq,stage) for scorer validation")
    args = ap.parse_args()

    rows, truth = gen(args)
    S.write_epochs(rows, args.out)
    print(f"wrote {len(rows)} epochs (~{len(rows)*S.EPOCH_SECONDS/3600:.1f} h) -> {args.out}")
    if args.truth:
        with open(args.truth, "w") as f:
            f.write("seq,stage\n")
            for i, st in enumerate(truth):
                f.write(f"{i},{st}\n")
        print(f"wrote ground-truth hypnogram -> {args.truth}")


if __name__ == "__main__":
    main()
