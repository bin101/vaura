// Pure math behind the self-calibrating "falling back" floor: instead of a
// fixed absolute RSSI floor (guessed once, by hand, and wrong as soon as the
// terrain or group size changes), the floor tracks a "group baseline" -- the
// RSSI the intact group is actually riding at right now -- minus a margin.
// A rider genuinely falling back then sags below "everyone else, right now"
// rather than below a number picked on a different ride in different woods.
//
// Deliberately Arduino-free (like coop.h/charging_decision.h) so this runs in
// the native unit-test environment (pio test -e native): every tunable is a
// function parameter, not a #define, and callers (Roster) supply the actual
// values from config.h.
#pragma once

#include <stdint.h>

namespace Calibration {

// Median of the first `n` entries of `values` (in dBm, or any comparable
// unit -- used here for the group's slow-EMA RSSI values). Sorts `values` in
// place (insertion sort: n is at most MAX_PEERS+1, same "small n, keep it
// simple" tradeoff as the rider-list sort in ui.cpp). Median rather than mean
// so a single rider already falling back (an outlier by construction) can't
// drag the baseline down with them -- it takes a majority to move it.
// Returns 0 for n <= 0; callers must gate on their own peer count/
// `established` flag rather than trust this return value to signal validity.
float medianDbm(float *values, int n);

// One exponential-smoothing step of the group baseline toward `sample`.
// Keeps the baseline from jumping around every second as the peer set
// changes (a rider joining/leaving shifts the instantaneous median). Same
// alpha-blend shape as Roster's own per-peer RSSI EMAs (see roster.cpp) --
// alpha closer to 1 tracks faster, closer to 0 is steadier.
float smoothBaseline(float prevBaselineDbm, float sampleDbm, float alpha);

// Derives the "falling back" floor: `baselineDbm - marginDb`, clamped to
// [floorMinDbm, floorMaxDbm]. Falls back to `fallbackFloorDbm` verbatim when
// `baselineEstablished` is false (too few peers around to trust a group
// baseline -- e.g. right after boot, or riding alone) -- this reproduces the
// old fixed-floor behaviour exactly until there's enough company to
// calibrate against.
int16_t deriveFloorDbm(float baselineDbm, int marginDb, bool baselineEstablished, int16_t fallbackFloorDbm,
                        int16_t floorMinDbm, int16_t floorMaxDbm);

// Linear step mapping for a 0..maxLevel ruler (shared shape with the old
// fallingBackFloorDbm(): each step is worth `dbPerStep` dB) but inverted in
// sense -- `level` is now a *sensitivity*, so higher level = smaller margin =
// triggers sooner. level is clamped to [0, maxLevel] first so an
// out-of-range value (shouldn't happen; NVS load already validates) can't
// produce a negative margin. constexpr (and defined here, not in the .cpp)
// so config.h's fallingBackMarginDb() can use it in a static_assert.
constexpr int marginDbForLevel(uint8_t level, uint8_t maxLevel, int dbPerStep) {
  return dbPerStep * (static_cast<int>(maxLevel) - static_cast<int>(level > maxLevel ? maxLevel : level));
}

} // namespace Calibration
