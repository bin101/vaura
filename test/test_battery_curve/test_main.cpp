// Native unit tests for the 1S-LiPo voltage-to-percent curve
// (src/battery_curve.cpp). Run with: pio test -e native
#include <unity.h>

#include "battery_curve.h"

void setUp() {}
void tearDown() {}

void test_exact_curve_points() {
  TEST_ASSERT_EQUAL_UINT8(100, batteryPercentFromMillivolts(4200));
  TEST_ASSERT_EQUAL_UINT8(85, batteryPercentFromMillivolts(4000));
  TEST_ASSERT_EQUAL_UINT8(70, batteryPercentFromMillivolts(3850));
  TEST_ASSERT_EQUAL_UINT8(50, batteryPercentFromMillivolts(3700));
  TEST_ASSERT_EQUAL_UINT8(30, batteryPercentFromMillivolts(3600));
  TEST_ASSERT_EQUAL_UINT8(15, batteryPercentFromMillivolts(3500));
  TEST_ASSERT_EQUAL_UINT8(5, batteryPercentFromMillivolts(3400));
  TEST_ASSERT_EQUAL_UINT8(0, batteryPercentFromMillivolts(3300));
}

void test_interpolation_between_points() {
  // Midway between 3600 mV (30%) and 3700 mV (50%) -> 40%
  TEST_ASSERT_EQUAL_UINT8(40, batteryPercentFromMillivolts(3650));
  // Midway between 3700 mV (50%) and 3850 mV (70%) -> 60%
  TEST_ASSERT_EQUAL_UINT8(60, batteryPercentFromMillivolts(3775));
}

void test_clamping() {
  TEST_ASSERT_EQUAL_UINT8(100, batteryPercentFromMillivolts(4500)); // above top
  TEST_ASSERT_EQUAL_UINT8(0, batteryPercentFromMillivolts(3000));   // below bottom
  TEST_ASSERT_EQUAL_UINT8(0, batteryPercentFromMillivolts(0));
}

void test_monotonic_over_full_range() {
  uint8_t previous = 0;
  for (uint16_t mv = 3300; mv <= 4200; mv++) {
    uint8_t pct = batteryPercentFromMillivolts(mv);
    TEST_ASSERT_TRUE(pct >= previous);
    previous = pct;
  }
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_exact_curve_points);
  RUN_TEST(test_interpolation_between_points);
  RUN_TEST(test_clamping);
  RUN_TEST(test_monotonic_over_full_range);
  return UNITY_END();
}
