// Native unit tests for the charging hysteresis/dwell-time state machine
// (src/charging_decision.cpp). Run with: pio test -e native
#include <unity.h>

#include "charging_decision.h"

namespace {
constexpr int16_t kEnterMa = 40;
constexpr int16_t kExitMa = 15;
constexpr uint32_t kDwellMs = 5000;
} // namespace

void setUp() {}
void tearDown() {}

void test_stays_off_below_enter_threshold() {
  ChargingDecisionState s;
  TEST_ASSERT_FALSE(updateChargingDecision(s, 0, 0, kEnterMa, kExitMa, kDwellMs));
  TEST_ASSERT_FALSE(updateChargingDecision(s, 20, 1000, kEnterMa, kExitMa, kDwellMs));
  // Above the exit threshold but still below enter: not enough to start.
  TEST_ASSERT_FALSE(updateChargingDecision(s, kExitMa, 2000, kEnterMa, kExitMa, kDwellMs));
  TEST_ASSERT_FALSE(updateChargingDecision(s, kEnterMa - 1, 3000, kEnterMa, kExitMa, kDwellMs));
}

void test_enters_at_threshold() {
  ChargingDecisionState s;
  TEST_ASSERT_TRUE(updateChargingDecision(s, kEnterMa, 0, kEnterMa, kExitMa, kDwellMs));
}

void test_brief_dip_does_not_exit() {
  ChargingDecisionState s;
  TEST_ASSERT_TRUE(updateChargingDecision(s, 100, 0, kEnterMa, kExitMa, kDwellMs));
  // Dips below the exit threshold, but the dwell window hasn't elapsed yet.
  TEST_ASSERT_TRUE(updateChargingDecision(s, 5, kDwellMs - 1, kEnterMa, kExitMa, kDwellMs));
}

void test_exits_after_full_dwell_below_exit() {
  ChargingDecisionState s;
  TEST_ASSERT_TRUE(updateChargingDecision(s, 100, 0, kEnterMa, kExitMa, kDwellMs));
  TEST_ASSERT_FALSE(updateChargingDecision(s, 5, kDwellMs, kEnterMa, kExitMa, kDwellMs));
}

void test_dwell_clock_resets_when_current_recovers() {
  ChargingDecisionState s;
  TEST_ASSERT_TRUE(updateChargingDecision(s, 100, 0, kEnterMa, kExitMa, kDwellMs));
  TEST_ASSERT_TRUE(updateChargingDecision(s, 5, 3000, kEnterMa, kExitMa, kDwellMs));
  // Recovers back above the exit threshold before the dwell elapses -- the
  // clock resets from this new timestamp.
  TEST_ASSERT_TRUE(updateChargingDecision(s, 20, 4000, kEnterMa, kExitMa, kDwellMs));
  TEST_ASSERT_TRUE(updateChargingDecision(s, 5, 4000 + kDwellMs - 1, kEnterMa, kExitMa, kDwellMs));
  TEST_ASSERT_FALSE(updateChargingDecision(s, 5, 4000 + kDwellMs, kEnterMa, kExitMa, kDwellMs));
}

void test_reenters_after_exiting() {
  ChargingDecisionState s;
  TEST_ASSERT_TRUE(updateChargingDecision(s, 100, 0, kEnterMa, kExitMa, kDwellMs));
  TEST_ASSERT_FALSE(updateChargingDecision(s, 0, kDwellMs, kEnterMa, kExitMa, kDwellMs));
  TEST_ASSERT_TRUE(updateChargingDecision(s, kEnterMa, kDwellMs + 100, kEnterMa, kExitMa, kDwellMs));
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_stays_off_below_enter_threshold);
  RUN_TEST(test_enters_at_threshold);
  RUN_TEST(test_brief_dip_does_not_exit);
  RUN_TEST(test_exits_after_full_dwell_below_exit);
  RUN_TEST(test_dwell_clock_resets_when_current_recovers);
  RUN_TEST(test_reenters_after_exiting);
  return UNITY_END();
}
