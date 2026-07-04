#!/usr/bin/env python3
"""Offline sleep scoring for a Sleep-Tracker night log — a prototype of the
Phase-3 "morning re-pass" (PLAN.md §3.2, §5 Phase 3).

Pipeline:
  1. Sleep/wake per epoch — a Cole-Kripke-style weighted moving average over the
     dense wrist actigraphy `activity` column, thresholded.
  2. Stage refinement (light / deep / REM-estimate) from HR relative to the
     night's sleeping baseline + movement, then bout-smoothed.
  3. Summary metrics (TST, efficiency, latency, WASO, awakenings, RHR, SpO2/ODI,
     position breakdown) and a 0-100 sleep score.
  4. A dependency-free SVG hypnogram (no matplotlib needed).

This runs on real logs and on synthetic ones from gen_synthetic_night.py, so the
generator + scorer form a closed loop for developing the algorithm before the
hardware overnight run exists. Thresholds are deliberately explicit CLI knobs —
they must be *tuned on recorded nights* (Phase 3 task), since actigraphy counts
are device-specific.

    py score_night.py night.csv
    py score_night.py night.csv --svg hypno.svg --out scored.csv
"""
from __future__ import annotations
import argparse

import sleeplog as S

STAGE_W, STAGE_L, STAGE_D, STAGE_R = "wake", "light", "deep", "rem"
EP_PER_MIN = 60 // S.EPOCH_SECONDS
EP_PER_HR = 3600 // S.EPOCH_SECONDS


# ---- 1. sleep/wake ---------------------------------------------------------

def weighted_activity(activity):
    """Cole-Kripke-style smoothing: a centred weighted moving average of activity.
    (The classic CK constants assume 1-min epochs and specific count units; here
    we use a tunable symmetric kernel on 30 s epochs — calibrate on real data.)"""
    kernel = [0.04, 0.12, 0.20, 0.28, 0.20, 0.12, 0.04]  # sums to 1.0, ±3 epochs
    half = len(kernel) // 2
    n = len(activity)
    out = [0.0] * n
    for i in range(n):
        acc = wsum = 0.0
        for k, w in enumerate(kernel):
            j = i + k - half
            if 0 <= j < n:
                acc += w * activity[j]
                wsum += w
        out[i] = acc / wsum if wsum else 0.0
    return out


def classify_sleep_wake(df, threshold):
    activity = [int(a) if a == a else 0 for a in df["activity"].astype("float")]
    wma = weighted_activity(activity)
    wrist_off = [(int(f) & S.FLAG_WRIST_OFF) != 0 for f in df["flags"].astype(int)]
    return [(wma[i] < threshold) and not wrist_off[i] for i in range(len(df))], wma


# ---- 2. staging ------------------------------------------------------------

def hr_forward_filled(df):
    """HR is sampled only on duty-cycled cardiac epochs; carry it forward (and
    back-fill the leading gap) so staging has a value every epoch."""
    hr = [float(v) if v == v else None for v in df["hr_mean"].astype("float")]
    last = None
    for i in range(len(hr)):
        if hr[i] is not None:
            last = hr[i]
        elif last is not None:
            hr[i] = last
    nxt = None  # back-fill the leading gap
    for i in range(len(hr) - 1, -1, -1):
        if hr[i] is not None:
            nxt = hr[i]
        elif nxt is not None:
            hr[i] = nxt
    return hr


