// Pin map, radio/timing constants and per-device configuration (nickname, node id).
//
// Pin sources (verified against Seeed docs / RadioLib maintainer discussion for the
// XIAO ESP32S3 + Wio-SX1262 B2B "kit" combo -- NOT the same as the solder-in Wio-SX1262
// module, which uses different pins):
//   https://wiki.seeedstudio.com/wio_sx1262_with_xiao_esp32s3_kit/
//   https://github.com/jgromes/RadioLib/discussions/1361
#pragma once

#include <Arduino.h>

// Injected by platformio.ini from `git describe --tags` (SemVer tags, e.g.
// "v0.1.0" or "v0.1.0-3-g8f0e765-dirty" between releases). Fallback for
// build paths that don't pass the flag.
#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION "dev"
#endif

// ---------------------------------------------------------------------------
// LoRa radio (SX1262 on the Wio-SX1262 board, connected via B2B connector)
// ---------------------------------------------------------------------------
#define PIN_LORA_SCK 7
#define PIN_LORA_MISO 8
#define PIN_LORA_MOSI 9
#define PIN_LORA_NSS 41
#define PIN_LORA_DIO1 39
#define PIN_LORA_RESET 42
#define PIN_LORA_BUSY 40
#define PIN_LORA_ANT_SW 38 // single-pin RF switch, shared RX/TX enable

// The Wio-SX1262 uses a 1.8V TCXO, not a plain crystal -- must be told to RadioLib
// explicitly, otherwise the radio never leaves standby.
#define LORA_TCXO_VOLTAGE 1.8f

// ---------------------------------------------------------------------------
// I2C peripherals: 0.96" SSD1306 OLED + INA219 battery monitor, same bus
// ---------------------------------------------------------------------------
#define PIN_I2C_SDA 5
#define PIN_I2C_SCL 6
#define OLED_I2C_ADDRESS 0x3C
#define INA219_I2C_ADDRESS 0x40

// ---------------------------------------------------------------------------
// Single control button (as wired in the printed case: one leg to GND, one to D3)
// ---------------------------------------------------------------------------
#define PIN_BUTTON 4
// With a double-click handler attached, OneButton must wait this long after a
// click before reporting it as a single click -- every single click therefore
// reacts with exactly this delay. Shortened from OneButton's 400 ms default
// so the send menu feels immediate; go lower only after testing that fast
// double-clicks (the blind ACHTUNG gesture) are still recognized reliably.
#define BUTTON_CLICK_MS 175
// How long the button must be held before it counts as a long press
// (OneButton's own default would be 800 ms). Lower feels snappier in the
// menus but narrows the gap to a slow "normal" click -- with winter gloves a
// deliberate short press easily lasts 300+ ms, so keep a comfortable margin
// above BUTTON_CLICK_MS.
#define BUTTON_LONG_PRESS_MS 500

// ---------------------------------------------------------------------------
// Piezo beeper, driven through an NPN transistor (e.g. 2N2222) as a low-side
// switch -- the GPIO can't source enough current for the piezo directly, and
// this also keeps the piezo off ESP32's own 3.3V logic-level current budget.
// GPIO2 (D1), not GPIO3 (D2): GPIO3 is an ESP32-S3 strapping pin (JTAG source
// select at boot) with no internal pull resistor of its own -- anything wired
// to it needs to be considered as part of the boot-strapping circuit. GPIO2
// has no such role, so a beeper on it can't interfere with boot.
// ---------------------------------------------------------------------------
#define PIN_PIEZO 2
// Default/fallback frequency -- the actual frequency in use is user-adjustable
// at runtime (idle screen: long-press -> settings rotation -> "Ton") and
// persisted in flash, see DeviceConfig::beepFrequencyHz() below. 3000 Hz sits
// near the peak of human ear sensitivity and clears typical cycling wind
// noise, which is low-frequency-dominated -- see README "Piezo-Lautstaerke".
#define BEEP_FREQUENCY_HZ 3000
#define BEEP_FREQUENCY_MIN_HZ 2500
#define BEEP_FREQUENCY_MAX_HZ 3500
#define BEEP_FREQUENCY_STEP_HZ 100
#define BEEP_TEST_DURATION_MS 500UL // sample playback while adjusting in the Ton menu
#define BEEP_DURATION_MS 80UL // length of one short beep
#define BEEP_GAP_MS 80UL // silence between beeps within one beep sequence
// Beep pattern per event, distinct so it's tellable apart by ear alone:
#define WARNING_BEEP_COUNT 2 // incoming warning from another rider
#define FALLING_BACK_BEEP_COUNT 3 // SCHWACH -- peer's signal is fading
#define DROPPED_OFF_BEEP_DURATION_MS 1000UL // ABRISS -- one long tone, not a count

