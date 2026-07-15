// Per-outing counters for the statistics screen (settings -> "Statistik").
// Deliberately not persisted: the numbers describe the current power-on tour,
// a fresh start per ride is the point. Ride time itself comes straight from
// millis() at render time and needs no counter here.
#pragma once

#include <stdint.h>

namespace Stats {

// Own warnings that actually left the antenna (failed duty-cycle/radio
// attempts don't count; a succeeding retry of a failed first copy does).
void countWarningSent();
// Distinct incoming warnings (the guaranteed repeat copy is not re-counted).
void countWarningReceived();
// ABRISS events, including the same rider dropping multiple times.
void countDropOff();

uint32_t warningsSent();
uint32_t warningsReceived();
uint32_t dropOffs();

} // namespace Stats
