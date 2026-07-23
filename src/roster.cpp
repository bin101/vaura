#include "roster.h"

#include <string.h>

#include "config.h"
#include "coop.h"
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
  // (spurious "joined" event + EMA reset mid-episode).
  bool everSeen = false;
  uint32_t lastSeenMs = 0;
  // Two RSSI EMAs: fast = "where the signal is right now", slow = the
  // baseline it usually sits at. Falling-back is fast dropping well below
  // slow -- catches gradual drift as well as sharp drops (see config.h).
  float rssiEmaFast = 0;
  float rssiEmaSlow = 0;
  int16_t lastRawRssi = 0;
  uint16_t batteryMillivolts = 0; // from the peer's heartbeat; 0 = no INA219 there
  // Latched once the peer's battery dipped below BATTERY_LOW_MV, so "battery"
  // is logged exactly once per low episode (cleared above BATTERY_LOW_CLEAR_MV).
  bool batteryLowNotified = false;
  // Set once a second heartbeat has been folded into the EMAs, so the very
  // first reading after a peer appears isn't compared against itself.
  bool hasPreviousEma = false;
  // This device's own opinion of this peer, purely from local heartbeat
  // timing/RSSI trend -- computed exactly as before cooperative drop-off
  // confirmation existed. Feeds Coop's gossip mesh (see setOwnOpinion calls
  // below); no longer decides the alert by itself (see effectiveWeak/
  // effectiveDropped and Roster::tick()).
  bool localWeak = false;
  bool localLost = false;
  // The group-consensus-gated verdict that actually drives alerts and
  // everything UI-facing (PeerInfo.fallingBack/droppedOff, activeCount(),
  // anyDroppedOff(), the reminder/removal prompt, decorateNickname()'s
  // markers in ui.cpp). Falls back to localWeak/localLost verbatim when too
  // few devices are around to form a consensus -- see Roster::tick().
  bool effectiveWeak = false;
  bool effectiveDropped = false;
  // Manually removed from the group via the reminder prompt (see ui.cpp).
  // Only ever true while effectiveDropped is; cleared by the peer's next
  // heartbeat, so a rider who rejoins later is picked up automatically.
  bool dismissed = false;
};

Peer peers[MAX_PEERS];

char lastEventBuf[24] = {0};
uint32_t lastEventMs = 0;

// Separate from lastEventMs: only "falling back"/"dropped off" bump this one,
// not "joined"/"back". The UI uses it to wake the (otherwise sleeping)
// display specifically for the two events that are actually worth
// interrupting the rider for.
uint32_t lastAlertMs = 0;
uint32_t alertGeneration = 0; // one increment per "falling back"/"dropped off", never reset
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
  free_slot->localWeak = false;
  free_slot->localLost = false;
  free_slot->effectiveWeak = false;
  free_slot->effectiveDropped = false;
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
  // Trigger for the EMA reset below is our own local silence, not the
  // group-consensus flag -- if the group never agreed we'd lost this peer
  // (too small a group, or the others still heard them) but *we* locally
  // lost contact for a while, our own EMAs are still stale from that gap and
  // must reset the same way.
  bool wasLocalLost = peer->localLost;

  strncpy(peer->nickname, nickname, sizeof(peer->nickname) - 1);
  peer->nickname[sizeof(peer->nickname) - 1] = '\0';
  peer->everSeen = true;
  peer->lastSeenMs = now;
  if (peer->localLost) {
    peer->localLost = false;
    Coop::setOwnOpinion(DeviceConfig::nodeId(), nodeId, Protocol::GossipStatus::Lost, /*active=*/false, now);
  }
  peer->dismissed = false; // a manually removed rider rejoins automatically
  peer->lastRawRssi = rssi;
  peer->batteryMillivolts = batteryMillivolts;

  // A peer running out of juice is worth a note on the idle screen (their
  // device going dark looks exactly like a drop-off to everyone). Not an
  // alert: no wake/beep, it shows on the next glance and in the rider list.
  // mv == 0 means "no battery monitor fitted there" -- nothing to judge.
  if (batteryMillivolts > 0) {
    if (!peer->batteryLowNotified && batteryMillivolts < BATTERY_LOW_MV) {
      peer->batteryLowNotified = true;
      logEvent(peer->nickname, "BATTERY", now);
    } else if (peer->batteryLowNotified && batteryMillivolts > BATTERY_LOW_CLEAR_MV) {
      peer->batteryLowNotified = false;
    }
  }

  if (isNewPeer || wasLocalLost) {
    // Fresh start for the trend in both cases. After a gap ("back") the old
    // EMAs are stale -- the peer likely returns with a weak-but-recovering
    // signal, and judging that against the pre-gap baseline would log a
    // spurious "falling back" right on top of the "back".
    peer->rssiEmaFast = static_cast<float>(rssi);
    peer->rssiEmaSlow = static_cast<float>(rssi);
    peer->hasPreviousEma = false; // need one more heartbeat before a trend can be judged
    if (peer->localWeak) {
      peer->localWeak = false;
      Coop::setOwnOpinion(DeviceConfig::nodeId(), nodeId, Protocol::GossipStatus::Weak, /*active=*/false, now);
    }
    logEvent(peer->nickname, isNewPeer ? "JOINED" : "BACK", now);
  } else {
    peer->rssiEmaFast =
        RSSI_EMA_ALPHA_FAST * static_cast<float>(rssi) + (1.0f - RSSI_EMA_ALPHA_FAST) * peer->rssiEmaFast;
    peer->rssiEmaSlow =
        RSSI_EMA_ALPHA_SLOW * static_cast<float>(rssi) + (1.0f - RSSI_EMA_ALPHA_SLOW) * peer->rssiEmaSlow;

    bool wasLocalWeak = peer->localWeak;
    // Floor is user-adjustable (0..10, settings menu); re-evaluated on every
    // heartbeat, so a changed step takes effect within one interval.
    int16_t floorDbm = fallingBackFloorDbm(DeviceConfig::fallingBackSensitivity());
    peer->localWeak = peer->hasPreviousEma &&
                       peer->rssiEmaFast < floorDbm &&
                       peer->rssiEmaFast < (peer->rssiEmaSlow - RSSI_FALLING_BACK_DROP_DB);
    peer->hasPreviousEma = true;

    if (peer->localWeak != wasLocalWeak) {
      // Feed the change into the gossip mesh -- this device's opinion alone
      // no longer raises the alert, see Roster::tick().
      Coop::setOwnOpinion(DeviceConfig::nodeId(), nodeId, Protocol::GossipStatus::Weak, peer->localWeak, now);
    }
  }
}

