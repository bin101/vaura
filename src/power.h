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

// Charge current in mA, sign-normalized so positive means "flowing into the
// battery" (see config.h's INA219_CURRENT_CHARGE_SIGN -- the raw sign
// depends on which way the INA219's IN+/IN- ended up soldered, and must be
// confirmed on real hardware). Cached alongside batteryMillivolts() in the
// same BATTERY_READ_INTERVAL_MS-throttled I2C read. 0 if no INA219 was found.
int16_t chargeCurrentMilliamps();

// True while the battery is actively being charged, decided from the charge
// current with hysteresis and a minimum dwell time (see charging_decision.h)
// so a tapering charge current near "full" doesn't flap the device in and
// out of charging mode. Always false when no INA219 is fitted -- a
// battery-less test device on USB power draws no charge current and
// therefore never reports charging, staying in ordinary operation.
bool isCharging();

} // namespace Power