// ---------------------------------------------------------------------------
// GFSK RF parameters -- EU868. Switched from LoRa to (G)FSK because this
// device only needs a few hundred metres of range (peloton spacing, not
// point-to-point long-range), and GFSK's much shorter time-on-air lets the
// heartbeat run far more often within the same legal duty-cycle budget --
// which is what actually drives how fast a gap/drop-off is detected. See
// README.md "Funkprotokoll" for the range/rate trade-off and the numbers
// behind the chosen bitrate.
// ---------------------------------------------------------------------------
#define GFSK_FREQUENCY_MHZ 868.3f
#define GFSK_BITRATE_KBPS 19.2f
#define GFSK_FREQUENCY_DEVIATION_KHZ 10.0f
#define GFSK_RX_BANDWIDTH_KHZ 46.9f // one of the SX126x's fixed RxBw steps; must match exactly
#define GFSK_TX_POWER_DBM 14 // EU868 SRD band limit is 14 dBm ERP for this channel
#define GFSK_PREAMBLE_LENGTH_BITS 16
// Private-network sync word, distinct from RadioLib's own FSK default ({0x12, 0xAD}).
// Reuses the old LoRa magic/syncword bytes for continuity, not for any technical reason.
// The group channel (0..9, settings menu, NVS) is added onto the SECOND byte:
// 0x51 + channel = 0x51..0x5A, all distinct from the RadioLib default. Devices
// on different channels are mutually invisible at the radio level -- the whole
// group must agree on one. Channel 0 == this historical base sync word, so
// already-provisioned devices keep hearing each other after an update.
#define GFSK_SYNC_WORD_BYTES {0xC9, 0x51}
#define GFSK_SYNC_WORD_LEN 2
#define GROUP_CHANNEL_MAX 9
#define GFSK_CRC_LEN_BYTES 2 // used both for radio.setCRC() and the airtime math in radio.cpp

// EU868 legal duty-cycle limit for this sub-band. The radio module enforces this
// itself via a rolling token bucket -- see radio.cpp.
#define LORA_DUTY_CYCLE_FRACTION 0.01f

// ---------------------------------------------------------------------------
// Protocol timing
// ---------------------------------------------------------------------------
// 2s was chosen so that heartbeat airtime alone stays well under half the 1%
// duty-cycle budget (~0.45% at 19.2 kb/s for a 13-byte packet, see radio.cpp
// fullFrameTimeOnAirUs()), leaving headroom for WARNING bursts.
#define HEARTBEAT_INTERVAL_MS 2000UL
#define HEARTBEAT_JITTER_MS 400UL // avoids all nodes transmitting in lockstep
#define WARNING_REPEAT_DELAY_MS 400UL // 2nd copy of a WARNING, for loss resilience

// A peer is "falling back" once its fast-smoothed RSSI is below the floor AND
// at least RSSI_FALLING_BACK_DROP_DB below the slow baseline EMA. Two EMAs
// instead of comparing the fast EMA against its own previous value: with a
// single EMA (alpha 0.35) the raw RSSI had to collapse by ~17 dB within one
// 2 s heartbeat for the smoothed value to move the required 6 dB -- a rider
// drifting off *gradually* (the actual early-warning use case) never fired
// and went straight to ABRISS. The slow EMA (alpha 0.05, time constant ~20
// heartbeats = ~40 s) is "where the signal usually sits"; the fast one is
// "where it is right now".
#define RSSI_FALLING_BACK_FLOOR_DBM -105
#define RSSI_FALLING_BACK_DROP_DB 6
#define RSSI_EMA_ALPHA_FAST 0.35f
#define RSSI_EMA_ALPHA_SLOW 0.05f

