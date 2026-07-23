// Native unit tests for the wire format (src/protocol.cpp): encode/decode
// roundtrips including nickname padding/trimming, and rejection of malformed
// packets. Run with: pio test -e native
#include <string.h>

#include <unity.h>

#include "protocol.h"

namespace {
// Header (6) + battery (2) + nickname (5)
constexpr size_t kHeartbeatLen = 13;
// Header (6) + warning type (1)
constexpr size_t kWarningLen = 7;
} // namespace

void setUp() {}
void tearDown() {}

void test_heartbeat_roundtrip() {
  uint8_t buf[Protocol::kMaxPacketLen];
  size_t len = Protocol::encodeHeartbeat(buf, 0xBEEF, 42, 3712, "ROB");
  TEST_ASSERT_EQUAL_UINT(kHeartbeatLen, len);

  Protocol::DecodedPacket pkt;
  TEST_ASSERT_TRUE(Protocol::decode(buf, len, pkt));
  TEST_ASSERT_EQUAL(static_cast<int>(Protocol::MsgType::Heartbeat), static_cast<int>(pkt.type));
  TEST_ASSERT_EQUAL_HEX16(0xBEEF, pkt.senderId);
  TEST_ASSERT_EQUAL_UINT8(42, pkt.seq);
  TEST_ASSERT_EQUAL_UINT16(3712, pkt.batteryMillivolts);
  TEST_ASSERT_EQUAL_STRING("ROB", pkt.nickname); // wire padding trimmed again on decode
}

void test_heartbeat_full_length_nickname() {
  uint8_t buf[Protocol::kMaxPacketLen];
  size_t len = Protocol::encodeHeartbeat(buf, 1, 0, 4200, "KLAUS");
  Protocol::DecodedPacket pkt;
  TEST_ASSERT_TRUE(Protocol::decode(buf, len, pkt));
  TEST_ASSERT_EQUAL_STRING("KLAUS", pkt.nickname);
}

void test_heartbeat_overlong_nickname_is_truncated_on_wire() {
  uint8_t buf[Protocol::kMaxPacketLen];
  size_t len = Protocol::encodeHeartbeat(buf, 1, 0, 4200, "ROBERT");
  TEST_ASSERT_EQUAL_UINT(kHeartbeatLen, len); // packet stays fixed-size
  Protocol::DecodedPacket pkt;
  TEST_ASSERT_TRUE(Protocol::decode(buf, len, pkt));
  TEST_ASSERT_EQUAL_STRING("ROBER", pkt.nickname);
}

void test_heartbeat_empty_nickname() {
  uint8_t buf[Protocol::kMaxPacketLen];
  size_t len = Protocol::encodeHeartbeat(buf, 1, 0, 3000, "");
  Protocol::DecodedPacket pkt;
  TEST_ASSERT_TRUE(Protocol::decode(buf, len, pkt));
  TEST_ASSERT_EQUAL_STRING("", pkt.nickname); // all-spaces field trims to empty
}

void test_warning_roundtrip_all_types() {
  const Protocol::WarningType kTypes[] = {
      Protocol::WarningType::CarBehind,
      Protocol::WarningType::HazardAhead,
      Protocol::WarningType::Stopping,
      Protocol::WarningType::Regroup,
      Protocol::WarningType::Attention,
  };
  for (Protocol::WarningType type : kTypes) {
    uint8_t buf[Protocol::kMaxPacketLen];
    size_t len = Protocol::encodeWarning(buf, 0x1234, 7, type);
    TEST_ASSERT_EQUAL_UINT(kWarningLen, len);

    Protocol::DecodedPacket pkt;
    TEST_ASSERT_TRUE(Protocol::decode(buf, len, pkt));
    TEST_ASSERT_EQUAL(static_cast<int>(Protocol::MsgType::Warning), static_cast<int>(pkt.type));
    TEST_ASSERT_EQUAL_HEX16(0x1234, pkt.senderId);
    TEST_ASSERT_EQUAL_UINT8(7, pkt.seq);
    TEST_ASSERT_EQUAL(static_cast<int>(type), static_cast<int>(pkt.warningType));
  }
}

void test_reject_wrong_magic() {
  uint8_t buf[Protocol::kMaxPacketLen];
  size_t len = Protocol::encodeWarning(buf, 1, 0, Protocol::WarningType::CarBehind);
  buf[0] ^= 0xFF;
  Protocol::DecodedPacket pkt;
  TEST_ASSERT_FALSE(Protocol::decode(buf, len, pkt));
}

