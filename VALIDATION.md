# Validation & De-risking Plan

Now that the hardware is in hand (board, MAX30102 — MAX30101 arriving, **Polar H10**), the smart engineering move *before* building the full firmware is to de-risk a few key assumptions with small, throwaway spikes — mainly power, SD throughput, and sensor coupling. One spike (S1) also checks whether the **optional** HRV feature is feasible on your wrist; HRV is a nice-to-have, so a poor result there just means HRV is deferred or dropped, not that the project is blocked.

> **Status note (the build ran ahead of these spikes):** Phases 0–2 were built and hardware-validated without formally running S1–S5 first. That's fine — these remain the open de-risking items, now with sharper targets: **S3/S4** are the specific blockers on the Phase-2 "8 h on battery" exit gate (SD throughput + overnight mAh, once a Li-Po is attached), and **S1** is the go/no-go for the fuller HRV-grade path (a basic live RMSSD already ships). Run them against the real recorded nights the firmware now produces.

This doc covers three things:
1. **De-risking spikes** — the riskiest assumptions and the minimal experiments that test them (§1).
2. **HRV validation protocol** — how to use the H10 as ground truth *if* you pursue the optional HRV feature (§2).
3. **H10 as an on-device reference** — a development trick that makes validation live instead of an offline chore (§3).

---

## 1. De-risking spikes (run in this order, before full builds)

Each is a rough sketch, not production code — though several feed directly into the real `ppg`/`sleep_core`/`board` components. Do them on the real board, on your own wrist.

| # | Assumption under test | Minimal experiment | Pass bar | If it fails |
|---|---|---|---|---|
| **S1** | **Wrist PPG (MAX30102/30101) can yield beat timing good enough for RMSSD on *your* body** (decides whether the optional HRV feature is worth pursuing) | Sit still 5 min. Record raw IR PPG to SD + H10 RR simultaneously. Offline: detect beats, compute RMSSD both ways, compare. | RMSSD within ~5–10 ms / <15% of H10 on clean, still data | Iterate sensor pressure/placement, LED current, sample rate — or drop/defer HRV; core tracking is unaffected |
| **S2** | **We can tell good windows from bad** (motion robustness) | Record still → slight motion → restless. Watch SQI + activity count. | SQI reliably flags the corrupted windows so they can be excluded | Strengthen SQI (perfusion index, template correlation) before trusting any HRV |
| **S3** | **PSRAM can buffer full-rate raw data and flush to SD all night without drops** | Synthetic generator: 400 Hz × (red+ir) + 50 Hz accel → PSRAM ring → SD. Run 8 h. Count dropped samples, measure sustained write rate. | Zero drops over a night; SD writes fit well inside the timing budget | Add compression (§ optimizations), lower raw rate, or log epochs-only |
| **S4** | **S3 + duty-cycling survives a night on the chosen cell** | Run a duty-cycle skeleton with sensors active on the real duty pattern. Read mAh consumed from the AXP2101 fuel gauge over 8 h. | ≥ one full night with comfortable margin on the 500–1000 mAh cell | Tune duty cycle / HRV-window frequency → C6 fallback if still short |
| **S5** | **The sensor can be held against the wrist steadily enough** (mechanical coupling) | Crude strap. Log DC level + SQI across a night, including position changes. | Contact and signal maintained through rolling over | Redesign strap/pressure; coupling determines PPG quality more than any code |

**S1 decides the HRV nice-to-have.** If wrist PPG HRV isn't achievable on your body, you simply skip or defer HRV — core sleep tracking (HR, SpO2, movement, staging, position) is unaffected. Run S1 early anyway so the decision is made on data, not late in the build; §2/§3 exist to make it rigorous and fast.

---

## 2. HRV validation protocol (Polar H10 as ground truth)

The H10 is a chest-strap ECG — RR intervals from it are the clinical gold standard for HRV, and it exposes them over standard BLE. It is exactly the right reference for validating (and tuning) the wrist pipeline.

### 2.1 Session design
- **Simultaneous** wrist + H10 on the same session.
- Three conditions, in increasing difficulty:
  - **Baseline:** 5 min seated, still — the cleanest possible comparison (this is the S1 spike).
  - **Paced breathing 6/min:** deliberately drives large, predictable RSA/HRV swings — great dynamic range to confirm the wrist *tracks* HRV, not just averages it.
  - **Full night:** the real target condition, with all its motion and contact challenges.
- Repeat across **≥5–7 nights** (and ideally more than one wearer) before declaring anything — one good night can be luck.