// The floor is user-adjustable in 11 steps (0..10) via the settings menu
// ("Empfindlich"), persisted in NVS -- see DeviceConfig::fallingBackSensitivity().
// Each step shifts the floor by 3 dB; step 5 is the historical fixed value:
//   step 0  -> -120 dBm: below the receiver's sensitivity (~-111 dBm), SCHWACH
//              effectively never fires -- a clean "practically off" endpoint
//              (ABRISS detection is unaffected).
//   step 10 -> -90 dBm: for distant riders the floor is almost always met, so
//              the check degenerates -- deliberately -- into the pure trend
//              detector ("warn on any 6 dB sag against the baseline").
// The RSSI_FALLING_BACK_DROP_DB condition stays FIXED across all steps:
// shrinking it would trip on normal multipath wobble (underpass, truck,
// corner), and one explainable degree of freedom is all a one-button menu
// can reasonably carry.
#define FALLING_BACK_SENSITIVITY_DEFAULT 5
#define FALLING_BACK_SENSITIVITY_MAX 10 // steps 0..10 = 11 ruler positions
constexpr int16_t fallingBackFloorDbm(uint8_t level) {
  return static_cast<int16_t>(-120 + 3 * static_cast<int>(level));
}
static_assert(fallingBackFloorDbm(FALLING_BACK_SENSITIVITY_DEFAULT) == RSSI_FALLING_BACK_FLOOR_DBM,
              "step 5 must equal the historical fixed floor");
// The tone menu shares the same 0..10 ruler scale (step = (Hz - min) / step
// width) -- if either range changes, the two menus drift apart visually.
static_assert((BEEP_FREQUENCY_MAX_HZ - BEEP_FREQUENCY_MIN_HZ) / BEEP_FREQUENCY_STEP_HZ ==
                  FALLING_BACK_SENSITIVITY_MAX,
              "tone and sensitivity menus share the same 0..10 ruler scale");

// A peer is considered fully dropped ("Abriss") once its heartbeat has been
// missing for this many intervals. Kept at 2 rather than 1: a single missed
// heartbeat is often just a collision on a shared channel with several peers
// (all nodes are half-duplex -- two overlapping transmissions silently lose
// both), not a real drop -- requiring 2 in a row filters that out.
#define DROPPED_OFF_MISSED_INTERVALS 2
// The actual silence threshold must budget for the sender's jitter, not just
// the nominal interval: with one heartbeat lost, the legitimate gap is up to
// DROPPED_OFF_MISSED_INTERVALS * (interval + jitter) = 4800 ms. Judging
// against 2 * interval = 4000 ms (as an earlier version did) made every
// single collision a coin-flip false ABRISS. The extra half interval covers
// Roster::tick()'s 1 s granularity plus receive/dispatch latency. Trade-off:
// a real drop is now reported after ~5.8 s instead of ~4 s.
#define DROPPED_OFF_TIMEOUT_MS \
  (DROPPED_OFF_MISSED_INTERVALS * (HEARTBEAT_INTERVAL_MS + HEARTBEAT_JITTER_MS) + \
   HEARTBEAT_INTERVAL_MS / 2)

// While a rider stays dropped off, remind with a short beep this often (the
// wind can swallow the single long ABRISS tone). From the second reminder on
// the UI also offers to remove the rider from the group -- see ui.cpp.
#define DROPPED_OFF_REMINDER_INTERVAL_MS 60000UL

#define MAX_PEERS 16

// ---------------------------------------------------------------------------
// Battery thresholds
// ---------------------------------------------------------------------------
// The INA219 is polled at most this often; everything else reads the cached
// value. Keeps the shared I2C bus (OLED!) free and the main loop fast.
#define BATTERY_READ_INTERVAL_MS 1000UL
// "Low" latches below LOW and only clears above CLEAR -- the 100 mV hysteresis
// stops the warning from flapping while the voltage bounces around under load.
// Same thresholds are used for the peers' battery level from their heartbeats.
#define BATTERY_LOW_MV 3500
#define BATTERY_LOW_CLEAR_MV 3600