void test_reject_wrong_version() {
  uint8_t buf[Protocol::kMaxPacketLen];
  size_t len = Protocol::encodeWarning(buf, 1, 0, Protocol::WarningType::CarBehind);
  buf[1] = Protocol::kVersion + 1;
  Protocol::DecodedPacket pkt;
  TEST_ASSERT_FALSE(Protocol::decode(buf, len, pkt));
}

void test_reject_v1_heartbeat() {
  // Byte-for-byte v1 heartbeat: version byte 1, 6-char nickname field,
  // 14 bytes total. v2 firmware must reject it at the version byte -- a
  // mixed-version fleet cleanly sees nothing instead of half-working (see
  // the kVersion comment in protocol.h).
  const uint8_t v1[] = {0xC9, 1,   1,   0xEF, 0xBE, 42, 0x80,
                        0x0E, 'R', 'O', 'B',  ' ',  ' ', ' '};
  Protocol::DecodedPacket pkt;
  TEST_ASSERT_FALSE(Protocol::decode(v1, sizeof(v1), pkt));
}

void test_reject_short_buffer() {
  uint8_t buf[Protocol::kMaxPacketLen];
  size_t len = Protocol::encodeHeartbeat(buf, 1, 0, 3700, "ROB");
  Protocol::DecodedPacket pkt;
  // Every truncation must be rejected: below the header, and below the
  // type-specific payload length.
  for (size_t cut = 0; cut < len; cut++) {
    TEST_ASSERT_FALSE(Protocol::decode(buf, cut, pkt));
  }
  TEST_ASSERT_TRUE(Protocol::decode(buf, len, pkt));
}

void test_reject_unknown_msg_type() {
  uint8_t buf[Protocol::kMaxPacketLen];
  size_t len = Protocol::encodeWarning(buf, 1, 0, Protocol::WarningType::CarBehind);
  buf[2] = 0x7F; // neither Heartbeat nor Warning
  Protocol::DecodedPacket pkt;
  TEST_ASSERT_FALSE(Protocol::decode(buf, len, pkt));
}

void test_reject_unknown_warning_type() {
  // A well-formed Warning packet whose type byte is out of WarningType's
  // range (0 and 6+ are unassigned) must be rejected at decode, not let
  // through to degrade later at display time (warningLabel()'s "?").
  uint8_t buf[Protocol::kMaxPacketLen];
  size_t len = Protocol::encodeWarning(buf, 1, 0, Protocol::WarningType::CarBehind);
  Protocol::DecodedPacket pkt;

  buf[Protocol::kHeaderLen] = 0; // below WarningType::CarBehind (1)
  TEST_ASSERT_FALSE(Protocol::decode(buf, len, pkt));

  buf[Protocol::kHeaderLen] = static_cast<uint8_t>(Protocol::WarningType::Attention) + 1; // above the last type
  TEST_ASSERT_FALSE(Protocol::decode(buf, len, pkt));

  buf[Protocol::kHeaderLen] = 0xFF;
  TEST_ASSERT_FALSE(Protocol::decode(buf, len, pkt));
}

void test_gossip_roundtrip_empty() {
  uint8_t buf[Protocol::kMaxPacketLen];
  size_t len = Protocol::encodeGossip(buf, 0xABCD, 3, nullptr, 0);
  TEST_ASSERT_EQUAL_UINT(Protocol::kHeaderLen + 1, len); // header + count byte only

  Protocol::DecodedPacket pkt;
  TEST_ASSERT_TRUE(Protocol::decode(buf, len, pkt));
  TEST_ASSERT_EQUAL(static_cast<int>(Protocol::MsgType::Gossip), static_cast<int>(pkt.type));
  TEST_ASSERT_EQUAL_HEX16(0xABCD, pkt.senderId);
  TEST_ASSERT_EQUAL_UINT8(3, pkt.seq);
  TEST_ASSERT_EQUAL_UINT8(0, pkt.gossipCount);
}

