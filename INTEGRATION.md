# Home Assistant & CPAP Integration

**Long-term vision (v3+).** The wrist tracker becomes one node in a larger sleep-health system centered on Home Assistant (HA). The wrist device and the CPAP machine both feed their data into HA; HA merges them; and a **combined night summary — wrist metrics *plus* CPAP therapy data — is displayed back on the wrist AMOLED**. This document sketches the architecture so earlier phases don't paint us into a corner.

> Status: design/vision. Nothing here is required for the standalone device (Phases 0–4). It's captured now so the firmware's data model, Wi-Fi stack, and sync layer are built with this endpoint in mind.

---

## 1. Core principle: Home Assistant is the hub

The wrist device should **not** talk to the CPAP directly. CPAP data paths are proprietary and awkward (cloud APIs, encrypted SD formats), and a wrist-worn device is the wrong place to parse them. Instead, every source publishes to HA, and the wrist pulls a merged result back:

```
   ┌─────────────────────┐         Wi-Fi / MQTT         ┌──────────────────────┐
   │  Wrist Sleep Tracker │  ── HR, HRV, SpO2, stages ──►│                      │
   │  (ESP32-S3 + MAX30102)│ ◄── merged night summary ── │   Home Assistant     │
   └─────────────────────┘                               │   (MQTT broker +     │
                                                          │    automations)      │
   ┌─────────────────────┐   CPAP integration (HACS)     │                      │
   │  CPAP machine        │  ── AHI, leak, pressure, ────►│                      │
   │  (e.g. ResMed)       │     usage, events            │                      │
   └─────────────────────┘                               └──────────────────────┘
                                                                    │
                                                          InfluxDB + Grafana,
                                                          phone dashboards, alerts
```

**Why this way:** the CPAP↔HA problem is already solved by the community, HA already speaks to hundreds of device types, and it gives us persistent history, dashboards, and automations for free. The wrist device only has to speak one protocol (MQTT) and stay dumb about CPAP internals.

---

## 2. How the CPAP data reaches HA (user's side, off-device)

This happens entirely in Home Assistant — the wrist firmware never touches it. Two established paths, both landing CPAP metrics as HA sensors:

