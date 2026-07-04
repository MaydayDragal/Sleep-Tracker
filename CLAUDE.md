# CLAUDE.md — working agreements for this repo

This project is worked on from **multiple computers and separate Claude sessions**, so these conventions live in the repo (not machine-local memory) to keep behavior consistent everywhere.

**Starting a session? Read [HANDOFF.md](HANDOFF.md) first** — current state, next steps, and hardware gotchas.

## Keep the UI mockup in sync with the on-device UI

Whenever you change the on-device watch UI (`firmware/components/ui/**` — screens, tiles, controls, layout, labels), also update **[`docs/watch-ui-mockup.html`](docs/watch-ui-mockup.html)** — the versioned design reference showing every screen at the real 368×448 — to match, and commit the mockup **in the same commit/PR** as the firmware change. Don't let the example drift behind the device.

A committed **Stop hook** (`.claude/settings.json`) is a backstop: it prints a reminder when `firmware/components/ui/` has uncommitted changes without a matching `docs/watch-ui-mockup.html` change. The hook only *nudges* — doing the actual mockup redesign is the model's job. (The device carries a dev-only **PPG-debug tile** that the product mockup intentionally omits — that one gap is fine.)

## Branch off `origin/main`, after fetching

PRs merge on GitHub and the local clone is not auto-pulled, so local `main` falls behind fast (it has silently been *many* merged PRs stale). **Before branching or committing:** `git fetch origin main`, check `git log --oneline HEAD..origin/main`, and branch off **`origin/main`** — never stale local `main`.

## Build gotchas (see [firmware/README.md](firmware/README.md))

- Pinned `platform = espressif32@6.11.0` (ESP-IDF 5.4.1 / GCC 14.2) — do **not** bump to 7.0.x. Also builds on native ESP-IDF v6.0.2 (GCC 15.2).
- Required workarounds: `-Os` (`CONFIG_COMPILER_OPTIMIZATION_SIZE`) and `-Wno-error=format`.
- The watch UI needs larger Montserrat fonts + `CONFIG_LV_USE_CLIB_MALLOC` in `sdkconfig.defaults` (the default 64 KB LVGL pool is exhausted by the ~200-widget UI). These are choice/aggregate Kconfig symbols — after editing `sdkconfig.defaults`, run `rm firmware/sdkconfig.esp32s3-amoled` and rebuild so they take effect.
- Build/flash: `pio run -d firmware -e esp32s3-amoled [-t upload -t monitor]`; board enumerates as **COM6**. If USB writes time out / the port flaps, **swap the USB-C cable first**.
