// Tracks the other riders' devices heard this outing: presence (via
// heartbeats) and a smoothed RSSI trend, which is how "falling back" /
// "dropped off" is detected automatically -- no GPS needed. From 3 devices
// on, that local per-device verdict is only ever an *opinion*: it is
// corroborated with the rest of the group over a lightweight gossip mesh
// (see coop.{h,cpp}) before it becomes the alert exposed here -- see
// Roster::tick() for the consensus gating, and Coop::Consensus for what
// happens below 3 devices or when a peer has too few potential observers.
#pragma once

#include <Arduino.h>

#include "protocol.h"

namespace Roster {

// Which of the two "worth interrupting the rider for" events lastAlertTimestampMs()
// refers to -- lets the UI pick a distinct beep/notification per event type.
enum class AlertType { FallingBack, DroppedOff };

// Snapshot of one tracked peer, for the rider-list and range-test screens.
struct PeerInfo {
  uint16_t nodeId; // for Roster::dismiss()
  char nickname[Protocol::kNicknameFieldLen + 1];
  int16_t rssiDbm; // fast-smoothed (EMA), the value this device's own local verdict judges
  int16_t rssiSlowDbm; // slow-smoothed (EMA) -- "where the signal usually sits", the trend baseline
  int16_t lastRawRssiDbm; // unsmoothed RSSI of the most recent heartbeat
  uint16_t batteryMillivolts; // 0 if the peer has no battery monitor
  // Group-consensus-gated, not purely local: e.g. droppedOff can be false
  // here even while this device hasn't heard the peer in a while, if the
  // rest of the group still hears them (see Roster::tick()).
  bool fallingBack;
  bool droppedOff;
  // This device's own, purely local falling-back opinion (see Peer::localWeak
  // in roster.cpp), *before* group-consensus gating -- distinct from
  // `fallingBack` above. Exposed for the range-test screen, which is
  // specifically the field-calibration tool and wants to show what this
  // device itself is seeing, not the (possibly not-yet-corroborated) group
  // verdict.
  bool localFallingBack;
  uint32_t lastSeenMs;
};

void begin();

// Feed every received HEARTBEAT here. Creates the peer on first sight,
// updates last-seen + RSSI trend otherwise, and detects/logs a recovery if
// the peer had previously been marked dropped off.
void onHeartbeat(uint16_t nodeId, const char *nickname, uint16_t batteryMillivolts, int16_t rssi);

// Call roughly once a second from the main loop. Detects peers that have
// gone silent for too long and marks/logs them as dropped off.
void tick(uint32_t nowMs);

// Peers seen this outing that have *not* gone silent for longer than
// DROPPED_OFF_TIMEOUT_MS.
int activeCount();

// All peers seen this outing, including currently dropped-off ones.
int totalCount();

// Fills `out` with the index-th tracked peer (0 <= index < totalCount(),
// unsorted slot order -- display ordering is the UI's job). Returns false
// once index runs past the end.
bool peerInfo(int index, PeerInfo &out);

// True if at least one (non-dismissed) peer is currently dropped off --
// drives the periodic drop-off reminder in the UI.
bool anyDroppedOff();

// Fills `out` with the longest-gone (oldest lastSeenMs) non-dismissed
// dropped-off peer; false if there is none. The UI's removal-prompt candidate.
bool longestDropped(PeerInfo &out);

// Removes a dropped-off rider from the group: the peer stops
// counting/showing/reminding, but its slot stays -- a heartbeat from the
// same node re-adds it automatically (logged as "back"). Returns false (and
// changes nothing) if the peer meanwhile came back on its own -- the UI
// words its toast accordingly.
bool dismiss(uint16_t nodeId);

// Looks up a peer's nickname for display; returns a fallback "#xxxx" string
// (from a static buffer, valid until the next call) if the sender hasn't
// been heard from via a heartbeat yet.
const char *nicknameFor(uint16_t nodeId);

// Most recent roster-level event (join / falling back / dropped off / back),
// for the idle screen's "last event" line. Returns nullptr if none yet.
const char *lastEventText();
uint32_t lastEventTimestampMs();

// Timestamp of the most recent "falling back"/"dropped off" event
// specifically (a subset of the above -- joins/returns don't count). Used by
// the UI to wake the display for the two events actually worth interrupting
// the rider for. 0 if none yet.
uint32_t lastAlertTimestampMs();

// Bumped on every "falling back"/"dropped off" event. The UI's dedup key:
// unlike the timestamp above it also distinguishes two alerts logged in the
// very same millisecond (e.g. two peers dropping in one tick()). 0 = no
// alert yet.
uint32_t lastAlertGeneration();

// Which event lastAlertTimestampMs() refers to. Only meaningful once
// lastAlertTimestampMs() is non-zero.
AlertType lastAlertType();

// The self-calibrated "group baseline" RSSI this device is currently using to
// derive the falling-back floor (median slow-EMA RSSI across the intact
// group, smoothed -- see Calibration::medianDbm()/smoothBaseline() in
// calibration.{h,cpp} and Roster::tick()). Only meaningful when
// baselineEstablished() is true; returns RSSI_FALLING_BACK_FLOOR_DBM
// verbatim otherwise (matching the fixed-floor fallback, so callers that
// don't check baselineEstablished() still show something sane).
int16_t groupBaselineDbm();

// True once enough peers are present to trust groupBaselineDbm() (see
// CAL_MIN_PEERS_FOR_BASELINE in config.h) -- false right after boot or when
// riding alone, in which case falling-back detection uses the fixed fallback
// floor instead.
bool baselineEstablished();

// The floor currently in effect for a given peer's falling-back check -- the
// same value onHeartbeat() itself computes and judges rssiDbm against; for
// the range-test screen so it can show exactly what's being compared.
int16_t activeFloorDbm();

} // namespace Roster
