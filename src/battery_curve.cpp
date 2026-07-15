#include "battery_curve.h"

#include <math.h>
#include <stddef.h>

namespace {
struct CurvePoint {
  uint16_t mv;
  uint8_t percent;
};
const CurvePoint kCurve[] = {
    {4200, 100}, {4000, 85}, {3850, 70}, {3700, 50},
    {3600, 30},  {3500, 15}, {3400, 5},  {3300, 0},
};
const size_t kCurveLen = sizeof(kCurve) / sizeof(kCurve[0]);
} // namespace

uint8_t batteryPercentFromMillivolts(uint16_t mv) {
  if (mv >= kCurve[0].mv) {
    return kCurve[0].percent;
  }
  if (mv <= kCurve[kCurveLen - 1].mv) {
    return kCurve[kCurveLen - 1].percent;
  }
  for (size_t i = 0; i + 1 < kCurveLen; i++) {
    if (mv <= kCurve[i].mv && mv >= kCurve[i + 1].mv) {
      float span = static_cast<float>(kCurve[i].mv - kCurve[i + 1].mv);
      float pos = static_cast<float>(mv - kCurve[i + 1].mv) / span;
      float pct = kCurve[i + 1].percent + pos * (kCurve[i].percent - kCurve[i + 1].percent);
      return static_cast<uint8_t>(lroundf(pct));
    }
  }
  return 0;
}
