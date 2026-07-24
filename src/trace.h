// Onboard calibration trace: an opt-in ring buffer of per-peer RSSI/EMA/
// baseline/floor/verdict samples, recorded once per Roster::tick() (~1/s),
// for post-ride analysis of how the self-calibrated falling-back floor
// (see calibration.{h,cpp}) actually tracked the group -- the field-test
// evidence complementing the range-test screen's live readout (ui.cpp).
//
// Entirely passive: never influences any detection logic, just observes it.
// Off by default and controlled via the serial console (`trace on`/`off`/
// `dump`/`clear`, see config.cpp) -- nobody wants a ride's worth of samples
// dumped unasked-for, and recording costs nothing while disabled.
#pragma once

#include <Arduino.h>

namespace Trace {

// Allocates the ring buffer (PSRAM if available, else a small RAM fallback --
// see TRACE_CAPACITY_*_SAMPLES in config.h). Call once from setup(), after
// Serial.begin() (logs which backing store it picked).
void begin();

// Starts/stops recording. Disabling does not clear what's already recorded --
// `trace dump` still works afterwards, and re-enabling resumes appending
// where it left off (the ring wraps once full, oldest samples first to go).
void setEnabled(bool on);
bool isEnabled();

// Discards all recorded samples (but keeps the allocated buffer and the
// enabled/disabled state).
void clear();

// Records one row per currently-tracked peer (via Roster::peerInfo()) plus
// the group baseline/floor in effect right now (Roster::groupBaselineDbm()/
// activeFloorDbm()/baselineEstablished()). No-op if disabled or the buffer
// failed to allocate. Call once per Roster::tick() interval, right after it,
// from main.cpp's loop() -- deliberately not called from inside Roster
// itself, so this debug-only feature can never affect the safety-relevant
// module it observes.
void sample(uint32_t nowMs);

// Prints the whole ring buffer as CSV (header row first, oldest sample
// first) over Serial, for the `trace dump` console command. Blocking --
// meant to be run stopped at the roadside after a ride, not mid-ride.
void dumpCsv();

// Capacity actually allocated and how many samples are currently stored --
// for the `trace on`/`trace dump` command echoes.
size_t capacity();
size_t count();

} // namespace Trace
