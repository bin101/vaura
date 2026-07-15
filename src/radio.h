// Thin wrapper around RadioLib's SX1262 driver: init, interrupt-driven receive,
// and a legally-required EU868 duty-cycle guard on transmit.
#pragma once

#include <Arduino.h>

#include "protocol.h"

namespace Radio {

// Initializes SPI + the SX1262, arms the RF switch and starts listening.
// Applies the persisted group channel (DeviceConfig::channel() -- main.cpp
// runs DeviceConfig::begin() first). Logs and returns without halting on
// failure -- a dead radio should not brick the rest of the device
// (display/buttons still work for local use).
void begin();

// Re-programs the sync word for a new group channel (0..GROUP_CHANNEL_MAX)
// and resumes listening. For runtime changes from the settings menu; the
// caller also persists the value via DeviceConfig::setChannel().
void applyChannel(uint8_t channel);

// Non-blocking. Returns true and fills `pkt`/`rssi` if a new, valid packet
// arrived since the last call. Re-arms the receiver internally. (No SNR out:
// the SX126x only reports a meaningful SNR for LoRa packets, not FSK.)
bool poll(Protocol::DecodedPacket &pkt, int16_t &rssi);

// Blocking send (airtime is a few ms at our GFSK bitrate). Returns false
// without transmitting if the EU868 1% duty-cycle budget is exhausted --
// callers should treat that as "try again shortly", not a hard error.
bool send(uint8_t *data, size_t len);

} // namespace Radio
