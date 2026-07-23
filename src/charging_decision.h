// Pure hysteresis + dwell-time state machine that turns a noisy battery
// charge-current reading into a stable "is charging" boolean. Split out from
// power.cpp (which owns the actual INA219 reads) so this decision logic has
// no Arduino dependency and runs in the native unit tests (pio test -e
// native), like battery_curve.h.
#pragma once

#include <stdint.h>

struct ChargingDecisionState {
  bool charging = false;
  // millis() timestamp of the most recent reading that was still at or above
  // exitMa -- used to measure how long the current has continuously stayed
  // below exitMa before actually leaving charging mode. Doubles as "not yet
  // charging" state (irrelevant while charging == false).
  uint32_t lastAboveExitMs = 0;
};

// currentMa: charge current in mA, sign-normalized so positive means "into
// the battery" (see config.h's INA219_CURRENT_CHARGE_SIGN). nowMs: the
// caller's current millis() timestamp -- passed in rather than read here so
// this stays Arduino-free and testable with fabricated time.
//
// Enters charging once currentMa reaches enterMa. Once charging, only exits
// after currentMa has stayed below exitMa continuously for at least
// minDwellMs -- a LiPo's charge current tapers off gradually near full, and
// without this dwell the device would flap in and out of charging mode on
// every small dip. The dwell window is measured from the last sample that
// was still at/above exitMa, not from when the dip visually started -- a
// deliberately conservative (slightly longer) measure that needs no extra
// state. enterMa should be greater than exitMa (the hysteresis gap); that is
// a config.h concern, not validated here.
bool updateChargingDecision(ChargingDecisionState &state, int16_t currentMa, uint32_t nowMs,
                             int16_t enterMa, int16_t exitMa, uint32_t minDwellMs);
