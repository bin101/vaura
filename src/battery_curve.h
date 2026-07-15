// Pure 1S-LiPo voltage-to-percent mapping, shared by the local battery readout
// (power.cpp) and the peers' battery display (their heartbeats carry raw
// millivolts). No Arduino dependency, so it runs in the native unit tests
// (pio test -e native) as-is.
#pragma once

#include <stdint.h>

// Rough open-circuit-ish curve matching a light, steady load (OLED + MCU +
// occasional radio burst). Deliberately conservative at the low end since LiPo
// cells sag further under any load. Clamps to [0, 100]; not a fuel gauge.
uint8_t batteryPercentFromMillivolts(uint16_t mv);