void test_gossip_roundtrip_typical() {
  const Protocol::GossipEntry entries[] = {
      {0x1111, 0x2222, Protocol::GossipStatus::Weak, 3, 7},
      {0x3333, 0x4444, Protocol::GossipStatus::Lost, 0, 255}, // ttl 0 = do not relay further
  };
  uint8_t buf[Protocol::kMaxPacketLen];
  size_t len = Protocol::encodeGossip(buf, 0x9999, 1, entries, 2);
  TEST_ASSERT_EQUAL_UINT(Protocol::kHeaderLen + 1 + 2 * Protocol::kGossipEntryWireLen, len);

  Protocol::DecodedPacket pkt;
  TEST_ASSERT_TRUE(Protocol::decode(buf, len, pkt));
  TEST_ASSERT_EQUAL(static_cast<int>(Protocol::MsgType::Gossip), static_cast<int>(pkt.type));
  TEST_ASSERT_EQUAL_UINT8(2, pkt.gossipCount);

  TEST_ASSERT_EQUAL_HEX16(0x1111, pkt.gossipEntries[0].observerId);
  TEST_ASSERT_EQUAL_HEX16(0x2222, pkt.gossipEntries[0].subjectId);
  TEST_ASSERT_EQUAL(static_cast<int>(Protocol::GossipStatus::Weak),
                     static_cast<int>(pkt.gossipEntries[0].status));
  TEST_ASSERT_EQUAL_UINT8(3, pkt.gossipEntries[0].ttl);
  TEST_ASSERT_EQUAL_UINT8(7, pkt.gossipEntries[0].seq);

  TEST_ASSERT_EQUAL_HEX16(0x3333, pkt.gossipEntries[1].observerId);
  TEST_ASSERT_EQUAL_HEX16(0x4444, pkt.gossipEntries[1].subjectId);
  TEST_ASSERT_EQUAL(static_cast<int>(Protocol::GossipStatus::Lost),
                     static_cast<int>(pkt.gossipEntries[1].status));
  TEST_ASSERT_EQUAL_UINT8(0, pkt.gossipEntries[1].ttl);
  TEST_ASSERT_EQUAL_UINT8(255, pkt.gossipEntries[1].seq);
}

// A pure encode-then-decode roundtrip cannot catch a bug where both sides
// are consistently wrong in the same way (e.g. swapped byte order, a moved
// bit position) -- pin the exact on-wire bytes for one known entry, the same
// way test_reject_v1_heartbeat pins the heartbeat layout.
void test_gossip_golden_bytes_pins_wire_layout() {
  const Protocol::GossipEntry entries[] = {
      {0xABCD, 0x0102, Protocol::GossipStatus::Lost, /*ttl=*/3, /*seq=*/42},
  };
  uint8_t buf[Protocol::kMaxPacketLen];
  size_t len = Protocol::encodeGossip(buf, /*senderId=*/0x1234, /*seq=*/7, entries, 1);

  const uint8_t expected[] = {
      0xC9,       // magic
      3,          // version (kVersion)
      3,          // MsgType::Gossip
      0x34, 0x12, // senderId 0x1234, little-endian
      7,          // header seq
      1,          // entry count
      0xCD, 0xAB, // observerId 0xABCD, little-endian
      0x02, 0x01, // subjectId 0x0102, little-endian
      0x83,       // status(Lost=1)<<7 | ttl(3) == 0x80 | 0x03
      42,         // entry seq
  };
  TEST_ASSERT_EQUAL_UINT(sizeof(expected), len);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, buf, sizeof(expected));

  // Decoding the golden bytes must reproduce the same entry.
  Protocol::DecodedPacket pkt;
  TEST_ASSERT_TRUE(Protocol::decode(expected, sizeof(expected), pkt));
  TEST_ASSERT_EQUAL_UINT8(1, pkt.gossipCount);
  TEST_ASSERT_EQUAL_HEX16(0xABCD, pkt.gossipEntries[0].observerId);
  TEST_ASSERT_EQUAL_HEX16(0x0102, pkt.gossipEntries[0].subjectId);
  TEST_ASSERT_EQUAL(static_cast<int>(Protocol::GossipStatus::Lost),
                     static_cast<int>(pkt.gossipEntries[0].status));
  TEST_ASSERT_EQUAL_UINT8(3, pkt.gossipEntries[0].ttl);
  TEST_ASSERT_EQUAL_UINT8(42, pkt.gossipEntries[0].seq);
}

// The status/ttl byte packs a 1-bit status into the top bit and a 4-bit ttl
// into the low nibble -- exercise the ttl range that actually fits (0..15)
// to pin down that packing, independent of the roundtrip test above.
void test_gossip_ttl_nibble_range_roundtrips() {
  for (uint8_t ttl = 0; ttl <= 15; ttl++) {
    Protocol::GossipEntry entry{0x1234, 0x5678, Protocol::GossipStatus::Weak, ttl, 0};
    uint8_t buf[Protocol::kMaxPacketLen];
    size_t len = Protocol::encodeGossip(buf, 1, 0, &entry, 1);
    Protocol::DecodedPacket pkt;
    TEST_ASSERT_TRUE(Protocol::decode(buf, len, pkt));
    TEST_ASSERT_EQUAL_UINT8(ttl, pkt.gossipEntries[0].ttl);
  }
}