def stage_night(df, asleep, deep_act, rem_act, deep_hr, rem_hr):
    activity = [int(a) if a == a else 0 for a in df["activity"].astype("float")]
    hr = hr_forward_filled(df)
    sleep_hr = [hr[i] for i in range(len(df)) if asleep[i] and hr[i] is not None]
    baseline = sorted(sleep_hr)[len(sleep_hr) // 2] if sleep_hr else None

    # Deep sits below the sleeping-HR baseline, REM clearly above it; light is the
    # band in between (and the default when HR is unavailable). Offsets are in bpm
    # relative to the baseline and MUST be tuned per device on recorded nights.
    stages = []
    for i in range(len(df)):
        if not asleep[i]:
            stages.append(STAGE_W)
            continue
        if baseline is None or hr[i] is None:
            stages.append(STAGE_L)
            continue
        rel = hr[i] - baseline
        if activity[i] <= deep_act and rel <= deep_hr:
            stages.append(STAGE_D)
        elif activity[i] <= rem_act and rel >= rem_hr:
            stages.append(STAGE_R)
        else:
            stages.append(STAGE_L)

    stages = smooth_bouts(stages, min_len=3)
    suppress_early_rem(stages, asleep)
    return stages, baseline


def smooth_bouts(stages, min_len):
    """Collapse stage bouts shorter than min_len into the previous bout — kills
    single-epoch flicker (a crude version of the clinical 'don't stage <1 min')."""
    out = stages[:]
    i = 0
    n = len(out)
    while i < n:
        j = i
        while j < n and out[j] == out[i]:
            j += 1
        if (j - i) < min_len and i > 0:
            for k in range(i, j):
                out[k] = out[i - 1]
        i = j
    return out


def suppress_early_rem(stages, asleep):
    """REM in the first ~60 min after sleep onset is physiologically unlikely;
    reassign it to light (common actigraphy-staging heuristic)."""
    try:
        onset = asleep.index(True)
    except ValueError:
        return
    for i in range(onset, min(len(stages), onset + 60 * EP_PER_MIN)):
        if stages[i] == STAGE_R:
            stages[i] = STAGE_L


# ---- 3. metrics + score ----------------------------------------------------

def clamp(x, lo=0.0, hi=1.0):
    return max(lo, min(hi, x))


def summarize(df, asleep, stages, baseline):
    n = len(df)
    m = {}
    m["epochs"] = n
    m["tib_min"] = n * S.EPOCH_SECONDS / 60
    asleep_idx = [i for i in range(n) if asleep[i]]
    if not asleep_idx:
        m["no_sleep"] = True
        return m
    onset, final = asleep_idx[0], asleep_idx[-1]
    m["onset_latency_min"] = onset * S.EPOCH_SECONDS / 60
    m["tst_min"] = len(asleep_idx) * S.EPOCH_SECONDS / 60
    m["efficiency"] = len(asleep_idx) / n

    # WASO + awakenings within the sleep period (onset..final).
    waso = awk = 0
    in_wake = False
    for i in range(onset, final + 1):
        if not asleep[i]:
            waso += 1
            if not in_wake:
                awk += 1
                in_wake = True
        else:
            in_wake = False
    m["waso_min"] = waso * S.EPOCH_SECONDS / 60
    m["awakenings"] = awk

    for st, key in ((STAGE_D, "deep"), (STAGE_L, "light"), (STAGE_R, "rem")):
        c = sum(1 for i in asleep_idx if stages[i] == st)
        m[f"{key}_min"] = c * S.EPOCH_SECONDS / 60
        m[f"{key}_pct"] = c / len(asleep_idx) if asleep_idx else 0.0

    # Resting HR: mean of the lowest decile of sleeping HR.
    hr = hr_forward_filled(df)
    shr = sorted(hr[i] for i in asleep_idx if hr[i] is not None)
    if shr:
        k = max(1, len(shr) // 10)
        m["rhr"] = sum(shr[:k]) / k
        m["hr_baseline"] = baseline

    # SpO2 + Oxygen Desaturation Index (desat events / hour asleep).
    spo2 = [float(v) for v in df["spo2_pct"].astype("float") if v == v]
    if spo2:
        m["spo2_min"] = min(spo2)
        m["spo2_mean"] = sum(spo2) / len(spo2)
    flags = [int(f) for f in df["flags"].astype(int)]
    desat_events = prev = 0
    for i in range(n):
        d = (flags[i] & S.FLAG_DESAT) != 0
        if d and not prev:
            desat_events += 1
        prev = d
    hrs_asleep = m["tst_min"] / 60
    m["desat_events"] = desat_events
    m["odi"] = desat_events / hrs_asleep if hrs_asleep else 0.0

    # Position breakdown over asleep epochs (+ position-segmented HR / SpO2).
    pos_min, pos_hr, pos_spo2 = {}, {}, {}
    positions = [int(p) if p == p else 0 for p in df["body_position"].astype("float")]
    spo2_col = [float(v) if v == v else None for v in df["spo2_pct"].astype("float")]
    for i in asleep_idx:
        p = positions[i]
        pos_min[p] = pos_min.get(p, 0) + 1
        if hr[i] is not None:
            pos_hr.setdefault(p, []).append(hr[i])
        if spo2_col[i] is not None:
            pos_spo2.setdefault(p, []).append(spo2_col[i])
    m["position"] = {
        S.POSITIONS.get(p, str(p)): {
            "min": c * S.EPOCH_SECONDS / 60,
            "pct": c / len(asleep_idx),
            "hr": (sum(pos_hr[p]) / len(pos_hr[p])) if pos_hr.get(p) else None,
            "spo2_min": min(pos_spo2[p]) if pos_spo2.get(p) else None,
        }
        for p, c in sorted(pos_min.items(), key=lambda kv: -kv[1])
    }
    m["has_position"] = any(p != 0 for p in positions)
    return m


def sleep_score(m):
    if m.get("no_sleep"):
        return 0
    dur = clamp(m["tst_min"] / 60 / 8.0)                    # vs 8 h goal
    eff = clamp((m["efficiency"] - 0.50) / 0.45)            # 50%->0, 95%->1
    cont = clamp(1 - m["awakenings"] / 10.0)               # fewer awakenings better
    deep_err = abs(m.get("deep_pct", 0) - 0.18) / 0.18
    rem_err = abs(m.get("rem_pct", 0) - 0.22) / 0.22
    restoration = clamp(1 - 0.5 * deep_err - 0.5 * rem_err)
    score = 100 * (0.30 * dur + 0.25 * eff + 0.20 * cont + 0.25 * restoration)
    return round(score)


# ---- 4. SVG hypnogram ------------------------------------------------------

STAGE_LANE = {STAGE_W: 0, STAGE_R: 1, STAGE_L: 2, STAGE_D: 3}
STAGE_COLOR = {STAGE_W: "#e4572e", STAGE_R: "#4c9be8", STAGE_L: "#8bd3c7", STAGE_D: "#3d348b"}


def write_svg(stages, t0, score, path):
    n = len(stages)
    W, lane_h, pad_l, pad_t, pad_b = max(720, n), 34, 64, 46, 30
    H = pad_t + 4 * lane_h + pad_b
    px = (W - pad_l - 16) / n
    parts = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" '
             f'font-family="system-ui,sans-serif" font-size="12">']
    parts.append(f'<rect width="{W}" height="{H}" fill="#0f1020"/>')
    parts.append(f'<text x="{pad_l}" y="24" fill="#f0f0f5" font-size="16">'
                 f'Hypnogram — sleep score {score}/100</text>')
    for st, lane in STAGE_LANE.items():
        y = pad_t + lane * lane_h + lane_h / 2
        parts.append(f'<text x="8" y="{y+4:.0f}" fill="#9aa">{st}</text>')
        parts.append(f'<line x1="{pad_l}" y1="{y:.0f}" x2="{W-16}" y2="{y:.0f}" stroke="#23243a"/>')
    # stage bars
    for i, st in enumerate(stages):
        x = pad_l + i * px
        y = pad_t + STAGE_LANE[st] * lane_h + 6
        parts.append(f'<rect x="{x:.1f}" y="{y}" width="{px+0.6:.1f}" height="{lane_h-12}" '
                     f'fill="{STAGE_COLOR[st]}"/>')
    # hour ticks
    from datetime import datetime, timezone
    for h in range(0, n // EP_PER_HR + 1):
        i = h * EP_PER_HR
        x = pad_l + i * px
        label = datetime.fromtimestamp(t0 + i * S.EPOCH_SECONDS, tz=timezone.utc).strftime("%H:%M")
        parts.append(f'<line x1="{x:.0f}" y1="{pad_t}" x2="{x:.0f}" y2="{pad_t+4*lane_h}" stroke="#31324e"/>')
        parts.append(f'<text x="{x:.0f}" y="{H-10}" fill="#9aa" text-anchor="middle">{label}</text>')
    parts.append("</svg>")
    with open(path, "w") as f:
        f.write("\n".join(parts))


# ---- report ----------------------------------------------------------------

def hm(minutes):
    return f"{int(minutes)//60}h{int(minutes)%60:02d}m"


def report(path, m, score):
    print(f"file          : {path}")
    print(f"time in bed   : {hm(m['tib_min'])}  ({m['epochs']} epochs)")
    if m.get("no_sleep"):
        print("no sleep detected (all-wake or wrist-off night)")
        return
    print(f"SLEEP SCORE   : {score}/100")
    print(f"total sleep   : {hm(m['tst_min'])}   efficiency {100*m['efficiency']:.0f}%")
    print(f"onset latency : {m['onset_latency_min']:.0f} min    "
          f"WASO {m['waso_min']:.0f} min    awakenings {m['awakenings']}")
    print(f"stages        : deep {hm(m['deep_min'])} ({100*m['deep_pct']:.0f}%)   "
          f"light {hm(m['light_min'])} ({100*m['light_pct']:.0f}%)   "
          f"REM {hm(m['rem_min'])} ({100*m['rem_pct']:.0f}%)")
    if "rhr" in m:
        print(f"resting HR    : {m['rhr']:.0f} bpm   (sleeping baseline {m['hr_baseline']:.0f})")
    if "spo2_min" in m:
        print(f"SpO2          : mean {m['spo2_mean']:.0f}%   min {m['spo2_min']:.0f}%   "
              f"desat events {m['desat_events']}   ODI {m['odi']:.1f}/h")
    if m.get("has_position"):
        print("position      :")
        for name, d in m["position"].items():
            extra = ""
            if d["hr"] is not None:
                extra = f"   HR {d['hr']:.0f}"
            if d["spo2_min"] is not None:
                extra += f"   SpO2min {d['spo2_min']:.0f}%"
            print(f"    {name:<7} {hm(d['min'])} ({100*d['pct']:.0f}%){extra}")
    else:
        print("position      : not available (body sensor / Phase 2.5 not present)")


def validate(stages, truth_path):
    """Compare predicted stages against a ground-truth hypnogram (from
    gen_synthetic_night.py --truth): sleep/wake accuracy, 4-stage accuracy, and a
    confusion matrix. This is how the staging thresholds get tuned objectively."""
    import csv
    truth = {}
    with open(truth_path) as f:
        for row in csv.DictReader(f):
            truth[int(row["seq"])] = row["stage"]
    labels = [STAGE_W, STAGE_L, STAGE_D, STAGE_R]
    conf = {t: {p: 0 for p in labels} for t in labels}
    n = correct = sw_correct = 0
    for i, pred in enumerate(stages):
        t = truth.get(i)
        if t is None:
            continue
        n += 1
        correct += (t == pred)
        sw_correct += ((t == STAGE_W) == (pred == STAGE_W))
        conf[t][pred] += 1
    if not n:
        return
    print(f"\nvalidation vs {truth_path}:")
    print(f"  sleep/wake accuracy : {100*sw_correct/n:.1f}%  (n={n})")
    print(f"  4-stage accuracy    : {100*correct/n:.1f}%")
    print("  confusion (rows=true, cols=pred):")
    print("           " + "".join(f"{p:>7}" for p in labels))
    for t in labels:
        print(f"    {t:<7}" + "".join(f"{conf[t][p]:>7}" for p in labels))


def main() -> None:
    ap = argparse.ArgumentParser(description="Score a Sleep-Tracker night log.")
    ap.add_argument("csv", help="path to a night epoch CSV")
    ap.add_argument("--wake-threshold", type=float, default=90.0,
                    help="smoothed-activity threshold above which an epoch is wake (tune per device)")
    ap.add_argument("--deep-act", type=int, default=25, help="max activity for a deep-sleep epoch")
    ap.add_argument("--rem-act", type=int, default=50, help="max activity for a REM epoch")
    ap.add_argument("--deep-hr", type=float, default=-4.0, help="HR (bpm vs sleeping baseline) at/below which sleep is deep")
    ap.add_argument("--rem-hr", type=float, default=4.0, help="HR (bpm vs sleeping baseline) at/above which sleep is REM")
    ap.add_argument("--svg", metavar="OUT", help="write an SVG hypnogram")
    ap.add_argument("--out", metavar="OUT", help="write a per-epoch scored CSV (adds stage,asleep)")
    ap.add_argument("--truth", metavar="CSV", help="validate against a ground-truth hypnogram (seq,stage)")
    args = ap.parse_args()

    df = S.load_epochs(args.csv)
    asleep, _ = classify_sleep_wake(df, args.wake_threshold)
    stages, baseline = stage_night(df, asleep, args.deep_act, args.rem_act, args.deep_hr, args.rem_hr)
    m = summarize(df, asleep, stages, baseline)
    score = sleep_score(m)
    report(args.csv, m, score)
    if args.truth:
        validate(stages, args.truth)

    if args.svg:
        write_svg(stages, int(df["t_unix"].iloc[0]), score, args.svg)
        print(f"wrote hypnogram -> {args.svg}")
    if args.out:
        df2 = df.copy()
        df2["asleep"] = [int(a) for a in asleep]
        df2["stage"] = stages
        df2.to_csv(args.out, index=False)
        print(f"wrote scored CSV -> {args.out}")


if __name__ == "__main__":
    main()