// ---------------------------------------------------------------------------
// UI timing
// ---------------------------------------------------------------------------
#define UI_MENU_TIMEOUT_MS 6000UL
#define UI_INCOMING_DISPLAY_MS 15000UL
#define UI_RENAME_TIMEOUT_MS 6000UL // resets on every click/hold, so slow typing is fine

// How long the OLED stays on after being woken (button press, incoming warning,
// or a SCHWACH/ABRISS roster event), before going back to sleep to save power.
// Default only -- the actual timeout is user-adjustable at runtime (settings
// rotation -> "Anzeige") and persisted in flash, see
// DeviceConfig::displayTimeoutMs() below.
#define OLED_WAKE_MS 30000UL
// The selectable steps; 0 = the display never sleeps. One shared list so the
// settings menu (ui.cpp) and the NVS-load validation (config.cpp) can never
// disagree on what a legal value is. Labels are index-parallel.
constexpr uint32_t OLED_TIMEOUT_STEPS_MS[] = {0, 15000, 30000, 60000, 300000};
constexpr const char *OLED_TIMEOUT_LABELS[] = {"Nie", "15 s", "30 s", "1 min", "5 min"};
constexpr size_t OLED_TIMEOUT_STEP_COUNT =
    sizeof(OLED_TIMEOUT_STEPS_MS) / sizeof(OLED_TIMEOUT_STEPS_MS[0]);
static_assert(OLED_TIMEOUT_STEP_COUNT ==
                  sizeof(OLED_TIMEOUT_LABELS) / sizeof(OLED_TIMEOUT_LABELS[0]),
              "one label per timeout step");

// ---------------------------------------------------------------------------
// Per-device configuration, persisted in NVS (Preferences)
// ---------------------------------------------------------------------------
namespace DeviceConfig {

// Call once from setup(). Loads the nickname from flash (falls back to a
// 4-letter code derived from the chip's MAC if none was ever set) and
// computes this device's numeric node id from the chip's factory-programmed MAC.
void begin();

// Short display name, e.g. "ROB". Letters only, no digits -- see README.md
// "Provisionierung". Never contains real names -- riders pick their own callsign.
const char *nickname();

// Persists a new nickname (max Protocol::kNicknameFieldLen chars, truncated)
// to flash immediately.
void setNickname(const char *name);

// Stable per-device identifier derived from efuse MAC. Used on the wire instead
// of the nickname so a rename doesn't change what peers use as a dictionary key.
uint16_t nodeId();

// Monotonically increasing counter shared by every outgoing packet (heartbeat,
// warning), so (nodeId, seq) always uniquely identifies a transmission.
uint8_t nextSeq();

// Persisted piezo beep frequency in Hz, user-adjustable via the on-device
// settings menu (see ui.cpp). Falls back to BEEP_FREQUENCY_HZ if never set;
// always within [BEEP_FREQUENCY_MIN_HZ, BEEP_FREQUENCY_MAX_HZ].
uint16_t beepFrequencyHz();
void setBeepFrequencyHz(uint16_t hz);

// Persisted OLED auto-off timeout in ms, user-adjustable via the on-device
// settings menu (see ui.cpp). 0 = the display never sleeps. Always one of
// OLED_TIMEOUT_STEPS_MS; falls back to OLED_WAKE_MS if never set.
uint32_t displayTimeoutMs();
void setDisplayTimeoutMs(uint32_t ms);

// Persisted SCHWACH sensitivity step (0..FALLING_BACK_SENSITIVITY_MAX), see
// fallingBackFloorDbm() above for the mapping. Falls back to
// FALLING_BACK_SENSITIVITY_DEFAULT if never set.
uint8_t fallingBackSensitivity();
void setFallingBackSensitivity(uint8_t level);

// Persisted radio group channel (0..GROUP_CHANNEL_MAX, default 0) -- shifts
// the GFSK sync word so separate groups don't hear each other. Radio::begin()
// reads it at boot; runtime changes additionally need Radio::applyChannel().
uint8_t channel();
void setChannel(uint8_t ch);

// Runs the tiny serial console used for one-time provisioning ("name ROB").
// Call every loop(); it only acts once a full line has been typed.
void pollSerialConsole();

} // namespace DeviceConfig
