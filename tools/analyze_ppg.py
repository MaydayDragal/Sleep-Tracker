#!/usr/bin/env python3
"""Analyze a raw PPG capture (or a synthetic test signal): band-pass filter,
beat detection via scipy AND NeuroKit2, HR, HRV (RMSSD), and a plot.

    py analyze_ppg.py --demo                      # synthetic sanity check
    py analyze_ppg.py raw.csv --fs 100 --col ir   # real capture (see capture_ppg.py)

Input CSV: one sample per row; --col picks the channel (default: first numeric).
This is the offline half of the PPG-accuracy loop — develop/validate the beat
detector here against NeuroKit2 + a known-HR reference, then port the winner to
the on-device C in components/ppg.
"""
import argparse
import numpy as np


def synth(fs=100.0, secs=25, hr=72.0, seed=1):
    """A realistic-ish PPG: systolic peak + dicrotic notch, mild HRV, baseline
    wander, white noise, and a 1 s motion-artifact burst to stress the detector."""
    rng = np.random.default_rng(seed)
    n = int(fs * secs)
    t = np.arange(n) / fs
    ppg = np.zeros(n)
    ibi = 60.0 / hr
    bt, beats = 0.0, []
    while bt < secs:
        beats.append(bt)
        idx = int(bt * fs)
        for k in range(int(0.9 * fs)):
            if idx + k < n:
                tau = k / fs
                ppg[idx + k] += (np.exp(-((tau - 0.12) ** 2) / (2 * 0.05 ** 2))
                                 + 0.4 * np.exp(-((tau - 0.34) ** 2) / (2 * 0.06 ** 2)))
        bt += ibi * (1.0 + 0.04 * rng.standard_normal())      # beat-to-beat HRV
    ppg += 0.3 * np.sin(2 * np.pi * 0.2 * t)                  # baseline wander
    ppg += 0.05 * rng.standard_normal(n)                      # sensor noise
    ppg[int(8 * fs):int(9 * fs)] += 1.5 * rng.standard_normal(int(fs))  # motion burst
    return t, ppg, np.array(beats)


def bandpass(x, fs, lo=0.5, hi=4.0):
    from scipy.signal import butter, filtfilt
    b, a = butter(2, [lo / (fs / 2), hi / (fs / 2)], btype="band")
    return filtfilt(b, a, x)


def scipy_beats(filt, fs):
    from scipy.signal import find_peaks
    pk, _ = find_peaks(filt, distance=int(0.35 * fs), prominence=np.std(filt) * 0.4)
    return pk


def main():
    ap = argparse.ArgumentParser(description="Analyze a PPG capture.")
    ap.add_argument("csv", nargs="?", help="raw PPG CSV (one sample/row)")
    ap.add_argument("--demo", action="store_true", help="use a synthetic signal instead")
    ap.add_argument("--fs", type=float, default=100.0, help="sample rate (Hz)")
    ap.add_argument("--col", default=None, help="CSV column to analyze")
    ap.add_argument("--plot", default="ppg_analysis.png", help="output plot PNG")
    a = ap.parse_args()

    true_beats = true_hr = None
    if a.demo:
        t, raw, true_beats = synth(fs=a.fs)
        true_hr = 60.0 / np.mean(np.diff(true_beats))
    elif a.csv:
        import pandas as pd
        df = pd.read_csv(a.csv)
        col = a.col or df.select_dtypes("number").columns[0]
        raw = df[col].to_numpy(float)
        t = np.arange(len(raw)) / a.fs
    else:
        ap.error("give a CSV path or --demo")

    fs = a.fs
    filt = bandpass(raw, fs)
    pk = scipy_beats(filt, fs)
    scipy_hr = 60.0 * fs / np.mean(np.diff(pk)) if len(pk) > 2 else float("nan")

    nk_hr, nk_pk = float("nan"), np.array([], dtype=int)
    try:
        import neurokit2 as nk
        sig, _ = nk.ppg_process(raw, sampling_rate=fs)
        nk_pk = np.where(sig["PPG_Peaks"].to_numpy() == 1)[0]
        nk_hr = float(np.nanmean(sig["PPG_Rate"]))
    except Exception as e:      # NeuroKit is optional; scipy path always runs
        print("neurokit:", e)

    print(f"samples      : {len(raw)}  (~{len(raw)/fs:.1f}s @ {fs:.0f} Hz)")
    if true_hr:
        print(f"true HR      : {true_hr:.1f} bpm  ({len(true_beats)} beats)")
    print(f"scipy HR     : {scipy_hr:.1f} bpm  ({len(pk)} beats)")
    print(f"neurokit HR  : {nk_hr:.1f} bpm  ({len(nk_pk)} beats)")
    if len(pk) > 3:
        ibi_ms = np.diff(pk) / fs * 1000.0
        rmssd = float(np.sqrt(np.mean(np.diff(ibi_ms) ** 2)))
        print(f"RMSSD        : {rmssd:.1f} ms  (scipy beats)")

    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    fig, ax = plt.subplots(2, 1, figsize=(11, 6), sharex=True)
    ax[0].plot(t, raw, lw=0.6, color="0.5")
    ax[0].set_title("raw PPG"); ax[0].set_ylabel("counts")
    ax[1].plot(t, filt, lw=0.8, color="tab:blue", label="band-passed 0.5-4 Hz")
    ax[1].plot(pk / fs, filt[pk], "r^", ms=6, label=f"scipy beats ({len(pk)})")
    if len(nk_pk):
        idx = np.clip(nk_pk, 0, len(filt) - 1)
        ax[1].plot(nk_pk / fs, filt[idx], "gv", ms=5, label=f"neurokit ({len(nk_pk)})")
    ax[1].legend(loc="upper right", fontsize=8)
    ax[1].set_xlabel("time (s)"); ax[1].set_ylabel("filtered")
    fig.tight_layout()
    fig.savefig(a.plot, dpi=110)
    print("wrote plot ->", a.plot)


if __name__ == "__main__":
    main()