void test_gossip_roundtrip_max_entries() {
  Protocol::GossipEntry entries[Protocol::kMaxGossipEntries];
  for (size_t i = 0; i < Protocol::kMaxGossipEntries; i++) {
    entries[i] = {static_cast<uint16_t>(0x1000 + i), static_cast<uint16_t>(0x2000 + i),
                  (i % 2 == 0) ? Protocol::GossipStatus::Weak : Protocol::GossipStatus::Lost,
                  static_cast<uint8_t>(i), static_cast<uint8_t>(i * 10)};
  }
  uint8_t buf[Protocol::kMaxPacketLen];
  size_t len = Protocol::encodeGossip(buf, 1, 0, entries, static_cast<uint8_t>(Protocol::kMaxGossipEntries));
  TEST_ASSERT_TRUE(len <= Protocol::kMaxPacketLen); // must actually fit in one packet

  Protocol::DecodedPacket pkt;
  TEST_ASSERT_TRUE(Protocol::decode(buf, len, pkt));
  TEST_ASSERT_EQUAL_UINT8(Protocol::kMaxGossipEntries, pkt.gossipCount);
  for (size_t i = 0; i < Protocol::kMaxGossipEntries; i++) {
    TEST_ASSERT_EQUAL_HEX16(entries[i].observerId, pkt.gossipEntries[i].observerId);
    TEST_ASSERT_EQUAL_HEX16(entries[i].subjectId, pkt.gossipEntries[i].subjectId);
    TEST_ASSERT_EQUAL(static_cast<int>(entries[i].status), static_cast<int>(pkt.gossipEntries[i].status));
    TEST_ASSERT_EQUAL_UINT8(entries[i].seq, pkt.gossipEntries[i].seq);
  }
}

void test_reject_gossip_count_exceeds_max() {
  uint8_t buf[Protocol::kMaxPacketLen];
  size_t len = Protocol::encodeGossip(buf, 1, 0, nullptr, 0);
  buf[Protocol::kHeaderLen] = static_cast<uint8_t>(Protocol::kMaxGossipEntries + 1); // claim more than could ever fit
  Protocol::DecodedPacket pkt;
  TEST_ASSERT_FALSE(Protocol::decode(buf, len, pkt));
}

void test_reject_gossip_short_buffer() {
  const Protocol::GossipEntry entries[] = {
      {0x1111, 0x2222, Protocol::GossipStatus::Weak, 3, 7},
  };
  uint8_t buf[Protocol::kMaxPacketLen];
  size_t len = Protocol::encodeGossip(buf, 1, 0, entries, 1);
  Protocol::DecodedPacket pkt;
  for (size_t cut = 0; cut < len; cut++) {
    TEST_ASSERT_FALSE(Protocol::decode(buf, cut, pkt));
  }
  TEST_ASSERT_TRUE(Protocol::decode(buf, len, pkt));
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_heartbeat_roundtrip);
  RUN_TEST(test_heartbeat_full_length_nickname);
  RUN_TEST(test_heartbeat_overlong_nickname_is_truncated_on_wire);
  RUN_TEST(test_heartbeat_empty_nickname);
  RUN_TEST(test_warning_roundtrip_all_types);
  RUN_TEST(test_reject_wrong_magic);
  RUN_TEST(test_reject_wrong_version);
  RUN_TEST(test_reject_v1_heartbeat);
  RUN_TEST(test_reject_short_buffer);
  RUN_TEST(test_reject_unknown_msg_type);
  RUN_TEST(test_reject_unknown_warning_type);
  RUN_TEST(test_gossip_roundtrip_empty);
  RUN_TEST(test_gossip_roundtrip_typical);
  RUN_TEST(test_gossip_golden_bytes_pins_wire_layout);
  RUN_TEST(test_gossip_ttl_nibble_range_roundtrips);
  RUN_TEST(test_gossip_roundtrip_max_entries);
  RUN_TEST(test_reject_gossip_count_exceeds_max);
  RUN_TEST(test_reject_gossip_short_buffer);
  return UNITY_END();
}
