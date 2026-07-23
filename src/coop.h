// Cooperative drop-off confirmation: a lightweight, anomaly-only gossip mesh
// that lets a device's local WEAK/LOST opinion about a peer be corroborated
// (or refuted) by the rest of the group before it becomes a rider-facing
// alert -- see Roster::tick(), which is the only caller that turns this
// module's verdict into an actual beep/screen update.
//
// Only anomalies travel on the wire (Protocol::GossipEntry, MsgType::Gossip):
// an all-fine group generates no gossip traffic at all. Each device owns and
// versions its own opinions (see setOwnOpinion()) and relays anyone else's
// anomalies it hears, decrementing a hop budget (TTL) -- a small controlled
// flood, not a routing protocol. This lets a rider at the front learn about
// a rider at the back falling behind even if they can no longer hear each
// other directly, as long as *some* chain of intermediate riders can.
//
// Pure state/logic module, no Arduino.h, so it runs unchanged in the native
// unit-test environment (pio test -e native) just like protocol.cpp -- every
// timestamp is passed in explicitly rather than read from millis().
#pragma once

#include "protocol.h"

namespace Coop {

// Tunables live here, not in config.h: this module is deliberately
// Arduino-free (see the file comment above) so it runs unchanged in the
// native unit-test environment, and config.h pulls in Arduino.h.

// A device's own WEAK/LOST opinion about a peer no longer alerts the rider
// by itself -- see Roster::tick(). It only does so once the group agrees.
// Below this many total riders (this device + tracked peers) there aren't
// enough independent vantage points for a meaningful consensus, so behaviour
// falls back to the pre-existing, purely local verdict.
constexpr int kMinGroup = 3;

// How often an active (WEAK/LOST) own opinion is re-broadcast while it
// lasts, so the mesh re-converges after a lost packet or a topology change
// (a relay rider drifting out of the chain). Independent of
// HEARTBEAT_INTERVAL_MS -- gossip is event-driven (fires immediately on a
// change), this is only the "keep nagging while unresolved" cadence.
// Trade-off: shorter converges faster but spends more of the shared
// duty-cycle budget while a problem is ongoing -- Radio::send() already
// treats gossip like heartbeats/warnings (best-effort, silently skipped when
// the budget is tight), so a busy incident just means some refreshes are
// dropped, not a hard failure.
constexpr uint32_t kRefreshMs = 2500;
// Small random delay before actually transmitting a due gossip packet, so
// several devices reacting to the same event (e.g. everyone losing the same
// rider at once) don't all key up in the same instant.
constexpr uint32_t kSendJitterMs = 300;
// An observer's report only counts toward consensus while newer than this --
// comfortably more than one kRefreshMs cycle plus relay/jitter latency, so a
// single missed packet doesn't drop someone's vote. The anomaly-only design
// has no explicit "back to OK" message, so an entry aging out past this *is*
// the retraction (see tick()).
constexpr uint32_t kEntryTtlMs = 8000;
// Hop budget for the controlled flood: each relay decrements this by one and
// stops forwarding at 0. Bounds propagation to a reasonably-sized group
// without needing real routing/addressing.
constexpr uint8_t kTtlHops = 3;
// The wire format packs ttl into a 4-bit nibble alongside the 1-bit status
// (see protocol.cpp's encodeGossip()/decode()) -- a ttl above this would be
// silently truncated on the wire instead of failing loudly, so pin the
// invariant here where kTtlHops is actually chosen.
static_assert(kTtlHops <= 0x0F, "ttl must fit the wire format's 4-bit nibble (see protocol.cpp's status/ttl packing)");
// Floor on how often this device actually keys up for gossip, regardless of
// how much is pending. A maximal ~37-byte gossip packet costs ~18.3ms of
// airtime, and the shared EU868 1% duty-cycle budget only refills at
// ~10us/ms -- so one such packet needs ~1.83s to fully replenish. Without a
// floor, a busy table (many simultaneous anomalies) can re-arm a send on
// almost every loop() iteration once the jitter window elapses, competing
// gossip against heartbeats for the same budget far faster than the budget
// can sustain. 1s is a deliberately simple, conservative middle ground: it
// meaningfully caps worst-case contention without perceptibly slowing down
// normal (few-anomaly) mesh convergence, which is already paced by
// kRefreshMs above.
constexpr uint32_t kMinSendGapMs = 1000;
// Bounded gossip table: one row per (observer, subject) pair with a
// currently-tracked opinion. Generous for a club-ride-sized group;
// table-full falls back to evicting the globally stalest row (same "bounded
// and reused" approach as Roster's own peer table, see findOrCreate() there).
constexpr int kMaxEntries = 40;
// Percentage of the group (this device + tracked peers) that must
// independently report LOST / WEAK-or-worse about the same peer before the
// group "agrees" -- see subjectConsensus(). Both are floored at 2 distinct
// observers regardless of group size: a single device can never trigger an
// alert on its own, which is the whole point of gating on consensus.
constexpr int kLostQuorumPct = 60;
constexpr int kWeakQuorumPct = 40;

enum class Consensus {
  // Fewer than kMinGroup total riders (this device + tracked peers), or
  // this is simply too small a group for any subject to ever gather a
  // 2-observer quorum -- not enough independent vantage points to corroborate
  // anything. Callers should fall back to their own local RSSI/heartbeat
  // verdict, unchanged from before this feature existed.
  InsufficientObservers,
  Ok, // enough observers, but neither the WEAK nor LOST quorum is met
  Weak, // WEAK-or-worse quorum met -- group-confirmed "falling back"
  Lost, // LOST quorum met -- group-confirmed "dropped off"
};

// Clears the table. Call once from setup().
void begin();

// Records this device's own current opinion of `subjectId` (`ownId` is this
// device's node id, so our own rows are recognizable and never relayed back
// to us). `active=false` retracts a previously-active opinion (the subject
// returned to OK from this device's point of view) -- `status` is then
// ignored. Arms the entry for prompt (jittered) transmission whenever the
// opinion actually changed since the last call; a no-op call (same status,
// still active) does not re-arm a send by itself (see tick() for the
// periodic refresh while unchanged). Does NOT bump the sequence number
// itself -- that happens once, in collectOutgoing(), at the moment an
// opinion is actually (re)transmitted (see the note there for why).
void setOwnOpinion(uint16_t ownId, uint16_t subjectId, Protocol::GossipStatus status, bool active,
                    uint32_t nowMs);

// Merges one received/relayed gossip entry into the table. Ignores entries
// about `ownId` itself (we are authoritative for our own opinions -- this is
// what stops a relayed copy of our own report from bouncing back to us).
// Freshness is decided purely by the sequence number, regardless of whether
// `status` matches the stored entry: a stale or duplicate (non-newer) `seq`
// for an already-known (observerId, subjectId) is dropped silently -- this
// is the flood's loop/dup breaker. (Comparing status too, before checking
// seq, would let an out-of-order OLDER report with a DIFFERENT status
// silently overwrite a newer one -- e.g. a late "Weak" arriving after an
// already-stored newer "Lost" downgrading it back.) A genuinely newer report
// is stored and, if its ttl allows, armed for one further relay hop.
void mergeEntry(uint16_t ownId, const Protocol::GossipEntry &entry, uint32_t nowMs);

// Housekeeping: frees any entry that has gone stale (older than kEntryTtlMs
// -- this *is* the anomaly-only design's implicit retraction) and arms the
// periodic refresh for this device's own still-active opinions. Call once a
// second, alongside Roster::tick().
void tick(uint16_t ownId, uint32_t nowMs);

// True once at least one own-opinion (re)send or pending relay is waiting to
// go out AND at least kMinSendGapMs has passed since the last actual send --
// main.cpp gates/jitters the actual radio transmit on this, so an all-OK
// group's mesh stays silent on air, and a busy one can't key up faster than
// the floor allows.
bool sendDue(uint32_t nowMs);

// Fills `out` (capacity `capacity`) with the next batch to transmit: due own
// opinions first, then pending relays (already decremented), up to capacity.
// Bumps each collected own entry's sequence number and renews its
// `lastUpdateMs` -- this is the single place that treats "we are actually
// putting this on the air right now" as a new generation, whether the send
// was triggered by a genuine change (setOwnOpinion) or a periodic refresh
// (tick()). Without this, a refreshed-but-unchanged own opinion would look
// identical to observers as the previous transmission and never renew their
// copy's freshness either -- see the kEntryTtlMs comment. Returns the count
// written and clears the collected entries' pending flags -- a failed
// Radio::send() afterwards is not specially retried; the own-opinion
// refresh cadence and/or a later relay of the same report will naturally
// re-surface it, same "best-effort" tolerance as heartbeats.
uint8_t collectOutgoing(Protocol::GossipEntry *out, uint8_t capacity, uint32_t nowMs);

// Group consensus about `subjectId`, given `groupSize` (this device + every
// currently-tracked peer, i.e. Roster::totalCount() + 1).
Consensus subjectConsensus(int groupSize, uint16_t subjectId, uint32_t nowMs);

// The four Consensus cases mapped onto the two rider-facing flags
// (Roster::PeerInfo's fallingBack/droppedOff). Pure function -- pulled out
// of Roster::tick() so this safety-relevant mapping gets native test
// coverage like the rest of this module.
struct EffectiveState {
  bool dropped;
  bool weak;
};
EffectiveState resolveVerdict(Consensus consensus, bool localLost, bool localWeak);

// Rising-edge alert decision from a peer's previous vs. current effective
// state. A Lost->Weak transition (the peer recovering, not worsening) must
// NOT fire the falling-back alert -- that's what `!wasDropped` guards
// against in the Weak case below.
struct AlertDecision {
  bool fireDropped;
  bool fireWeak;
};
AlertDecision decideAlert(bool wasDropped, bool isDropped, bool wasWeak, bool isWeak);

} // namespace Coop
