#pragma once

#include <stdint.h>

// Pure, Arduino-free derivation of the on-wire node id from the ESP32's
// factory MAC (ESP.getEfuseMac()). Kept header-only and dependency-free so
// the native test suite (pio test -e native) can exercise it directly --
// see test/test_node_id/test_main.cpp.
//
// The low 3 octets of the factory MAC are the Espressif OUI (vendor prefix),
// identical across every chip from the same manufacturing block -- e.g. all
// XIAO ESP32-S3 boards from one batch share it. Per-device uniqueness lives
// in the *upper* 3 octets, the NIC-specific portion. Folding those into 16
// bits keeps the id compact while actually drawing on the entropy that
// varies between devices.
inline uint16_t nodeIdFromMac(uint64_t mac) {
  uint32_t nic = static_cast<uint32_t>(mac >> 24) & 0xFFFFFF; // MAC octets 3..5
  return static_cast<uint16_t>(nic ^ (nic >> 16));
}
