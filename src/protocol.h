// Wire format for the club warning network. Deliberately tiny and hand-packed
// (no structs sent directly over the air) so there is no compiler-padding
// ambiguity between the two ends -- both are the same MCU today, but the
// format should stay portable regardless.
#pragma once

// Plain C headers only (no Arduino.h): this module is pure byte-packing and
// runs unchanged in the native unit-test environment (pio test -e native).
#include <stddef.h>
#include <stdint.h>

namespace Protocol {

constexpr uint8_t kMagic = 0xC9;
// v2: nickname field shrank from 6 to 5 bytes. Bumped deliberately so v1 and
// v2 devices reject each other symmetrically at the version byte -- without
// the bump a v1 device would silently never see v2 heartbeats (13 < its
// expected 14) while v2 still saw v1, a one-way invisibility that is far more
// confusing than a clean "update the whole fleet together".
constexpr uint8_t kVersion = 2;
// Hard cap on nickname length -- not an arbitrary round number. The idle
// screen lists riders in up to three columns on the 128px display, and a name
// is shown "decorated" with its status there: "!NAME!" = signal fading,
// "(NAME)" = dropped off. 5 name chars + 2 decoration chars = 7 chars per
// column entry; three 7-char columns are exactly the 21 chars one line fits
// at the 6px/char font (u8g2_font_6x10_tf). Raising this breaks that layout
// (see ui.cpp renderIdle()).
constexpr size_t kNicknameFieldLen = 5; // not null-terminated on the wire
constexpr size_t kMaxPacketLen = 16; // generous headroom over the largest packet

enum class MsgType : uint8_t {
  Heartbeat = 1,
  Warning = 2,
};

// Ordered roughly by expected frequency of use -- also the order they cycle
// through in the single-button send menu (see ui.cpp).
enum class WarningType : uint8_t {
  CarBehind = 1, // "Car behind"
  HazardAhead = 2, // "Hazard ahead"
  Stopping = 3, // "Braking / stopping"
  Regroup = 4, // "Regroup / wait"
  // Not in the send menu: fired by the double-click emergency gesture (works
  // blind, even while the display sleeps). Deliberately generic -- a blind
  // gesture must never claim something specific that might be wrong.
  Attention = 5, // "Attention!"
};

// Human-readable short labels for the OLED (max ~16 chars at the chosen font).
const char *warningLabel(WarningType type);

// Decoded representation of any received packet. Only the fields relevant to
// `type` are meaningful -- callers switch on `type` first.
struct DecodedPacket {
  MsgType type;
  uint16_t senderId;
  uint8_t seq;
  // Heartbeat
  uint16_t batteryMillivolts;
  char nickname[kNicknameFieldLen + 1];
  // Warning
  WarningType warningType;
};

// Encoders return the number of bytes written into `out` (which must be at
// least kMaxPacketLen bytes), or 0 on failure.
size_t encodeHeartbeat(uint8_t *out, uint16_t senderId, uint8_t seq, uint16_t batteryMillivolts,
                        const char *nickname);
size_t encodeWarning(uint8_t *out, uint16_t senderId, uint8_t seq, WarningType type);

// Returns true and fills `result` if `data` is a well-formed packet of a known type.
bool decode(const uint8_t *data, size_t len, DecodedPacket &result);

} // namespace Protocol
