#include "coop.h"

namespace Coop {

namespace {
struct Entry {
  bool used = false;
  uint16_t observerId = 0;
  uint16_t subjectId = 0;
  Protocol::GossipStatus status = Protocol::GossipStatus::Weak;
  uint8_t seq = 0;
  uint8_t ttl = 0; // hops remaining if/when (re)forwarded
  uint32_t lastUpdateMs = 0; // stamped on creation, own refresh, or a merged newer report
  bool relayPending = false; // foreign entry: needs forwarding on the next send
  bool ownSendPending = false; // own entry: needs (re)transmission on the next send
  uint32_t nextOwnRefreshMs = 0; // own entry: when to refresh next while active
};

Entry table[kMaxEntries];

// Rate-limit state for sendDue()/collectOutgoing() -- see kMinSendGapMs.
// `hasSent` gates whether the floor even applies yet: at boot (or right
// after begin()) nothing has been sent, so there is nothing to rate-limit
// against -- without this, lastActualSendMs's default of 0 would wrongly
// block any send attempted within the first kMinSendGapMs of millis().
bool hasSent = false;
uint32_t lastActualSendMs = 0;

// Wrap-aware "is sequence `a` newer than `b`" for the 8-bit per-(observer,
// subject) counter -- same idiom as the millis()-wraparound comparisons used
// throughout main.cpp/ui.cpp, just at 8 bits instead of 32. Only meaningful
// for gaps under 128; a per-entry counter bumped every few seconds and aged
// out well before then (kEntryTtlMs) never gets close.
bool seqIsNewer(uint8_t a, uint8_t b) {
  return static_cast<int8_t>(static_cast<uint8_t>(a - b)) > 0;
}

Entry *find(uint16_t observerId, uint16_t subjectId) {
  for (auto &e : table) {
    if (e.used && e.observerId == observerId && e.subjectId == subjectId) {
      return &e;
    }
  }
  return nullptr;
}

// Finds the row for (observerId, subjectId), creating one if needed. The
// table is a bounded resource: once full, the globally least-recently-
// updated row is evicted to make room -- same "bounded and reused" approach
// as Roster's own peer table (see findOrCreate() there).
Entry *findOrMakeRoom(uint16_t observerId, uint16_t subjectId, uint32_t nowMs) {
  Entry *existing = find(observerId, subjectId);
  if (existing != nullptr) {
    return existing;
  }
  Entry *chosen = nullptr;
  uint32_t chosenAgeMs = 0;
  for (auto &e : table) {
    if (!e.used) {
      chosen = &e;
      break;
    }
    uint32_t ageMs = nowMs - e.lastUpdateMs; // wraparound-safe unsigned subtraction
    if (chosen == nullptr || ageMs > chosenAgeMs) {
      chosen = &e;
      chosenAgeMs = ageMs;
    }
  }
  *chosen = Entry{};
  return chosen;
}

// max(2, ceil(groupSize * pct / 100)) -- the floor of 2 is what guarantees a
// single device can never trigger a consensus alert on its own.
int quorumFor(int groupSize, int pct) {
  int q = (groupSize * pct + 99) / 100;
  return q < 2 ? 2 : q;
}
} // namespace

void begin() {
  for (auto &e : table) {
    e = Entry{};
  }
  hasSent = false;
  lastActualSendMs = 0;
}

void setOwnOpinion(uint16_t ownId, uint16_t subjectId, Protocol::GossipStatus status, bool active,
                    uint32_t nowMs) {
  Entry *existing = find(ownId, subjectId);
  if (!active) {
    // Retract outright: unlike a foreign row, we don't need to remember "we
    // once had an opinion" here -- this device is trivially always a
    // potential observer of its own tracked peers regardless of table state.
    if (existing != nullptr) {
      *existing = Entry{};
    }
    return;
  }

  bool changed = existing == nullptr || existing->status != status;
  Entry *e = existing != nullptr ? existing : findOrMakeRoom(ownId, subjectId, nowMs);
  if (changed) {
    e->ownSendPending = true; // event-driven: send promptly on an actual change
  }
  // seq is bumped once, in collectOutgoing(), at the moment this opinion is
  // actually (re)transmitted -- not here. Bumping it here would only cover
  // the "genuine change" case; a later periodic refresh of an *unchanged*
  // opinion needs a fresh seq too (see collectOutgoing()'s comment), so a
  // single spot that bumps on every real transmission is both simpler and
  // correct for both cases.
  e->used = true;
  e->observerId = ownId;
  e->subjectId = subjectId;
  e->status = status;
  e->ttl = kTtlHops;
  e->lastUpdateMs = nowMs;
  e->nextOwnRefreshMs = nowMs + kRefreshMs;
}

void mergeEntry(uint16_t ownId, const Protocol::GossipEntry &entry, uint32_t nowMs) {
  if (entry.observerId == ownId) {
    return; // we are authoritative for our own opinions -- ignore a relayed echo of ourselves
  }
  Entry *existing = find(entry.observerId, entry.subjectId);
  // Freshness is decided purely by seq, regardless of whether status
  // matches: comparing status first (and only checking seq when it's
  // unchanged) would let an out-of-order OLDER report with a DIFFERENT
  // status silently overwrite a newer one -- e.g. a late "Weak" arriving
  // after an already-stored newer "Lost", downgrading it back.
  if (existing != nullptr && !seqIsNewer(entry.seq, existing->seq)) {
    // Also the (accepted, self-healing) cost of a rebooted observer: its seq
    // restarts low, so its first post-reboot reports are rejected here as
    // "not newer" until our stale copy ages out on its own via kEntryTtlMs
    // (at most ~8s) -- not worth extra reboot-detection machinery for a
    // bounded, rare, self-recovering gap.
    return; // stale/duplicate copy of what we already have -- the flood's loop/dup breaker
  }
  Entry *e = existing != nullptr ? existing : findOrMakeRoom(entry.observerId, entry.subjectId, nowMs);
  e->used = true;
  e->observerId = entry.observerId;
  e->subjectId = entry.subjectId;
  e->status = entry.status;
  e->seq = entry.seq;
  e->lastUpdateMs = nowMs;
  if (entry.ttl > 0) {
    e->ttl = static_cast<uint8_t>(entry.ttl - 1);
    e->relayPending = true;
  } else {
    e->ttl = 0;
    e->relayPending = false;
  }
}

void tick(uint16_t ownId, uint32_t nowMs) {
  for (auto &e : table) {
    if (!e.used) {
      continue;
    }
    if (nowMs - e.lastUpdateMs > kEntryTtlMs) {
      // The anomaly-only design has no explicit "back to OK" message -- an
      // entry aging out past its TTL *is* the retraction, for both our own
      // rows (shouldn't normally happen; setOwnOpinion(active=false) already
      // retracts promptly) and foreign ones (the observer stopped
      // refreshing, meaning their own opinion lapsed or they went silent).
      e = Entry{};
      continue;
    }
    if (e.observerId == ownId && !e.ownSendPending &&
        static_cast<int32_t>(nowMs - e.nextOwnRefreshMs) >= 0) {
      e.ownSendPending = true; // periodic refresh due while the opinion stays active
    }
  }
}

bool sendDue(uint32_t nowMs) {
  // Rate-limit floor: not applied until the very first actual send has ever
  // happened (see hasSent's comment above).
  if (hasSent && (nowMs - lastActualSendMs < kMinSendGapMs)) {
    return false;
  }
  for (auto &e : table) {
    if (e.used && (e.ownSendPending || e.relayPending)) {
      return true;
    }
  }
  return false;
}

uint8_t collectOutgoing(Protocol::GossipEntry *out, uint8_t capacity, uint32_t nowMs) {
  uint8_t count = 0;
  // Own active opinions first: they're the reason a chain of relays exists at
  // all, and they must keep refreshing (see nextOwnRefreshMs) so the group
  // doesn't lose track of a still-ongoing problem.
  for (auto &e : table) {
    if (count >= capacity) {
      break;
    }
    if (!e.used || !e.ownSendPending) {
      continue;
    }
    // This is the one place that treats "we are actually putting this on
    // the air right now" as a new generation, whether the send was
    // triggered by a genuine change or a periodic refresh of an unchanged
    // opinion -- bumping seq only in setOwnOpinion() would miss the refresh
    // case, leaving a still-ongoing opinion looking stale to observers (and
    // to our own tick()'s TTL check) despite being actively resent.
    e.seq = static_cast<uint8_t>(e.seq + 1);
    e.lastUpdateMs = nowMs;
    out[count].observerId = e.observerId;
    out[count].subjectId = e.subjectId;
    out[count].status = e.status;
    out[count].ttl = e.ttl;
    out[count].seq = e.seq;
    count++;
    e.ownSendPending = false;
    e.nextOwnRefreshMs = nowMs + kRefreshMs;
  }
  for (auto &e : table) {
    if (count >= capacity) {
      break;
    }
    if (!e.used || !e.relayPending) {
      continue;
    }
    out[count].observerId = e.observerId;
    out[count].subjectId = e.subjectId;
    out[count].status = e.status;
    out[count].ttl = e.ttl;
    out[count].seq = e.seq;
    count++;
    e.relayPending = false;
  }
  if (count > 0) {
    hasSent = true;
    lastActualSendMs = nowMs;
  }
  return count;
}

Consensus subjectConsensus(int groupSize, uint16_t subjectId, uint32_t nowMs) {
  if (groupSize < kMinGroup) {
    return Consensus::InsufficientObservers;
  }

  int lostCount = 0;
  int weakOrWorseCount = 0;
  for (auto &e : table) {
    if (!e.used || e.subjectId != subjectId) {
      continue;
    }
    // tick() already frees anything past kEntryTtlMs every second, but guard
    // here too in case this is called between tick()s.
    if (nowMs - e.lastUpdateMs > kEntryTtlMs) {
      continue;
    }
    weakOrWorseCount++;
    if (e.status == Protocol::GossipStatus::Lost) {
      lostCount++;
    }
  }

  if (lostCount >= quorumFor(groupSize, kLostQuorumPct)) {
    return Consensus::Lost;
  }
  if (weakOrWorseCount >= quorumFor(groupSize, kWeakQuorumPct)) {
    return Consensus::Weak;
  }
  return Consensus::Ok;
}

EffectiveState resolveVerdict(Consensus consensus, bool localLost, bool localWeak) {
  switch (consensus) {
    case Consensus::InsufficientObservers:
      // Not enough of the group can possibly corroborate this peer yet --
      // fall back to this device's own local verdict, unchanged from before
      // cooperative confirmation existed.
      return {localLost, !localLost && localWeak};
    case Consensus::Ok:
      // The group does not agree this peer is in trouble -- even if *we*
      // locally lost them, that reads as a local range/positioning issue,
      // not a real detachment. Softened, not hidden: there is currently no
      // separate UI signal for this quieter case (see roster.cpp).
      return {false, false};
    case Consensus::Weak:
      return {false, true};
    case Consensus::Lost:
      return {true, false}; // dropped supersedes falling-back
  }
  return {false, false}; // unreachable -- silences -Wreturn-type
}

AlertDecision decideAlert(bool wasDropped, bool isDropped, bool wasWeak, bool isWeak) {
  // Rising edge only -- consensus can flip back and forth as reporters come
  // and go, so "still true" must not re-alert on every call.
  if (isDropped && !wasDropped) {
    return {true, false};
  }
  // A Lost->Weak transition means the peer is recovering, not newly
  // faltering -- `!wasDropped` stops that improvement from firing a
  // falling-back alert. An Ok->Weak transition still fires normally.
  if (isWeak && !wasWeak && !wasDropped) {
    return {false, true};
  }
  return {false, false};
}

} // namespace Coop
