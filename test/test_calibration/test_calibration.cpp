// Native unit tests for the self-calibrating falling-back floor's pure math
// (src/calibration.cpp). Run with: pio test -e native
#include <unity.h>

#include "calibration.h"

void setUp() {}
void tearDown() {}

// --- medianDbm ---------------------------------------------------------

void test_median_single_value() {
  float v[] = {-90.0f};
  TEST_ASSERT_EQUAL_FLOAT(-90.0f, Calibration::medianDbm(v, 1));
}

void test_median_odd_count_unsorted_input() {
  float v[] = {-70.0f, -100.0f, -85.0f};
  // Sorted: -100, -85, -70 -- middle is -85.
  TEST_ASSERT_EQUAL_FLOAT(-85.0f, Calibration::medianDbm(v, 3));
}

void test_median_even_count_averages_middle_two() {
  float v[] = {-90.0f, -80.0f, -100.0f, -70.0f};
  // Sorted: -100, -90, -80, -70 -- middle two are -90 and -80 -> -85.
  TEST_ASSERT_EQUAL_FLOAT(-85.0f, Calibration::medianDbm(v, 4));
}

void test_median_single_outlier_does_not_dominate() {
  // Three riders riding together around -85 dBm, one already falling back
  // hard at -115 dBm -- the median should stay close to the healthy group,
  // not get dragged toward the outlier the way a mean would.
  float v[] = {-84.0f, -86.0f, -85.0f, -115.0f};
  float median = Calibration::medianDbm(v, 4);
  TEST_ASSERT_TRUE(median > -90.0f);
}

void test_median_zero_or_negative_count_returns_zero() {
  float v[] = {-90.0f};
  TEST_ASSERT_EQUAL_FLOAT(0.0f, Calibration::medianDbm(v, 0));
  TEST_ASSERT_EQUAL_FLOAT(0.0f, Calibration::medianDbm(v, -1));
}

// --- smoothBaseline ------------------------------------------------------

void test_smooth_baseline_alpha_zero_keeps_previous() {
  TEST_ASSERT_EQUAL_FLOAT(-90.0f, Calibration::smoothBaseline(-90.0f, -70.0f, 0.0f));
}

void test_smooth_baseline_alpha_one_jumps_to_sample() {
  TEST_ASSERT_EQUAL_FLOAT(-70.0f, Calibration::smoothBaseline(-90.0f, -70.0f, 1.0f));
}

void test_smooth_baseline_blends_partially() {
  // alpha 0.1 blend of prev=-100, sample=-90 -> -100 + 0.1*10 = -99.
  TEST_ASSERT_FLOAT_WITHIN(0.01f, -99.0f, Calibration::smoothBaseline(-100.0f, -90.0f, 0.1f));
}

// --- deriveFloorDbm ------------------------------------------------------

void test_derive_floor_uses_fallback_when_not_established() {
  int16_t floor = Calibration::deriveFloorDbm(/*baselineDbm=*/-70.0f, /*marginDb=*/15, /*baselineEstablished=*/false,
                                               /*fallbackFloorDbm=*/-105, /*floorMinDbm=*/-112, /*floorMaxDbm=*/-80);
  TEST_ASSERT_EQUAL_INT16(-105, floor);
}

void test_derive_floor_baseline_minus_margin_when_established() {
  int16_t floor = Calibration::deriveFloorDbm(/*baselineDbm=*/-85.0f, /*marginDb=*/15, /*baselineEstablished=*/true,
                                               /*fallbackFloorDbm=*/-105, /*floorMinDbm=*/-112, /*floorMaxDbm=*/-80);
  TEST_ASSERT_EQUAL_INT16(-100, floor);
}

void test_derive_floor_clamps_to_min() {
  // A very weak group baseline (-110) minus a small margin would compute
  // below the receiver's real sensitivity -- clamp to floorMinDbm instead of
  // producing a floor that can never actually be crossed.
  int16_t floor = Calibration::deriveFloorDbm(/*baselineDbm=*/-110.0f, /*marginDb=*/5, /*baselineEstablished=*/true,
                                               /*fallbackFloorDbm=*/-105, /*floorMinDbm=*/-112, /*floorMaxDbm=*/-80);
  TEST_ASSERT_EQUAL_INT16(-112, floor);
}

void test_derive_floor_clamps_to_max() {
  // A very strong, tight group baseline (-60) with a small margin would push
  // the floor implausibly high -- clamp so an ordinary signal dip a bit below
  // the pack doesn't become a permanent false alarm.
  int16_t floor = Calibration::deriveFloorDbm(/*baselineDbm=*/-60.0f, /*marginDb=*/0, /*baselineEstablished=*/true,
                                               /*fallbackFloorDbm=*/-105, /*floorMinDbm=*/-112, /*floorMaxDbm=*/-80);
  TEST_ASSERT_EQUAL_INT16(-80, floor);
}

// --- marginDbForLevel ------------------------------------------------------

void test_margin_default_level_matches_historical_floor_margin() {
  // Level 5 of 0..10 at 3 dB/step must equal 15 dB -- the margin that
  // reproduces the historical fixed -105 dBm floor's effective sensitivity
  // (see config.h's fallingBackMarginDb() static_assert).
  TEST_ASSERT_EQUAL_INT(15, Calibration::marginDbForLevel(5, 10, 3));
}

void test_margin_max_level_is_most_sensitive_zero_margin() {
  TEST_ASSERT_EQUAL_INT(0, Calibration::marginDbForLevel(10, 10, 3));
}

void test_margin_zero_level_is_least_sensitive_full_margin() {
  TEST_ASSERT_EQUAL_INT(30, Calibration::marginDbForLevel(0, 10, 3));
}

void test_margin_level_above_max_clamps_to_max_level() {
  // Shouldn't happen (NVS load already validates), but must not produce a
  // negative margin if it ever did.
  TEST_ASSERT_EQUAL_INT(0, Calibration::marginDbForLevel(255, 10, 3));
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_median_single_value);
  RUN_TEST(test_median_odd_count_unsorted_input);
  RUN_TEST(test_median_even_count_averages_middle_two);
  RUN_TEST(test_median_single_outlier_does_not_dominate);
  RUN_TEST(test_median_zero_or_negative_count_returns_zero);
  RUN_TEST(test_smooth_baseline_alpha_zero_keeps_previous);
  RUN_TEST(test_smooth_baseline_alpha_one_jumps_to_sample);
  RUN_TEST(test_smooth_baseline_blends_partially);
  RUN_TEST(test_derive_floor_uses_fallback_when_not_established);
  RUN_TEST(test_derive_floor_baseline_minus_margin_when_established);
  RUN_TEST(test_derive_floor_clamps_to_min);
  RUN_TEST(test_derive_floor_clamps_to_max);
  RUN_TEST(test_margin_default_level_matches_historical_floor_margin);
  RUN_TEST(test_margin_max_level_is_most_sensitive_zero_margin);
  RUN_TEST(test_margin_zero_level_is_least_sensitive_full_margin);
  RUN_TEST(test_margin_level_above_max_clamps_to_max_level);
  return UNITY_END();
}
