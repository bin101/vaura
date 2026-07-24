#include "calibration.h"

namespace Calibration {

float medianDbm(float *values, int n) {
  if (n <= 0) {
    return 0.0f;
  }
  // Insertion sort: n is at most MAX_PEERS+1 (a club-ride-sized group), same
  // "small n, keep it simple" tradeoff as the rider-list sort in ui.cpp.
  for (int i = 1; i < n; i++) {
    float key = values[i];
    int j = i - 1;
    while (j >= 0 && values[j] > key) {
      values[j + 1] = values[j];
      j--;
    }
    values[j + 1] = key;
  }
  if (n % 2 == 1) {
    return values[n / 2];
  }
  return (values[n / 2 - 1] + values[n / 2]) / 2.0f;
}

float smoothBaseline(float prevBaselineDbm, float sampleDbm, float alpha) {
  return alpha * sampleDbm + (1.0f - alpha) * prevBaselineDbm;
}

int16_t deriveFloorDbm(float baselineDbm, int marginDb, bool baselineEstablished, int16_t fallbackFloorDbm,
                        int16_t floorMinDbm, int16_t floorMaxDbm) {
  if (!baselineEstablished) {
    return fallbackFloorDbm;
  }
  float floor = baselineDbm - static_cast<float>(marginDb);
  if (floor < static_cast<float>(floorMinDbm)) {
    return floorMinDbm;
  }
  if (floor > static_cast<float>(floorMaxDbm)) {
    return floorMaxDbm;
  }
  return static_cast<int16_t>(floor);
}

} // namespace Calibration