### 2.2 Time alignment (the classic two-device pitfall)
RMSSD compares *consecutive* beats, so even a small clock offset ruins beat-to-beat matching. Options, best first:
1. **Capture both streams under one clock** — subscribe to the H10 from the device and log its RR next to wrist IBI (see §3). Eliminates the problem outright.
2. **Cross-correlate the two HR series** to recover the offset, then align.
3. **Shared sync event:** a firm tap on the sensor (spikes both PPG and ECG motion) or a breath-hold at the start as a fiducial marker.

### 2.3 Metrics & acceptance
- Per matched window (2–5 min): **RMSSD, SDNN, pNN50, mean HR**.
- Judge agreement with a **Bland–Altman** analysis (bias + limits of agreement), not just mean error — a small average error can hide large per-night scatter.
- Beat-level: **sensitivity / precision** of wrist beat detection vs H10 (matched / extra / missed beats).
- **Proposed acceptance (tune after S1):** on clean still windows, RMSSD bias < ~5 ms with tight limits of agreement, and beat-detection sensitivity & precision > ~98%. This is the quality bar for *surfacing* HRV with confidence — a check on the optional HRV feature, not a gate on any other phase.

### 2.4 Interpreting disagreement
- **Systematic bias** (wrist always reads high/low) → a personal correction factor or model; recoverable.
- **Random scatter on clean data** → the beat detector / sub-sample interpolation is the culprit; fix the DSP.
- **Disagreement only during motion** → expected; tighten SQI gating and report gaps rather than bad numbers. Coverage-with-honesty beats coverage-with-noise (PLAN.md §3.3).

---

## 3. H10 as an on-device reference (a development superpower)

The ESP32-S3 has a BLE central role, and the H10 broadcasts heart rate **and RR intervals** via the standard **BLE Heart Rate Service** (`0x180D` / Heart Rate Measurement `0x2A37`, RR field in units of 1/1024 s). That means the device itself can subscribe to the H10 during development.

**Why this is a big deal:**
- A dev-only **`refmon`** component logs H10 RR *next to* wrist IBI, **timestamped by the same device clock** — §2.2's alignment problem disappears by construction.
- It enables a **live on-screen "wrist RMSSD vs H10 RMSSD"** readout. Validation stops being an offline batch chore and becomes a number you watch *while* adjusting strap pressure, LED current, and filter constants — a massive iteration-speed win for the fiddliest optional feature.
- It cleanly seeds a future **calibration mode** feature: record paired wrist+H10 data on demand to auto-fit a personal PPG→HRV correction (FEATURES.md §12).
- (Advanced, optional: the H10 also streams raw ECG at 130 Hz over Polar's PMD service if beat-level ECG is ever wanted. RR over the standard service is the easy, sufficient path.)

Plan: add `refmon` as a **build-flag-gated dev component** (compiled out of release firmware). It reuses the same `sleep_hrv_rmssd()` from `sleep_core`, so wrist and reference are scored by identical math.

---

## 4. Analysis & test tooling (build alongside the spikes)

- **`tools/` (Python):** the offline toolkit now exists — `sleeplog`/`read_night` load and validate night logs, `gen_synthetic_night`/`score_night` prototype the scorer against ground truth, and `capture_ppg`/`analyze_ppg` run the raw-PPG capture + beat-detector loop (scipy/NeuroKit2). Extend these to match wrist vs H10 beats and produce Bland–Altman plots / hypnogram overlays for S1. See [tools/README.md](tools/README.md).
- **Golden test vectors:** capture a handful of labeled PPG snippets (clean, ectopic, motion) and freeze them as host-side unit tests for the beat detector, so future DSP changes are regression-checked on a laptop with no hardware in the loop.

---

## 5. How this sequences with PLAN.md

Run all the spikes up front, during / just before **Phase 1** — front-loading them is the whole point (learn the risk in week 1, not month 3). Each still retires a *different* phase's risk:

- **S1 → the HRV go/no-go** (PPG/HRV feasibility). Run it early, but it decides only the *optional* HRV feature, not the phase. **S2** (SQI / motion-robustness) matters for HR/SpO2 quality too.
- **S3, S4 → de-risk Phase 2** (raw-to-SD throughput and overnight power). S3 backs the Phase 2 double-buffered DMA-SD design in PLAN.md §5.1.
- **S5 → de-risks mechanical coupling** before the first full-night recording (Phase 2's exit criterion).

§2's protocol is the quality check for the optional HRV feature, made concrete — not a gate on any phase. §3's `refmon` is worth building early *if* you're pursuing HRV — it pays for itself across Phase 1–3.
