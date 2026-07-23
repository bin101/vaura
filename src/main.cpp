// Entry point: wires the modules together (config/power/radio/roster/ui),
// runs the single-button gestures, the heartbeat scheduler, and the incoming-
// packet dispatch. See README.md for the full picture and the wiring plan.
#include <Arduino.h>
#include <OneButton.h>

#include "config.h"
#include "coop.h"
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

// Cooperative drop-off confirmation (see coop.h): a gossip send is armed the
// moment Coop::sendDue() goes true, then actually transmitted after a short
// random delay so devices reacting to the same event don't all key up at
// once (mirrors the heartbeat jitter idiom below, at a shorter timescale).
bool gossipSendArmed = false;
uint32_t nextGossipSendMs = 0;

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

// Transmits a due gossip packet (own opinion changes/refreshes and/or
// pending relays), jittered so several devices reacting to the same event
// don't all key up in the same instant. No-op (and no radio traffic at all)
// whenever the group is entirely OK -- see Coop::sendDue().
void serviceGossip(uint32_t now) {
  if (!gossipSendArmed) {
    if (!Coop::sendDue(now)) {
      return;
    }
    gossipSendArmed = true;
    nextGossipSendMs = now + static_cast<uint32_t>(random(0, static_cast<long>(Coop::kSendJitterMs) + 1));
    return;
  }
  if (static_cast<int32_t>(now - nextGossipSendMs) < 0) {
    return; // jitter window not elapsed yet
  }
  gossipSendArmed = false;

  Protocol::GossipEntry entries[Protocol::kMaxGossipEntries];
  uint8_t count = Coop::collectOutgoing(entries, static_cast<uint8_t>(Protocol::kMaxGossipEntries), now);
  if (count == 0) {
    return; // whatever was due already resolved (e.g. the opinion was retracted) -- nothing left to send
  }
  uint8_t buf[Protocol::kMaxPacketLen];
  size_t len = Protocol::encodeGossip(buf, DeviceConfig::nodeId(), DeviceConfig::nextSeq(), entries, count);
  Radio::send(buf, len); // best-effort, like heartbeats/warnings -- see coop.h's kRefreshMs comment
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
    case Protocol::MsgType::Gossip: {
      uint32_t now = millis();
      for (uint8_t i = 0; i < pkt.gossipCount; i++) {
        Coop::mergeEntry(DeviceConfig::nodeId(), pkt.gossipEntries[i], now);
      }
      break;
    }
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
  delay(300); // brief warm-up so early log lines aren't lost on cold boot

  DeviceConfig::begin();
  Power::begin();
  Ui::begin();
  Radio::begin();
  Roster::begin();
  Coop::begin();

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
    Roster::tick(now); // also drives Coop::tick() internally
    nextRosterTickMs = now + 1000;
  }

  // No boot-channel gating here (unlike sendHeartbeat() above): gossip only
  // ever fires in reaction to an actual WEAK/LOST opinion or relay, which
  // can't exist yet this early (Roster hasn't heard anyone), so there is
  // nothing to hold back.
  serviceGossip(now);

  Ui::setBatteryStatusForDisplay(Power::batteryPercent(), Power::available(), Power::isLow());
  Ui::tick();
}
