// Entry point: wires the modules together (config/power/radio/roster/ui),
// runs the single-button gestures, the heartbeat scheduler, and the incoming-
// packet dispatch. See README.md for the full picture and the wiring plan.
#include <Arduino.h>
#include <OneButton.h>

#include "config.h"
#include "power.h"
#include "protocol.h"
#include "radio.h"
#include "roster.h"
#include "stats.h"
#include "ui.h"

namespace {
OneButton button(PIN_BUTTON, /*activeLow=*/true, /*pullupActive=*/true);

uint32_t nextHeartbeatMs = 0;
uint32_t nextRosterTickMs = 0;

// Tracks the last WARNING seq reacted to *per sender*, so the guaranteed 2nd
// copy (see WARNING_REPEAT_DELAY_MS / Ui::sendWarning()) doesn't trigger a
// second beep/toast for what is really the same warning arriving twice.
// Per-sender rather than one global slot: if rider A and rider B warn almost
// simultaneously (A, B, A-repeat), a single slot would have forgotten A by the
// time its repeat arrives and would alert twice for the same warning.
struct SeenWarning {
  bool used = false;
  uint16_t senderId = 0;
  uint8_t seq = 0;
};
SeenWarning seenWarnings[MAX_PEERS];

// True if this exact (senderId, seq) was already reacted to; records it either way.
bool isRepeatedWarning(uint16_t senderId, uint8_t seq) {
  SeenWarning *slot = nullptr;
  for (auto &w : seenWarnings) {
    if (w.used && w.senderId == senderId) {
      slot = &w;
      break;
    }
    if (!w.used && slot == nullptr) {
      slot = &w;
    }
  }
  if (slot == nullptr) {
    return false; // table full (warnings from > MAX_PEERS senders) -- rather alert twice than never
  }
  bool repeat = slot->used && slot->senderId == senderId && slot->seq == seq;
  slot->used = true;
  slot->senderId = senderId;
  slot->seq = seq;
  return repeat;
}

// Randomized around HEARTBEAT_INTERVAL_MS so a group of devices that happened
// to boot in sync don't keep colliding on every single heartbeat.
uint32_t randomizedHeartbeatInterval() {
  long jitter = random(-static_cast<long>(HEARTBEAT_JITTER_MS), static_cast<long>(HEARTBEAT_JITTER_MS) + 1);
  return HEARTBEAT_INTERVAL_MS + jitter;
}

void sendHeartbeat() {
  uint8_t buf[Protocol::kMaxPacketLen];
  size_t len = Protocol::encodeHeartbeat(buf, DeviceConfig::nodeId(), DeviceConfig::nextSeq(),
                                          Power::batteryMillivolts(), DeviceConfig::nickname());
  Radio::send(buf, len);
}

// Returns true if a packet was dispatched, so the caller can drain a burst
// (radio.cpp's RX path holds only one frame at a time -- see the comment in
// loop() below).
bool dispatchIncomingPacket() {
  Protocol::DecodedPacket pkt;
  int16_t rssi;
  if (!Radio::poll(pkt, rssi)) {
    return false;
  }

  switch (pkt.type) {
    case Protocol::MsgType::Heartbeat:
      Roster::onHeartbeat(pkt.senderId, pkt.nickname, pkt.batteryMillivolts, rssi);
      break;
    case Protocol::MsgType::Warning: {
      if (!isRepeatedWarning(pkt.senderId, pkt.seq)) {
        Stats::countWarningReceived();
        Ui::onIncomingWarning(Roster::nicknameFor(pkt.senderId), pkt.warningType);
      }
      break;
    }
  }
  return true;
}
} // namespace

void setup() {
  Serial.begin(115200);
  // TEMP DEBUG: XIAO's native USB-CDC drops early prints if the host terminal
  // hasn't reattached yet after a reset. Bounded so a USB-less field boot
  // (the common case) isn't delayed -- revert before merging.
  uint32_t serialWaitStart = millis();
  while (!Serial && millis() - serialWaitStart < 2000) {
    delay(10);
  }
  delay(300); // brief warm-up so early log lines aren't lost on cold boot

  DeviceConfig::begin();
  Power::begin();
  Ui::begin();
  Radio::begin();
  Roster::begin();

  // random() (used for heartbeat jitter, see randomizedHeartbeatInterval())
  // is otherwise unseeded, so devices booted around the same time would draw
  // the identical jitter sequence and never stop colliding on air. Seed from
  // the hardware RNG, which is independent of Wi-Fi/BT state.
  randomSeed(esp_random());

  button.attachClick(Ui::onButtonClick);
  button.attachLongPressStart(Ui::onButtonLongPress);
  button.attachDoubleClick(Ui::onButtonDoubleClick);
  button.setClickMs(BUTTON_CLICK_MS); // single-click latency vs. double-click window, see config.h
  button.setPressMs(BUTTON_LONG_PRESS_MS); // hold time until "long press", see config.h

  uint32_t now = millis();
  nextHeartbeatMs = now + randomizedHeartbeatInterval();
  nextRosterTickMs = now + 1000;

  Serial.println("Setup complete.");
}

void loop() {
  DeviceConfig::pollSerialConsole();
  button.tick();
  // Drain fully: radio.cpp's RX path holds only the single most-recently
  // received frame, so a burst of heartbeats arriving between two loop()
  // iterations would otherwise lose all but the last one.
  while (dispatchIncomingPacket()) {
  }

  uint32_t now = millis();

  // Signed difference instead of `now >= deadline`: stays correct across the
  // ~49-day millis() wraparound (same idiom for every deadline in ui.cpp).
  if (static_cast<int32_t>(now - nextHeartbeatMs) >= 0) {
    // No heartbeat while the boot-time channel selection is open -- the
    // device must not announce itself to a group it may be about to leave.
    if (!Ui::bootChannelPending()) {
      sendHeartbeat();
    }
    nextHeartbeatMs = now + randomizedHeartbeatInterval();
  }

  if (static_cast<int32_t>(now - nextRosterTickMs) >= 0) {
    Roster::tick(now);
    nextRosterTickMs = now + 1000;
  }

  Ui::setBatteryStatusForDisplay(Power::batteryPercent(), Power::available(), Power::isLow());
  Ui::tick();
}
