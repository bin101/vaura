# CLAUDE.md

Orientation doc for AI coding assistants working in this repo. For hardware assembly, wiring,
full user-facing behavior, and field verification steps, see [README.md](README.md) — this file
is deliberately a summary, not a duplicate.

## What this is

**Vaura** is standalone firmware for a peer-to-peer cycling-group warning device: a Seeed XIAO
ESP32-S3 + Wio-SX1262 (GFSK radio, not LoRa mode) + SSD1306 OLED + INA219 battery monitor, in a
3D-printed case with one button and a piezo beeper. Riders broadcast hazard warnings to each other
and get automatic drop-off/falling-back alerts based on heartbeat signal strength — no GPS, no
phone, no LoRaWAN infrastructure. A companion Python/Tk GUI (`flasher/`) flashes prebuilt releases
and reads/writes device settings over the serial console, for club members who don't want to
compile anything.

## Repository layout

| Path | Purpose |
|---|---|
| `src/protocol.{h,cpp}` | Wire format: heartbeat + warning packets, versioned (`kVersion`) |
| `src/roster.{h,cpp}` | Peer tracking: dual-EMA RSSI trend, falling-back/dropped-off detection |
| `src/ui.{h,cpp}` | OLED rendering + single-button state machine (largest file, ~1200 lines) |
| `src/config.{h,cpp}` | Pin map, tunable constants, persisted per-device settings (NVS), serial console |
| `src/power.{h,cpp}` | INA219 battery reading, low-battery latch |
| `src/radio.{h,cpp}` | SX1262 GFSK wrapper, EU868 duty-cycle budget |
| `src/stats.{h,cpp}` | Per-tour counters (not persisted) for the stats screen |
| `src/battery_curve.{h,cpp}` | LiPo voltage → percent curve, shared by local + peer battery display |
| `src/main.cpp` | Entry point wiring the modules together |
| `flasher/flasher.py` | Standalone GUI flasher + device-settings tool (esptool-based) |
| `docs/ui-mockups.md` + `docs/mockups/*.svg` | One rendered mockup per UI screen state; regenerate via `docs/mockups/generate.py` |
| `test/` | Native unit tests (protocol roundtrips, battery curve) — no hardware needed |
| `.github/workflows/` | CI (build + test on push) and release (tag-triggered firmware + flasher builds) |

## Build, test, flash

```bash
pio run                      # compile firmware
pio run -t upload            # flash the USB-connected device
pio test -e native           # native unit tests, no hardware needed
pio device monitor           # serial console (115200 baud)
```

Flasher: `./.flasher_venv/bin/python flasher/flasher.py --check` (self-test, no GUI). On macOS,
the venv's Python needs Tk ≥ 8.6 (`brew install python-tk`) — the system Python's Tk 8.5 renders
an empty gray window.

## Conventions

- **Language:** everything is English — code, comments, docs, commit messages, and all
  user-facing product text (OLED screens, serial console, flasher GUI). No German.
- **Versioning:** [Semantic Versioning](https://semver.org/) via git tags (`v0.1.0`, ...) +
  [Conventional Commits](https://www.conventionalcommits.org/) (`feat:`, `fix:`, `docs:`,
  `refactor:`, `chore:` ...). `FIRMWARE_VERSION` is injected at build time from
  `git describe --tags` (see `platformio.ini`).
- **No AI attribution in commits.** Do not add `Co-Authored-By` or session-link trailers.
- **Wire-format changes require a `Protocol::kVersion` bump.** The protocol intentionally rejects
  mismatched versions symmetrically (see the comment on `kVersion` in `src/protocol.h`) rather
  than degrading silently — a version bump means the whole fleet must be reflashed together.
- **Display-only string changes** (menu labels, warning text) do **not** need a version bump —
  only the wire-format bytes are versioned, not what's printed on screen.

## Persisted settings (NVS, namespace `"warndevice"`)

| Setting | NVS key | Range | Console command |
|---|---|---|---|
| Nickname | `nickname` | 5 letters, no digits | `name <NAME>` |
| Beep frequency | `beepHz` | 2500–3500 Hz (100 Hz steps) | `tone <0-10>` (level, not Hz) |
| Display auto-off | `dispMs` | 0 (never), 15s, 30s, 1min, 5min | `display <0\|15\|30\|60\|300>` |
| Falling-back sensitivity | `sensLvl` | 0–10 (maps to −120..−90 dBm) | `sensitivity <0-10>` |
| Radio group channel | `channel` | 0–9 | `channel <0-9>` |

The `status` console command prints all of these as machine-readable `key=value` lines; the
flasher's "Device" panel parses that exact format (`SETTINGS_KEYS` in `flasher/flasher.py`) — keep
both sides in sync if the key names ever change.

## Working notes

- `src/ui.cpp`'s state machine has four places that must stay in sync for every `State` enum
  value: the `render()` switch, both button-handler switches (`onButtonClick`/
  `onButtonLongPress`), and the `tick()` timeout chain — the last one is a plain `if`-chain, not a
  `switch`, so the compiler won't catch a missed case there.
- The mounted SSD1306 is a **two-tone panel**: pixel rows 0–15 render yellow only, 16–63 blue.
  Headers/separators live in the yellow strip; all content must start at row ≥16 (see the comment
  above `renderIdle()` in `ui.cpp`).
- `docs/mockups/generate.py` hardcodes each screen's example copy (names, RSSI values, etc.) —
  re-run it and hand-edit the values after a layout or copy change in `ui.cpp`.
