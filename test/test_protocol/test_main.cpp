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
  return UNITY_END();
}
