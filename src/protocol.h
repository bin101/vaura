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
// v3: added the Gossip message type (cooperative drop-off confirmation, see
// coop.{h,cpp}). Bumped deliberately, same reasoning as the v1->v2 bump
// below: v2 and v3 devices reject each other symmetrically at the version
// byte rather than a v2 device silently never seeing Gossip traffic while a
// v3 device sees everything -- a clean "update the whole fleet together".
// v2: nickname field shrank from 6 to 5 bytes.
constexpr uint8_t kVersion = 3;
// Hard cap on nickname length -- not an arbitrary round number. The idle
// screen lists riders in up to three columns on the 128px display, and a name
// is shown "decorated" with its status there: "!NAME!" = signal fading,
// "(NAME)" = dropped off. 5 name chars + 2 decoration chars = 7 chars per
// column entry; three 7-char columns are exactly the 21 chars one line fits
// at the 6px/char font (u8g2_font_6x10_tf). Raising this breaks that layout
// (see ui.cpp renderIdle()).
constexpr size_t kNicknameFieldLen = 5; // not null-terminated on the wire

// Header common to every packet: magic(1) + version(1) + msgType(1) +
// senderId(2) + seq(1). Exposed here (not just internal to protocol.cpp) so
// callers can size buffers/derive per-type capacity constants (see
// kMaxGossipEntries below).
constexpr size_t kHeaderLen = 6;

constexpr size_t kMaxPacketLen = 40; // generous headroom over the largest packet (a full Gossip batch)

enum class MsgType : uint8_t {
  Heartbeat = 1,
  Warning = 2,
  Gossip = 3,
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

// Cooperative drop-off confirmation ("anomaly gossip", see coop.{h,cpp}):
// each device relays only its own and others' WEAK/LOST *opinions* about a
// peer, never "OK" -- an all-fine group therefore generates no gossip
// traffic at all. A device's local RSSI/heartbeat verdict is what forms an
// opinion; it no longer alerts the rider on its own (see Roster::tick()).
enum class GossipStatus : uint8_t {
  Weak = 0, // observer's local "falling back" opinion of the subject
  Lost = 1, // observer's local "dropped off" opinion of the subject
};

// One relayed opinion: `observerId` is whose opinion this is (not necessarily
// the packet's senderId -- a relay forwards someone else's entry unchanged
// apart from `ttl`). `seq` is per (observerId, subjectId), bumped on every
// status change AND on every periodic refresh while the opinion stays
// active, so a stale/duplicate copy arriving via a different relay path can
// be told apart from a genuinely newer report (see coop.cpp).
struct GossipEntry {
  uint16_t observerId;
  uint16_t subjectId;
  GossipStatus status;
  uint8_t ttl; // remaining relay hops; forwarders decrement, 0 = do not relay further
  uint8_t seq;
};

constexpr size_t kGossipEntryWireLen = 6; // observerId(2) + subjectId(2) + status/ttl(1) + seq(1)
constexpr size_t kGossipCountLen = 1; // entry-count byte
// How many entries fit in one packet given kMaxPacketLen -- also the buffer
// size DecodedPacket::gossipEntries needs.
constexpr size_t kMaxGossipEntries = (kMaxPacketLen - kHeaderLen - kGossipCountLen) / kGossipEntryWireLen;

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
  // Gossip
  GossipEntry gossipEntries[kMaxGossipEntries];
  uint8_t gossipCount;
};

// Encoders return the number of bytes written into `out` (which must be at
// least kMaxPacketLen bytes), or 0 on failure.
size_t encodeHeartbeat(uint8_t *out, uint16_t senderId, uint8_t seq, uint16_t batteryMillivolts,
                        const char *nickname);
size_t encodeWarning(uint8_t *out, uint16_t senderId, uint8_t seq, WarningType type);
// `count` is clamped to kMaxGossipEntries if larger (callers should already
// respect the cap -- see Coop::collectOutgoing()).
size_t encodeGossip(uint8_t *out, uint16_t senderId, uint8_t seq, const GossipEntry *entries, uint8_t count);

// Returns true and fills `result` if `data` is a well-formed packet of a known type.
bool decode(const uint8_t *data, size_t len, DecodedPacket &result);

} // namespace Protocol
