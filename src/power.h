// Battery voltage readout via the INA219 (wired high-side between the LiPo and
// the XIAO's BAT+ pin, per the case author's wiring notes -- see README.md).
#pragma once

#include <Arduino.h>

namespace Power {

// Returns false (without blocking) if no INA219 answers on the I2C bus --
// the rest of the device still works, the battery row is just hidden.
bool begin();

bool available();

// Cached: the INA219 is only actually read every BATTERY_READ_INTERVAL_MS --
// safe to call from the tight main loop.
uint16_t batteryMillivolts();

// Rough 1S LiPo voltage-to-charge curve; good enough for a UI estimate, not a
// lab-grade fuel gauge.
uint8_t batteryPercent();

// Below the safe-cutoff-ish threshold -- worth nagging the rider to charge.
// Latched with hysteresis (BATTERY_LOW_MV / BATTERY_LOW_CLEAR_MV) so it
// doesn't flap while the voltage bounces under load.
bool isLow();

} // namespace Power