| Path | How it works | Latency | Notes |
|---|---|---|---|
| **Local SD / ezShare** (e.g. [`hms-cpap`](https://github.com/hms-homelab/hms-cpap)) | Reads the CPAP's SD card (OSCAR-compatible DATALOG) over an ezShare Wi-Fi SD card or filesystem; publishes 47+ metrics to HA via MQTT auto-discovery | Near-real-time after mask-off | Richest data (AHI, per-event, leak, pressure, flow), fully local, no cloud |
| **Cloud myAir** (e.g. [`resmed_myair_sensors`](https://github.com/prestomation/resmed_myair_sensors)) | Pulls daily summary from ResMed's myAir cloud via an undocumented API (HACS custom component) | Hours (next-morning) | Summary only (AHI, usage, mask on/off, leak); depends on ResMed not breaking the API |

**Design consequence for us:** CPAP data is often *delayed* (cloud) or arrives only *after mask-off* (SD). The wrist can't assume CPAP numbers are ready at wake time — see §5 on the "pending" state.

CPAP metrics we care about pulling into the combined summary: **AHI** (apnea-hypopnea index — the headline), **usage hours** (mask-on time), **mask leak** (avg / 95th percentile), **pressure** (median / 95th percentile), and **event breakdown** (obstructive vs central apneas, hypopneas), ideally with **event timestamps** for correlation.

---

## 3. Transport: MQTT with HA auto-discovery

MQTT is the right fit for the wrist↔HA link: lightweight, bidirectional, natively supported by HA, and it works cleanly on the ESP32-S3 with the standard `esp-mqtt` component.

- **Firmware approach:** use native ESP-IDF `esp-mqtt` and publish HA **MQTT Discovery** config topics ourselves (they're just JSON messages on `homeassistant/<component>/<id>/config`). This gives auto-appearing HA entities *without* adopting ESPHome — ESPHome is great for simple sensors but would fight our custom LVGL/DSP firmware. (If we ever want a quick prototype, an ESPHome external component is a fallback, but native is the plan.)
- **Retained topics + LWT:** publish the last night's summary as a retained message so HA has it after restarts; use an MQTT Last-Will-and-Testament for device-online status.
- **Security:** local broker on the LAN, MQTT over TLS, per-device credentials. No cloud dependency for the wrist↔HA path; everything stays on the user's network.

**Bonus wins from being on Wi-Fi/HA** (worth building even before CPAP): **NTP time sync** to correct RTC drift, **OTA firmware updates**, and remote config (alarm time, thresholds) pushed from HA.

### 3.1 Topic layout (draft)

```
Uplink   (wrist → HA):
  sleeptracker/<id>/status                 online | offline   (retained, LWT)
  sleeptracker/<id>/live/hr                live HR while awake/charging
  sleeptracker/<id>/live/spo2
  sleeptracker/<id>/summary/sleep          JSON: TST, efficiency, score, stages, RHR, HRV, SpO2 min (retained)
  homeassistant/sensor/sleeptracker_<id>_*/config   discovery configs

Downlink (HA → wrist):
  sleeptracker/<id>/summary/cpap           JSON: AHI, usage, leak, pressure, events (published by HA automation)
  sleeptracker/<id>/summary/combined       JSON: merged card the wrist renders directly (optional convenience)
  sleeptracker/<id>/cmd/alarm              set/update alarm time
  sleeptracker/<id>/cmd/config             thresholds, brightness, etc.
```

---

## 4. Data model

**Uplink — the wrist publishes its own night summary** (superset of the on-device epoch record from PLAN.md §3.1): sleep window, total sleep time, efficiency, sleep score, stage durations + hypnogram, resting HR, nightly HRV (RMSSD/SDNN with beat-acceptance %), SpO2 min/mean and desaturation count, movement/position summary, battery.

**Downlink — HA publishes the CPAP summary** (and optionally a pre-merged combined card) after both sources are available. An HA automation is the merge point: it waits for the wrist summary + the CPAP integration's daily values for the same date, joins them, and publishes to `summary/cpap` (and/or `summary/combined`).

**Correlation-grade data (stretch):** to do event-level correlation (§6) rather than just daily-summary display, the wrist should log its per-epoch SpO2/HR/movement time series (already going to SD per PLAN.md) and CPAP event timestamps should be available in HA. The heavy correlation can run in HA/Grafana or an offline notebook; the wrist only needs the daily rollup for its display.

---

## 5. The combined night summary on the wrist (the headline feature)

On wake (or via a "sync" pull-to-refresh), the wrist requests/reads the merged summary and renders a card that no single device could produce alone:

```
┌─────────────────────────────┐
│  Tue · 23:10–06:45          │
│  Sleep Score       82       │   ← wrist
│  Asleep  7h02   Eff 91%     │   ← wrist
│  ▁▂▅▂▁▃▂ hypnogram          │   ← wrist
│  RHR 54  HRV 48ms  SpO2 92% │   ← wrist
│  ───────── CPAP ─────────   │
│  AHI       2.1   ✅         │   ← from HA
│  Usage     7h10             │   ← from HA
│  Leak      12 L/min         │   ← from HA
│  Pressure  9.4 / 11.2       │   ← from HA
│  "3 SpO2 dips lined up with │   ← combined insight
│   residual events"          │
└─────────────────────────────┘
```

**Handling CPAP-data latency:** show the wrist half immediately at wake; the CPAP half renders as "pending…" until HA delivers it (near-instant on local SD, up to hours on cloud myAir), then the card updates in place. A small "CPAP synced ✓ / pending" indicator sets expectations.

---

## 6. What this level of integration unlocks (the reason to do it)

Combining wrist biometrics with CPAP therapy data is where the real value lives — several of these need *both* devices:

1. **Positional apnea detection** *(wrist-only capability the CPAP lacks)* — cross the wrist IMU's sleep-position log with CPAP event timestamps: "your AHI is 4× higher when sleeping on your back." Neither device can find this alone. Highest-value combined feature.
2. **Second-sensor efficacy check** — overlay CPAP residual-event timestamps on the wrist's SpO2/HR traces. Do desats and HR spikes still coincide with events, or is therapy fully controlling them?
3. **Therapy → recovery trends** — track HRV, deep-sleep %, and resting HR against nightly AHI and usage over weeks. Does better mask seal / higher compliance actually improve measured recovery?
4. **Leak vs. awakenings** — correlate high-leak periods (CPAP) with wrist-detected awakenings/restlessness to catch mask-fit problems behaviorally.
5. **Mask-off vs. asleep** — wrist says asleep but CPAP reports mask off → gentle nudge; or flag therapy gaps.
6. **Compliance & routine automations** (HA side) — usage streaks, bedtime reminders, "you slept but didn't wear the mask" alerts, morning dashboard, share-with-doctor exports.

Positional apnea (#1) and the second-sensor efficacy check (#2) are the marquee outcomes — they're the payoff that justifies the whole integration.

---

## 7. Rough roadmap

Sequenced after the standalone device works (PLAN.md Phases 0–4). Each step is independently useful:

- **I0 — Wi-Fi + MQTT online.** `esp-mqtt`, connect to LAN broker, publish `status` + live HR, NTP time sync. *(Also delivers OTA groundwork.)*
- **I1 — Uplink summary + HA discovery.** Publish the nightly sleep summary with MQTT auto-discovery so wrist metrics appear as HA sensors → history/Grafana for free.
- **I2 — Downlink CPAP summary.** HA automation merges the user's existing CPAP integration data with the wrist summary and publishes it back; wrist renders the combined card (§5).
- **I3 — Combined insights.** Daily-summary correlations on the card (AHI vs SpO2 dips, etc.); remote config/alarm from HA.
- **I4 — Event-level correlation.** Positional apnea + second-sensor efficacy from time-series joins (HA/Grafana/offline notebook).

---

## 8. Open questions (for when we get here)

1. **Which CPAP + integration path** does the user run — local SD/ezShare (richer, faster) or cloud myAir (simpler, delayed)? Determines available metrics and latency. *(User has a CPAP to integrate; exact model/path TBD.)*
2. **Merge location** — do the join in an HA automation (publish a ready-made `combined` card) or on the wrist (subscribe to raw `cpap` + `sleep` and merge in firmware)? HA-side is simpler for the device; firmware-side is more self-contained. Leaning HA-side.
3. **Retention** — how many nights to keep on-device for the history view vs. lean entirely on HA/InfluxDB for long-term trends.
4. **Privacy** — keep everything on-LAN (local broker, no cloud) as the default stance; the only cloud touchpoint is the user's own CPAP integration if they choose myAir.
