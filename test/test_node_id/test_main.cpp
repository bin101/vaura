// Native unit tests for the node-id-from-MAC derivation (src/node_id.h).
// Run with: pio test -e native
#include <unity.h>

#include "node_id.h"

void setUp() {}
void tearDown() {}

// Three XIAO ESP32-S3 boards from the same manufacturing batch: identical
// Espressif OUI (low 3 octets), distinct NIC portion (upper 3 octets).
// ESP.getEfuseMac() packs MAC octet 0 into the LSB, so octet 5 lands in the
// MSB of the returned uint64_t.
uint64_t macFromOctets(uint8_t o0, uint8_t o1, uint8_t o2, uint8_t o3, uint8_t o4, uint8_t o5) {
  return static_cast<uint64_t>(o0) | (static_cast<uint64_t>(o1) << 8) | (static_cast<uint64_t>(o2) << 16) |
         (static_cast<uint64_t>(o3) << 24) | (static_cast<uint64_t>(o4) << 32) | (static_cast<uint64_t>(o5) << 40);
}

void test_same_oui_different_nic_yields_distinct_ids() {
  // Same Espressif OUI (7C:DF:A1), different NIC bytes per device.
  uint64_t macA = macFromOctets(0x7C, 0xDF, 0xA1, 0x01, 0x02, 0x03);
  uint64_t macB = macFromOctets(0x7C, 0xDF, 0xA1, 0x11, 0x22, 0x33);
  uint64_t macC = macFromOctets(0x7C, 0xDF, 0xA1, 0xAA, 0xBB, 0xCC);

  uint16_t idA = nodeIdFromMac(macA);
  uint16_t idB = nodeIdFromMac(macB);
  uint16_t idC = nodeIdFromMac(macC);

  TEST_ASSERT_NOT_EQUAL(idA, idB);
  TEST_ASSERT_NOT_EQUAL(idB, idC);
  TEST_ASSERT_NOT_EQUAL(idA, idC);
}

void test_regression_old_low16_formula_would_have_collided() {
  // Sanity check on the bug this replaces: `mac & 0xFFFF` reads octets 0-1,
  // which are OUI bytes shared by every device below -- confirming the old
  // derivation collides even though the devices are clearly distinct.
  uint64_t macA = macFromOctets(0x7C, 0xDF, 0xA1, 0x01, 0x02, 0x03);
  uint64_t macB = macFromOctets(0x7C, 0xDF, 0xA1, 0x11, 0x22, 0x33);

  uint16_t oldIdA = static_cast<uint16_t>(macA & 0xFFFF);
  uint16_t oldIdB = static_cast<uint16_t>(macB & 0xFFFF);
  TEST_ASSERT_EQUAL_UINT16(oldIdA, oldIdB); // the bug: both collapse to the OUI

  // The new derivation tells them apart.
  TEST_ASSERT_NOT_EQUAL(nodeIdFromMac(macA), nodeIdFromMac(macB));
}

void test_differing_only_in_nic_high_byte_still_differs() {
  // Two MACs differing only in the NIC's most-significant octet (5) --
  // guards against a fold that accidentally drops the high byte.
  uint64_t macA = macFromOctets(0x7C, 0xDF, 0xA1, 0x01, 0x02, 0x03);
  uint64_t macB = macFromOctets(0x7C, 0xDF, 0xA1, 0x01, 0x02, 0xFF);
  TEST_ASSERT_NOT_EQUAL(nodeIdFromMac(macA), nodeIdFromMac(macB));
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_same_oui_different_nic_yields_distinct_ids);
  RUN_TEST(test_regression_old_low16_formula_would_have_collided);
  RUN_TEST(test_differing_only_in_nic_high_byte_still_differs);
  return UNITY_END();
}
