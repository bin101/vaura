# Vaura — Cycling Club Warning Device (XIAO ESP32-S3 + Wio-SX1262)

Standalone firmware for the case ["Meshtastic Seeed Studio XIAO ESP32S3 & Wio SX1262 case MOD with screen and INA219 voltage reader" by *Lead*](https://www.printables.com/model/1433873-meshtastic-seeed-studio-xiao-esp32s3-wio-sx1262-ca). Uses the Wio-SX1262 radio module in **GFSK mode**, not the namesake LoRa mode — see "Radio protocol" further down for the reasoning.
No Meshtastic on board — its own lean broadcast protocol, tailored to a group ride of 6–12 riders:

- **Hazard buttons**: car behind, hazard ahead, braking/stopping, regroup/wait
- **Automatic drop-off detection** via heartbeat + signal strength (no GPS needed)
- **Battery display** via the already-fitted INA219

## Hardware (as fitted in the case)

| Component | Detail |
|---|---|
| MCU | Seeed Studio XIAO ESP32-S3 |
| Radio | Wio-SX1262 (LoRa), plugged onto the XIAO (B2B connector) |
| Display | 0.96" OLED SSD1306, 128×64, I²C (two-tone panel: top 16 pixel rows yellow, rest blue) |
| Battery monitor | INA219, I²C, high-side |
| Battery | LiPo 803040, 1000 mAh |
| Button | 1× tactile |
| Piezo beeper | 1× piezo element, switched via NPN transistor (e.g. 2N2222) |

> **Region/antenna:** This firmware is fixed to **EU868 (868.3 MHz)** (see `src/config.h`). The linked case is tagged "915mhz" (US) — **verify before first power-on that an 868 MHz antenna is fitted**, otherwise range suffers noticeably. Transmit power is capped at 14 dBm (EU868 SRD limit).

## Build plan / wiring

Matches the case author's build 1:1 — **no new wiring needed** if you followed their instructions. Summarized here for traceability:

1. **LoRa:** plug the Wio-SX1262 onto the XIAO (board-to-board connector), screw on the 868 MHz antenna.
2. **I²C chain** (enamel/magnet wire, ~4 cm is enough): INA219 → OLED → XIAO, connecting **GND / 3V3 / SDA / SCL** through each.
3. **Power path / battery sensing:** battery(+) → INA219 **Vin+** (high side) → INA219 **Vin−** → on/off switch → XIAO **BAT+**. battery(−) directly to XIAO **BAT−**.
4. **Button:** one leg to GND (e.g. at the OLED), the other to **D3 / GPIO4**.
5. **Piezo beeper** (via NPN transistor as a low-side switch, e.g. 2N2222 — the GPIO can't source enough current for the piezo directly):
   - Piezo (+) → **3V3**
   - Piezo (−) → transistor **collector**
   - Transistor **base** → 1 kΩ resistor → **D1 / GPIO2** — **this resistor is not optional:** without it, the base-emitter junction (a diode with ~0.6–0.7 V forward voltage) sits practically directly on the 3.3 V GPIO with no current limiting. That can exceed the GPIO pin's maximum allowed output current and damage it.
   - Transistor **base** → additionally a 10 kΩ resistor → **GND** (pull-down, prevents a random beep at boot). Unlike GPIO3, GPIO2 is **not** an ESP32-S3 strapping pin (see box below) — this pull-down is purely a nicety here, not a requirement.
   - Transistor **emitter** → **GND**
   - No flyback diode needed (only relevant for inductive loads like relays/motors; a piezo is capacitive).
   - **Why D1/GPIO2 and not D2/GPIO3:** GPIO3 is one of the ESP32-S3's strapping pins (controls the JTAG signal source at boot) and has **no** internal pull resistor of its own — anything wired externally to GPIO3 counts as part of the boot-strapping circuit. GPIO2 has no such special role, making it the simpler choice for a piezo that just needs to switch on and off.
   - **Watch the pinout:** "2N2222" is not a single standardized pinout — PN2222A (TO-92) is usually E-B-C, while P2N2222A (TO-92) is C-B-E, left to right with the flat side facing you. Check the exact print on your own part before soldering, or verify with an hFE transistor tester socket on a multimeter.
6. **Build order:** solder all wires to the XIAO first, **then** glue components into the case (hot glue) — not the other way around, or you can no longer reach the solder pads.

### Pinout (fixed in `src/config.h`)

```
LoRa SPI:      SCK=GPIO7   MISO=GPIO8   MOSI=GPIO9   NSS=GPIO41
LoRa control:  DIO1=GPIO39  RESET=GPIO42  BUSY=GPIO40  ANT_SW=GPIO38
I2C (OLED+INA219): SDA=GPIO5  SCL=GPIO6
Button:        GPIO4 (D3), against GND, internal pull-up
Piezo beeper:  GPIO2 (D1), via NPN transistor (1kΩ base, 10kΩ pull-down)
```

These pins belong to the B2B "kit" (Wio-SX1262 plugged on) — **not** to the separately available Wio-SX1262 solder-in board, which uses different pins.

### Breadboard version for the first test (USB only, no power supply, no battery)

Before gluing anything into the case, a loose breadboard build is worth it — power comes exclusively from the USB cable to the computer, no power supply and (initially) no battery needed:

1. **Plug XIAO + Wio-SX1262 together** (board-to-board connector) and screw on the antenna — exactly as in the final build. Always screw on the antenna for the breadboard test too, before transmitting (otherwise you risk the PA output stage).
2. **Put the XIAO on the breadboard** (or leave it loose and connect everything via jumper wires) and connect via **USB-C to the computer** — this powers the XIAO through its own 3V3 regulator.
3. **Power the rails:** XIAO **3V3** → plus rail, XIAO **GND** → minus rail of the breadboard.
4. **OLED (SSD1306):** VCC → plus rail, GND → minus rail, SDA → GPIO5, SCL → GPIO6.
5. **INA219:** VCC → plus rail, GND → minus rail, SDA → GPIO5 (same I²C bus, parallel to the OLED), SCL → GPIO6. **Vin+/Vin−** stay open without a battery (or bridged with a wire) — the firmware still detects the INA219 over I²C and simply shows `0` or a meaningless voltage; that's normal and not a bug for this first test without a battery.
6. **Button:** one leg to the minus rail (GND), the other to GPIO4 (D3).
7. **Piezo beeper:** transistor base via 1 kΩ to GPIO2 (D1), plus 10 kΩ from the base to the minus rail, emitter to the minus rail, collector to piezo (−), piezo (+) to the plus rail (see "Build plan" above for the full explanation including the pinout caveat).
8. **BAT+/BAT− on the XIAO stay completely unconnected** as long as no battery is being tested.

This already lets you fully test the display, button, naming, and LoRa send/receive — only the battery display won't show a meaningful value until a real battery is connected. The full test from the [Verification](#verification) section (drop-off detection etc.) needs at least two devices built this way, as described there, each on its own USB port.

Once the breadboard test succeeds, a battery can optionally be added (battery(+) → INA219 Vin+ → Vin− → on/off switch → XIAO BAT+, battery(−) → XIAO BAT−, see step 3 above in the build plan) — after that the device also works without a USB cable.

### Reserved for later (not populated)

GPIO1 (D0, ADC-capable) and GPIO43/44 (D6/D7) stay free — room for extra buttons or a vibration motor in a later expansion stage, without touching the existing wiring. (GPIO2/D1 is now taken by the piezo beeper, see above. GPIO3/D2 deliberately left free/unconnected — strapping pin, see build plan.)

## Building & flashing the firmware

Requires: [PlatformIO](https://platformio.org/) (CLI or VS Code extension).

```bash
pio run                      # compile
pio run -t upload            # flash the USB-connected device
pio device monitor           # serial console / debug log (115200 baud)
pio test -e native           # unit tests, run on the computer (no hardware needed)
```

Every device gets **the same** firmware — there are no compile-time differences between devices.

**Versioning:** The project uses [Semantic Versioning](https://semver.org/) via git tags (`v0.1.0`, `v0.2.0`, …) and [Conventional Commits](https://www.conventionalcommits.org/) (`feat:`, `fix:`, `docs:` …). The firmware picks up its version automatically at build time from `git describe` (`FIRMWARE_VERSION`, e.g. `v0.1.0-5-gabc1234` between releases, with `-dirty` on local changes) and shows it at boot in the event line, in the stats screen header, in the serial boot line, and in the `status` output — so it's quick to check who's running which version at the meeting point.

The unit tests (in `test/`) cover the radio packet format (`src/protocol.cpp`: encode/decode roundtrips, malformed packets) and the battery voltage curve (`src/battery_curve.cpp`). On every push, GitHub Actions (`.github/workflows/ci.yml`) additionally builds the firmware and runs the tests.

## Flashing without PlatformIO: the Vaura Flasher

For users who **don't** want to compile it themselves, there's a GUI app (macOS/Linux/Windows): connect the device via USB-C, "Download latest version", select the port (the device is auto-detected and pre-selected), "Flash firmware" — done. All settings stored on the device (nickname, tone frequency, display auto-off, etc.) survive: the app writes the merged binary in two pieces and deliberately skips the NVS settings region (0x9000–0xDFFF).

**Download:** The ready-to-run apps are attached to every GitHub release: `Vaura-Flasher-macos-apple-silicon.zip`, `-macos-intel.zip`, `-linux-x86_64`, `-windows.exe` — plus `vaura-firmware.bin` (the merged binary the app loads; also flashable manually: `esptool --chip esp32s3 write_flash 0x0 vaura-firmware.bin` — **careful:** unlike the app, manually flashing the whole thing at 0x0 also overwrites the NVS region, all device settings must be set again afterward).

**Device settings without flashing:** The **"4. Device"** section in the app connects to the running firmware's serial console. "Read" shows **node ID, nickname, and firmware version** — proof that the expected device is really on the other end of the cable — and fills in the fields for name, channel, sensitivity, tone level, and display auto-off. "Apply" only writes the values that changed and reads back the actual device state. Note: connecting restarts the device (macOS driver behavior on the USB port) — the channel prompt appears again and the tour stats start at zero.

Platform notes for users:

- **macOS:** drag the app out of the zip; right-click → "Open" on first launch (unsigned app, Gatekeeper).
- **Windows:** confirm the SmartScreen warning with "More info" → "Run anyway".
- **Linux:** make it executable (`chmod +x`) and add the user to the `dialout` group (Debian/Ubuntu) or `uucp` (Arch), otherwise the serial port permissions are missing.
- If the device isn't detected: plug it in while holding the **BOOT button (B)** and flash again.

### Running / developing the flasher from source

```bash
# Get a Python with a current Tcl/Tk once. IMPORTANT (macOS): do NOT use
# /usr/bin/python3 -- its ancient Tk 8.5 no longer draws widgets on current
# macOS, the GUI stays an empty gray window.
brew install python-tk                             # Homebrew Python incl. tkinter/Tk >= 8.6
$(brew --prefix)/bin/python3 -m venv .flasher_venv
./.flasher_venv/bin/pip install -r flasher/requirements.txt
./.flasher_venv/bin/python flasher/flasher.py      # start the GUI
./.flasher_venv/bin/python flasher/flasher.py --check   # self-test without GUI, also checks the Tk version (used in CI too)
```

A standalone app (runnable without a Python install) is built with PyInstaller — **always only for the OS you're currently on** (PyInstaller can't cross-compile; that's why the release workflow builds on four runners):

```bash
./.flasher_venv/bin/pip install pyinstaller
# --collect-data esptool is required: the stub-flasher JSONs are package data
# that write_flash needs at runtime, otherwise it fails with "Flasher stub
# data is missing".
./.flasher_venv/bin/pyinstaller --onefile --windowed --collect-data esptool \
    --name Vaura-Flasher \
    --distpath dist --workpath build --specpath build flasher/flasher.py
# Result in dist/: Vaura-Flasher.app on macOS (double-clickable),
# a single binary or a .exe on Linux/Windows
```

### Publishing a release (maintainers)

1. **One-time:** create a public GitHub repo and set the `GITHUB_REPO` constant in `flasher/flasher.py` from the placeholder to `user/repo` — before that, the app can only flash local files.
2. Push a tag: `git tag v1.0.0 && git push origin main --tags`.
3. `.github/workflows/release.yml` then automatically builds the release: unit tests → firmware → `vaura-firmware.bin` (merged binary) + the four flasher apps as assets.

## Provisioning (once per device)

The node ID is derived automatically from the chip's MAC address — no configuration needed. Only the **nickname** needs to be set once, via the serial console (`pio device monitor`):

```
name ROB
```

Any short name, **letters only, max. 5 characters** (no digits — same rule as the on-device rename menu, see below) — **no real name needed**. The nickname is stored permanently in flash (survives restarts/dead batteries).

The console can now set **all** device settings (case-insensitive):

| Command | Effect |
|---|---|
| `help` | Overview of all commands |
| `id` | Show node ID |
| `status` | All settings, machine-readable, as `key=value` (name, ID, version, channel, sensitivity, tone step, display) |
| `name <NICKNAME>` | Set the nickname |
| `channel <0–9>` | Set the radio channel (applies immediately) |
| `sensitivity <0–10>` | Set the falling-back sensitivity level |
| `tone <0–10>` | Set the piezo tone level (plays a test beep) |
| `display <0\|15\|30\|60\|300>` | Display auto-off in seconds (0 = never) |
| `beep [Hz]` | Play a test tone |

If you don't have `pio device monitor` handy: the **Vaura Flasher** has the "Device" section for this (see the flasher section above) — same commands, with form fields.

**Tuning piezo volume:** `beep <Hz>` plays a ~600 ms test tone at the given frequency over the same console (e.g. `beep 3200`), without needing to reflash. There's no software volume knob with the current single-transistor drive (the piezo is only switched on/off, no in-between steps) — frequency is the only lever, since a piezo is loudest right at its mechanical resonance. The frequency actually in use is **no longer** a compile-time value — it's runtime-adjustable and stored in flash (default: 3000 Hz, adjustable range 2500–3500 Hz) — either directly on the device (see "Settings" below; shown there as a **level 0–10** on a ruler scale, level = (Hz − 2500) / 100, default 3000 Hz = level 5), or by first finding a good value with `beep <Hz>` and then applying it on the device.

The 5-character limit isn't a round number, it's calculated: the idle screen lists riders in up to **three columns**, and the status lives directly in the name there — `!NAME!` = signal fading, `(NAME)` = dropped off. A name decorated like this takes 5 + 2 = 7 characters, and three 7-character columns exactly fill the 21 characters that fit on one line at 128 px display width and 6 px/character (`u8g2_font_6x10_tf`). A longer name is **rejected** by the `name` command (not silently truncated), with an error message pointing at the limit. (A 6-character name stored under older firmware is automatically shortened to 5 on the first boot.)

> **Firmware update note:** The 5-character limit changed the radio packet format (protocol version 2). **v2 is radio-incompatible with v1** — both sides cleanly reject each other's packets at the version byte. All devices in the club need to be updated together.

## UI mockups

[`docs/ui-mockups.md`](docs/ui-mockups.md) shows **every** screen state from `ui.cpp` as an image: boot channel selection, idle screen (alone / 1-, 2-, and 3-column rider list / muted / battery hint), send menu, incoming warning, "still dropped off" prompt, settings, stats, tone and sensitivity (both with a ruler scale), display (auto-off timeout), channel, changing the name, and range test (normal + dropped off).

## Operation (one button)

**Channel prompt on power-on:** Right after boot, the device first asks for the radio channel (`Select channel:` with a visible `Starting in Ns` countdown), so you join the right group straight away. Short press cycles 0–9 (resets the countdown), long press confirms; with no input, the shown selection confirms itself after 10 s — an unattended restart (e.g. a battery dropout mid-ride) never gets stuck in the prompt. **The device sends no heartbeats until confirmed**, and an incoming warning only beeps without taking over the screen; a double click confirms (instead of blindly warning on a possibly wrong channel). A channel change clears the rider list — riders from the old channel would otherwise become false drop-offs a few minutes later.

**Double click = instant warning:** From **any** state (except the channel prompt, see above) — even with the display asleep, even mid-menu — a double click instantly sends the generic **ATTENTION!** warning, without a menu and without looking. Deliberately generic: a blind gesture must never claim something specific that could be wrong. (Side effect: since the firmware waits after every click for a possible second one, a single click reacts with a slight delay — the wait time is `BUTTON_CLICK_MS` in `src/config.h`.)

| State | Short press | Long press |
|---|---|---|
| Select channel (right after boot) | next channel (0–9), resets the countdown | confirms the channel, device starts radio operation |
| Idle screen | opens send menu | opens settings |
| Send menu | cycle through the next warning (4 warnings) | send the marked warning |
| Incoming warning | dismiss | (no function) |
| "Still dropped off" prompt | keep the rider (next reminder in 60 s) | remove the rider from the group |
| Settings | next item (Mute/Stats/Tone/Display/Sensitivity/Channel/Name/Test/Back) | open the marked item, or toggle Mute |
| Stats | (just keeps the display awake) | back to idle screen |
| Change name | cycle through characters (A–Z only) | confirm + advance (empty = space); saves at the end |
| Set tone | next level (0–10 on the ruler scale, +100 Hz with wrap), plays a test beep | saves the level, back to idle screen |
| Set display | next auto-off level (Never/15 s/30 s/1 min/5 min) | saves the level, back to idle screen |
| Set sensitivity | next level (0–10 on the ruler scale, with wrap) | saves the level, back to idle screen |
| Set channel | next channel (0–9, with wrap) | saves + switches immediately, back to idle screen |
| Range test | next rider | ends the test |

- Send menu with no activity → back to the idle screen after 6 s (prevents accidental sends).
- An incoming warning is automatically hidden after 15 s if not dismissed sooner.
- There's no acknowledgement (ACK) — an incoming warning is only shown locally and dismissed with a short press, nothing is sent back.
- **Sending can fail** (duty-cycle budget exhausted, or the radio module wasn't found at boot): the display then shows `! NOT sent` and beeps once, briefly — so you never falsely believe you've warned someone. The repeat copy that's sent anyway (400 ms later) acts as an automatic retry here and corrects the toast if it gets through.
- The piezo beeper has its own, distinctly recognizable pattern per event (audible regardless of display sleep state):
  - **Incoming warning** from someone else → 2× short.
  - **Falling back** (a rider is falling behind) → 3× short.
  - **Dropped off** (a rider is completely gone) → 1× long (~1 s).
  - **Drop-off reminder**: while a rider stays dropped off, 1× short every 60 s (the long tone can get lost in the wind). From the **second** reminder on, the idle screen becomes the prompt `Still dropped off: (NAME)` — **long press removes the rider from the group** (no longer counted, shown, or reminded), short press or a 15 s timeout keeps them. A removed rider comes back **automatically** as soon as their heartbeat arrives again (a regular `BACK` event). Open menus/warnings are never interrupted by the prompt.
  - Sending your **own** warning deliberately does **not** beep: you already see the toast/display yourself right then. (The only exception is the failure beep, see above.)
  - **Your own battery low** (< 3.5 V) → 1× short + a `!` prefix before your own battery percentage in the header (once per low episode, with 100 mV hysteresis against flapping).
- If a **fellow rider's battery** drops below the same threshold, `<Name> BATTERY` appears in the event line (no beep/wake — their device would otherwise eventually go dark "without comment" and look like a drop-off to everyone). The voltage comes from their heartbeat.
- Devices **without an INA219** show no battery percentage (instead of a misleading `0%`).
- No status LED fitted/driven — not visible in the closed case anyway; the piezo beeper takes on the "perceivable without looking at the display" role.

### Display sleep (power saving)

The OLED automatically turns off after an adjustable period of inactivity — default 30 s (`OLED_WAKE_MS` in `src/config.h`), changeable on the device under **Settings → Display** (levels: Never/15 s/30 s/1 min/5 min, stored in flash; "Never" = stays on permanently, costs noticeably more battery). The device keeps running in the background meanwhile (heartbeats, roster, duty-cycle budget — all independent of the display). It wakes up again on:
- every button press,
- an incoming warning,
- a falling-back or dropped-off event of a rider.

**Important:** if the display is currently off, the **first** button press (short or long) only wakes it, but triggers **no action** — so you never blindly send a warning or blindly open settings. Only the next press operates the menu normally.

### Idle screen: who's here, who's falling back?

The idle screen lists the riders directly, **weakest signal first** — the riders this device exists for are always visible at the top. The status lives in the name itself: `(NAME)` = dropped off — parenthesized like an absentee, always sorted to the very top; `!NAME!` = currently falling back (the same criterion as the falling-back beep, exclamation marks as an active warning), ` NAME` = all good. The header shows `active/total` **including yourself** (two riders out together = `2/2`) and your own battery level. On the two-tone panel, the header glows yellow (top 16 pixel rows); the rider list deliberately starts right below that color boundary.

The layout adapts to the group size:

- **up to 4 riders** — one row per rider, plus the smoothed RSSI (dBm; `---` when dropped off) and the battery level from their heartbeat (`--` without an INA219 there; when dropped off, the last known value stays — it reveals whether the device simply ran out of battery):
  ```
  (MAX)     ---  15%
  !LEA!    -108  62%
   ROB      -71   --
  ```
- **5–8 riders** — two name columns (no RSSI/battery numbers).
- **9+ riders** — three columns in a smaller font (5×7), up to 15 names.

A rider **removed from the group** via the "still dropped off" prompt disappears from the list and the counter until their heartbeat arrives again.

If not everyone fits on the display, **only the weakest are shown** — a strong signal doesn't need the space. The per-rider RSSI detail view remains available via the range test (Settings → Test).

### Settings: Mute, Stats, Tone, Display, Sensitivity, Channel, Name, Test (on the device)

Long press from the idle screen opens settings — handy on the road, without PlatformIO/a cable. Just like the send menu: **short press** cycles through the items (**Mute → Stats → Tone → Display → Sensitivity → Channel → Name → Test → Back → ...**, as many times as you like), **long press** opens or toggles the currently shown item:

- **Mute** → toggles all alert beeps (long press switches ON/OFF, you stay in the menu) — for a break/café stop. While active, the idle screen shows `MUTE` centered in the header. **Deliberately not persisted:** after a restart the device is always loud, so it never rides silently into the next outing unnoticed. The test tones in the tone menu deliberately stay audible.
- **Stats** → overview of the current tour: ride time since power-on, warnings sent and received, drop-offs. Deliberately not saved — every tour starts at zero.
- **Tone** → piezo beeper frequency, shown as a **level 0–10 on a ruler scale** (level = (Hz − 2500) / 100): short press advances one level (plays a 500 ms test tone each time), wraps back to level 0 at the top end. Long press saves permanently to flash.
- **Display** → display auto-off timeout: short press cycles through **Never/15 s/30 s/1 min/5 min**, long press saves permanently to flash (see "Display sleep" above).
- **Sensitivity** → sensitivity of the falling-back early warning, same **ruler scale 0–10** (default: level 5 in the middle). Each level shifts the warning threshold by 3 dB:

  | Level | Threshold | Meaning |
  |---|---|---|
  | 0 | −120 dBm | below the reception limit → practically off (drop-off detection stays active) |
  | 5 | −105 dBm | default (previous fixed behavior) |
  | 10 | −90 dBm | very early — warns even at a slight falling back |

  The second condition (a sustained drop of ≥6 dB below your own baseline) stays the same across all levels — the level only determines **at what signal strength** a warning fires. To calibrate in the field: the range test (below) shows live the value this logic judges.
- **Channel** → radio channel 0–9 of the group (the same one the boot-time channel prompt sets). **All devices on a ride must be on the same channel** — different channels can't hear each other at all (a channel mistake looks exactly like everyone dropping off). Long press saves, switches immediately, and clears the rider list (riders on the old channel would otherwise become false drop-offs). Default: channel 0. Useful when two groups are riding at the same time.
- **Name** → as described two sections below.
- **Test** → range test, see the next section.
- **Back** → leaves settings immediately, without changing anything.
- With no button press for 6 s it automatically goes back to the idle screen (in Tone/Sensitivity/Channel without saving — just like changing the name, see below).
- Alternatively, test via the serial console (`beep <Hz>`, see "Tuning piezo volume" above) — handy for finding a good value beforehand, before dialing it in permanently on the device.

### Range test (field test tool)

Settings → **Test** shows for one rider, live: large, the smoothed RSSI (the value the falling-back logic judges), below it the raw RSSI of the last heartbeat and its age. Short press switches the observed rider, long press exits. **No timeout, no display sleep** in this mode — a range walk takes minutes of glancing, and you explicitly started the mode. The exact tool for verifying the estimated ~400–600 m range and the `RSSI_FALLING_BACK_*` thresholds in the field (see [Verification](#verification)).

### Changing the name on the device (no laptop needed)

Once "Name" is confirmed with a long press in settings, the name is entered character by character:

- Start: all 5 positions are empty, entry starts at position 1.
- **Short press** cycles the marked character exclusively through `A`–`Z` — **no digits, no space** in this rotation.
- **Long press** confirms the current character (whether cycled or still untouched and empty) and jumps to the next position — which **always starts empty**. A space is thus created deliberately by long-pressing at a position without clicking first.
- For a **shorter name** (e.g. "ROB"): simply long-press twice in a row after the last desired character, without clicking in between — two empty positions in a row are recognized as "done", the name is saved immediately (right-trimmed).
- If all 5 positions are confirmed, it also saves automatically.
- Whenever the next long press would save (a second space in a row, or the 5th position is reached), the footer switches from `short=char long=OK` to the unambiguous hint **`long=done`**.
- With no button press for 6 s, the edit is **discarded** (nothing is saved) and it goes back to the idle screen.
- The 5-character limit is the same as for the `name` command over the serial console (see Provisioning above) — but here it can't even be exceeded, since only 5 positions are shown to cycle through.

## How drop-off detection works

Every device sends a heartbeat with nickname + battery voltage roughly every ~2 s (slightly randomized). Every receiver derives **two** smoothed signal-strength averages (RSSI EMAs) per rider from this: a fast one ("where the signal is right now") and a slow one as a baseline ("where it usually sits", time constant ~40 s):

- **"Falling back"**: the fast average has dropped below the **adjustable threshold** (Settings → Sensitivity, level 0–10 in 3 dB steps from −120 to −90 dBm; default level 5 = −105 dBm) **and** is ≥6 dB below the slow baseline → early warning on the idle screen. Comparing against the baseline (instead of the immediately preceding heartbeat) also catches **gradual** falling back — with only one EMA, the raw RSSI would have had to collapse by ~17 dB within a single heartbeat interval.
- **"Dropped off"**: no signal at all for ~5.8 s → a clear alert with a timestamp. The threshold (`DROPPED_OFF_TIMEOUT_MS`) deliberately budgets 2 full intervals **including send jitter** plus margin — a single lost heartbeat (e.g. because two half-duplex senders overlapped) doesn't trigger a false alarm this way; it takes two missing in a row.

Both are automatic, no button press needed. Thresholds live as constants in `src/config.h` (`RSSI_FALLING_BACK_*`, `RSSI_EMA_ALPHA_*`, `DROPPED_OFF_MISSED_INTERVALS`/`DROPPED_OFF_TIMEOUT_MS`) and can be re-tuned after initial field tests.

## Radio protocol (quick reference)

EU868, 868.3 MHz, **GFSK** (not LoRa) at 19.2 kb/s, ~10 kHz frequency deviation, 46.9 kHz RX bandwidth, Gaussian shaping (BT=0.5), a private sync word (not LoRaWAN). The **group channel** (Settings → Channel, 0–9) shifts the second sync-word byte — devices on different channels are invisible to each other; all still transmit on the same frequency and share the airtime. The duty cycle is tracked in software as a rolling 1-hour budget (`src/radio.cpp`) — transmissions are held back once the legally allowed 1% budget is exhausted.

**Why GFSK instead of LoRa:** A group ride only needs a few hundred meters of range — LoRa's core advantage (several kilometers) isn't needed here. At 19.2 kb/s, GFSK has a much shorter time-on-air per packet (~9 ms instead of ~82 ms for LoRa SF8), which lets the heartbeat run much more often within the same legal 1% duty-cycle budget (~2 s instead of ~15 s) — that's the lever that makes drop-off/gap detection faster. Estimated range at this bitrate: ~400–600 m (sensitivity ~−111 dBm vs. ~−127 dBm for LoRa SF8) — verify in the field before relying on it. Warnings run in the same GFSK mode (an SX1262 can't receive LoRa and GFSK at the same time).

**Note on duty-cycle calculation:** RadioLib's `getTimeOnAir()` only counts the raw payload bits for FSK, not preamble/sync word/length byte/CRC. `Radio::fullFrameTimeOnAirUs()` in `src/radio.cpp` computes the full frame itself so the 1% budget reflects the actual transmit state.

## Verification

With **at least 2 devices**:

1. `pio run -t upload` on both (all devices together — v2 is radio-incompatible with v1, see above), then set nicknames once each (console, or more conveniently: Flasher → "Device").
2. Power on both → the **channel prompt** appears with a countdown; long press (or wait 10 s) confirms channel 0. The event line then briefly shows the firmware version (`FW v0.1.x`), and after a few seconds the respective other rider appears in the list (with RSSI), along with `2/2` in the header (the counter includes your own device).
3. Short press → cycle through → long press on "CAR BEHIND" → the other device shows the warning + beeps 2× short. Short press there dismisses it again.
4. **Double click** on one device (even with the display asleep) → the other immediately shows `ATTENTION!`.
5. Leave the devices next to each other for ≥ 30 minutes → **no** spontaneous `DROPPED`/`BACK` events in the event line (that was exactly the false-alarm bug from the 4 s threshold without a jitter margin).
6. Turn one device off / move it far away → after ~6 s, the other shows `<Name> DROPPED` in the last-event line (and the display wakes automatically for it, if it was asleep); with gradual distance, `<Name> WEAK` shows first. In the list, the rider moves to the top as `(NAME)`.
7. Enable **Mute** (Settings) → an incoming warning appears without a beep, the header shows `MUTE`. Turn it off again afterward.
8. **Drop-off reminder**: leave one device off → after ~60 s, 1 short reminder beep; after ~120 s, the prompt `Still dropped off: (NAME)` → long press removes the rider (disappears from the list and counter, no more reminders). Turn the device back on → the rider comes back automatically with `BACK`.
9. **Channel test**: set one device to channel 1 (Settings → Channel) → the devices lose each other (`DROPPED` after ~6 s); back to 0 → `BACK`.
10. **Sensitivity**: set it to level 10 and put one device in another room → `WEAK` comes noticeably earlier than at level 5; at level 0, `WEAK` no longer occurs at all, only the drop-off.
11. **Stats** (Settings) → ride time is running, warning/drop-off counters match the previous test steps.
12. Only once these steps work on the table, test in the field (a real ride) — that's what the **range test** (Settings → Test) is for: read the actual GFSK range live over the distances relevant in a group ride (the ~400–600 m is a datasheet estimate, not a field measurement), and fine-tune the heartbeat interval/RSSI thresholds as needed.

## Open items for later

- Verify the 868 MHz antenna before the first transmit test.
- Optional: AES encryption of packets (currently off, format in `src/protocol.h` prepared for a later extension).
- Expansion stage 2: dedicated buttons per warning (case model change) + a vibration motor (the piezo beeper is already implemented), so alerts are perceivable without looking at the display; possibly GPS for real distance readings, an SOS function, fall detection.
