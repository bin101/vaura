#include "roster.h"

#include <string.h>

#include "config.h"
#include "stats.h"

namespace Roster {

namespace {
struct Peer {
  bool used = false;
  uint16_t nodeId = 0;
  char nickname[Protocol::kNicknameFieldLen + 1] = {0};
  // Explicit flag, not "lastSeenMs == 0": millis() legitimately passes 0 once
  // every ~49.7 days, and a heartbeat stamped in exactly that millisecond
  // would otherwise make the peer look brand-new on its next heartbeat
  // (spurious DA event + EMA reset mid-episode).
  bool everSeen = false;
  uint32_t lastSeenMs = 0;
  // Two RSSI EMAs: fast = "where the signal is right now", slow = the
  // baseline it usually sits at. Falling-back is fast dropping well below
  // slow -- catches gradual drift as well as sharp drops (see config.h).
  float rssiEmaFast = 0;
  float rssiEmaSlow = 0;
  int16_t lastRawRssi = 0;
  uint16_t batteryMillivolts = 0; // from the peer's heartbeat; 0 = no INA219 there
  // Latched once the peer's battery dipped below BATTERY_LOW_MV, so "AKKU" is
  // logged exactly once per low episode (cleared above BATTERY_LOW_CLEAR_MV).
  bool batteryLowNotified = false;
  // Set once a second heartbeat has been folded into the EMAs, so the very
  // first reading after a peer appears isn't compared against itself.
  bool hasPreviousEma = false;
  bool fallingBack = false;
  bool droppedOff = false;
  // Manually removed from the group via the reminder prompt (see ui.cpp).
  // Only ever true while droppedOff is; cleared by the peer's next heartbeat,
  // so a rider who rejoins later is picked up automatically.
  bool dismissed = false;
};

Peer peers[MAX_PEERS];

char lastEventBuf[24] = {0};
uint32_t lastEventMs = 0;

// Separate from lastEventMs: only SCHWACH/ABRISS bump this one, not DA/ZURUECK.
// The UI uses it to wake the (otherwise sleeping) display specifically for the
// two events that are actually worth interrupting the rider for.
uint32_t lastAlertMs = 0;
uint32_t alertGeneration = 0; // one increment per SCHWACH/ABRISS, never reset
AlertType lastAlertType_ = AlertType::FallingBack; // arbitrary until lastAlertMs != 0

char nicknameFallbackBuf[8];

void logEvent(const char *nickname, const char *suffix, uint32_t nowMs) {
  snprintf(lastEventBuf, sizeof(lastEventBuf), "%s %s", nickname, suffix);
  lastEventMs = nowMs;
}

void logAlert(const char *nickname, const char *suffix, uint32_t nowMs, AlertType type) {
  logEvent(nickname, suffix, nowMs);
  lastAlertMs = nowMs;
  alertGeneration++;
  lastAlertType_ = type;
}

Peer *findOrCreate(uint16_t nodeId) {
  Peer *free_slot = nullptr;
  for (auto &p : peers) {
    if (p.used && p.nodeId == nodeId) {
      return &p;
    }
    if (!p.used && free_slot == nullptr) {
      free_slot = &p;
    }
  }
  if (free_slot == nullptr) {
    return nullptr; // roster full -- silently ignore, MAX_PEERS is generous for club rides
  }
  // Reset every field, not just the flags: slots are never released today,
  // but if eviction is ever added, a reused slot must not inherit the previous
  // occupant's nickname or everSeen (onHeartbeat derives "new peer" from it).
  free_slot->used = true;
  free_slot->nodeId = nodeId;
  free_slot->nickname[0] = '\0';
  free_slot->everSeen = false;
  free_slot->lastSeenMs = 0;
  free_slot->rssiEmaFast = 0;
  free_slot->rssiEmaSlow = 0;
  free_slot->lastRawRssi = 0;
  free_slot->batteryMillivolts = 0;
  free_slot->batteryLowNotified = false;
  free_slot->hasPreviousEma = false;
  free_slot->fallingBack = false;
  free_slot->droppedOff = false;
  free_slot->dismissed = false;
  return free_slot;
}
} // namespace

void begin() {
  for (auto &p : peers) {
    p.used = false;
  }
}

void onHeartbeat(uint16_t nodeId, const char *nickname, uint16_t batteryMillivolts, int16_t rssi) {
  uint32_t now = millis();
  Peer *peer = findOrCreate(nodeId);
  if (peer == nullptr) {
    return;
  }

  bool isNewPeer = !peer->everSeen;
  bool wasDroppedOff = peer->droppedOff;

  strncpy(peer->nickname, nickname, sizeof(peer->nickname) - 1);
  peer->nickname[sizeof(peer->nickname) - 1] = '\0';
  peer->everSeen = true;
  peer->lastSeenMs = now;
  peer->droppedOff = false;
  peer->dismissed = false; // a manually removed rider rejoins automatically
  peer->lastRawRssi = rssi;
  peer->batteryMillivolts = batteryMillivolts;

  // A peer running out of juice is worth a note on the idle screen (their
  // device going dark looks exactly like an Abriss to everyone). Not an
  // alert: no wake/beep, it shows on the next glance and in the rider list.
  // mv == 0 means "no battery monitor fitted there" -- nothing to judge.
  if (batteryMillivolts > 0) {
    if (!peer->batteryLowNotified && batteryMillivolts < BATTERY_LOW_MV) {
      peer->batteryLowNotified = true;
      logEvent(peer->nickname, "AKKU", now);
    } else if (peer->batteryLowNotified && batteryMillivolts > BATTERY_LOW_CLEAR_MV) {
      peer->batteryLowNotified = false;
    }
  }

  if (isNewPeer || wasDroppedOff) {
    // Fresh start for the trend in both cases. After a gap ("ZURUECK") the old
    // EMAs are stale -- the peer likely returns with a weak-but-recovering
    // signal, and judging that against the pre-gap baseline would log a
    // spurious SCHWACH right on top of the ZURUECK.
    peer->rssiEmaFast = static_cast<float>(rssi);
    peer->rssiEmaSlow = static_cast<float>(rssi);
    peer->hasPreviousEma = false; // need one more heartbeat before a trend can be judged
    peer->fallingBack = false;
    logEvent(peer->nickname, isNewPeer ? "DA" : "ZURUECK", now);
  } else {
    peer->rssiEmaFast =
        RSSI_EMA_ALPHA_FAST * static_cast<float>(rssi) + (1.0f - RSSI_EMA_ALPHA_FAST) * peer->rssiEmaFast;
    peer->rssiEmaSlow =
        RSSI_EMA_ALPHA_SLOW * static_cast<float>(rssi) + (1.0f - RSSI_EMA_ALPHA_SLOW) * peer->rssiEmaSlow;

    bool wasFallingBack = peer->fallingBack;
    // Floor is user-adjustable (0..10, settings menu); re-evaluated on every
    // heartbeat, so a changed step takes effect within one interval.
    int16_t floorDbm = fallingBackFloorDbm(DeviceConfig::fallingBackSensitivity());
    peer->fallingBack = peer->hasPreviousEma &&
                         peer->rssiEmaFast < floorDbm &&
                         peer->rssiEmaFast < (peer->rssiEmaSlow - RSSI_FALLING_BACK_DROP_DB);
    peer->hasPreviousEma = true;

    if (peer->fallingBack && !wasFallingBack) {
      logAlert(peer->nickname, "SCHWACH", now, AlertType::FallingBack);
    }
  }
}

void tick(uint32_t nowMs) {
  for (auto &p : peers) {
    if (!p.used || p.droppedOff) {
      continue;
    }
    uint32_t silentMs = nowMs - p.lastSeenMs;
    if (silentMs > DROPPED_OFF_TIMEOUT_MS) {
      p.droppedOff = true;
      p.fallingBack = false;
      Stats::countDropOff();
      logAlert(p.nickname, "ABRISS", nowMs, AlertType::DroppedOff);
    }
  }
}

int activeCount() {
  int count = 0;
  for (auto &p : peers) {
    if (p.used && !p.droppedOff) {
      count++;
    }
  }
  return count;
}

int totalCount() {
  int count = 0;
  for (auto &p : peers) {
    if (p.used && !p.dismissed) {
      count++;
    }
  }
  return count;
}

bool peerInfo(int index, PeerInfo &out) {
  int i = 0;
  for (auto &p : peers) {
    if (!p.used || p.dismissed) {
      continue;
    }
    if (i == index) {
      out.nodeId = p.nodeId;
      strncpy(out.nickname, p.nickname, sizeof(out.nickname) - 1);
      out.nickname[sizeof(out.nickname) - 1] = '\0';
      out.rssiDbm = static_cast<int16_t>(lroundf(p.rssiEmaFast));
      out.lastRawRssiDbm = p.lastRawRssi;
      out.batteryMillivolts = p.batteryMillivolts;
      out.fallingBack = p.fallingBack;
      out.droppedOff = p.droppedOff;
      out.lastSeenMs = p.lastSeenMs;
      return true;
    }
    i++;
  }
  return false;
}

bool anyDroppedOff() {
  for (auto &p : peers) {
    if (p.used && p.droppedOff && !p.dismissed) {
      return true;
    }
  }
  return false;
}

bool longestDropped(PeerInfo &out) {
  int bestIndex = -1;
  uint32_t now = millis();
  uint32_t bestSilentMs = 0;
  int i = 0;
  for (auto &p : peers) {
    if (!p.used || p.dismissed) {
      continue;
    }
    // Compare by age-of-last-heartbeat, not raw lastSeenMs: the unsigned
    // difference stays correct across the millis() wraparound.
    uint32_t silentMs = now - p.lastSeenMs;
    if (p.droppedOff && (bestIndex < 0 || silentMs > bestSilentMs)) {
      bestIndex = i;
      bestSilentMs = silentMs;
    }
    i++;
  }
  return bestIndex >= 0 && peerInfo(bestIndex, out);
}

void dismiss(uint16_t nodeId) {
  for (auto &p : peers) {
    // The droppedOff check makes this a no-op if the rider's heartbeat came
    // back between showing the prompt and the long press -- never silently
    // hide someone who is actually present.
    if (p.used && p.nodeId == nodeId && p.droppedOff) {
      p.dismissed = true;
    }
  }
}

const char *nicknameFor(uint16_t nodeId) {
  for (auto &p : peers) {
    if (p.used && p.nodeId == nodeId) {
      return p.nickname;
    }
  }
  snprintf(nicknameFallbackBuf, sizeof(nicknameFallbackBuf), "#%04X", nodeId);
  return nicknameFallbackBuf;
}

const char *lastEventText() { return lastEventMs == 0 ? nullptr : lastEventBuf; }

uint32_t lastEventTimestampMs() { return lastEventMs; }

uint32_t lastAlertTimestampMs() { return lastAlertMs; }

uint32_t lastAlertGeneration() { return alertGeneration; }

AlertType lastAlertType() { return lastAlertType_; }

} // namespace Roster