void tick(uint32_t nowMs) {
  // This device + every peer it's still tracking (dismissed ones excluded,
  // same headcount totalCount() already reports) -- the consensus quorum
  // denominator, computed once per tick rather than per peer.
  int groupSize = totalCount() + 1;

  for (auto &p : peers) {
    if (!p.used) {
      continue;
    }

    // Local (single-device) verdict: unchanged silence-timeout detection,
    // now decoupled from the alert itself -- it only feeds the group
    // consensus (Coop::setOwnOpinion) and, when too few devices are around
    // to corroborate, *is* the alert (see the InsufficientObservers case
    // below), exactly as before cooperative confirmation existed.
    bool wasLocalLost = p.localLost;
    uint32_t silentMs = nowMs - p.lastSeenMs;
    p.localLost = silentMs > DROPPED_OFF_TIMEOUT_MS;
    if (p.localLost && !wasLocalLost) {
      Coop::setOwnOpinion(DeviceConfig::nodeId(), p.nodeId, Protocol::GossipStatus::Lost, /*active=*/true, nowMs);
    }
    // (localLost's falling edge -- the peer coming back -- is handled in
    // onHeartbeat(), which is where we actually learn about it.)

    // Dismissed peers stop counting/alerting for this device entirely --
    // same as before this feature existed -- but their local opinion above
    // still keeps feeding the mesh, since it remains useful evidence for the
    // *rest* of the group even after this device has muted its own view.
    // effectiveDropped/effectiveWeak simply freeze at their last value here;
    // harmless, since every reader (peerInfo(), activeCount(), ...) already
    // excludes dismissed peers.
    if (p.dismissed) {
      continue;
    }

    bool wasEffectiveDropped = p.effectiveDropped;
    bool wasEffectiveWeak = p.effectiveWeak;

    Coop::Consensus consensus = Coop::subjectConsensus(groupSize, p.nodeId, nowMs);
    // The Consensus->flags mapping and the rising-edge alert decision are
    // pure functions of a few booleans, pulled out into Coop so this
    // safety-relevant logic gets native test coverage (see coop.h).
    Coop::EffectiveState verdict = Coop::resolveVerdict(consensus, p.localLost, p.localWeak);
    p.effectiveDropped = verdict.dropped;
    p.effectiveWeak = verdict.weak;

    Coop::AlertDecision decision =
        Coop::decideAlert(wasEffectiveDropped, p.effectiveDropped, wasEffectiveWeak, p.effectiveWeak);
    if (decision.fireDropped) {
      Stats::countDropOff();
      logAlert(p.nickname, "DROPPED", nowMs, AlertType::DroppedOff);
    } else if (decision.fireWeak) {
      logAlert(p.nickname, "WEAK", nowMs, AlertType::FallingBack);
    }
  }

  Coop::tick(DeviceConfig::nodeId(), nowMs);
}

int activeCount() {
  int count = 0;
  for (auto &p : peers) {
    if (p.used && !p.effectiveDropped && !p.dismissed) {
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
      out.fallingBack = p.effectiveWeak;
      out.droppedOff = p.effectiveDropped;
      out.lastSeenMs = p.lastSeenMs;
      return true;
    }
    i++;
  }
  return false;
}

bool anyDroppedOff() {
  for (auto &p : peers) {
    if (p.used && p.effectiveDropped && !p.dismissed) {
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
    if (p.effectiveDropped && (bestIndex < 0 || silentMs > bestSilentMs)) {
      bestIndex = i;
      bestSilentMs = silentMs;
    }
    i++;
  }
  return bestIndex >= 0 && peerInfo(bestIndex, out);
}

bool dismiss(uint16_t nodeId) {
  for (auto &p : peers) {
    // The effectiveDropped check makes this a no-op if the rider's heartbeat
    // came back (or the group stopped agreeing they were lost) between
    // showing the prompt and the long press -- never silently hide someone
    // who is actually present.
    if (p.used && p.nodeId == nodeId && p.effectiveDropped) {
      p.dismissed = true;
      return true;
    }
  }
  return false;
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
