// Native unit tests for the cooperative drop-off confirmation mesh
// (src/coop.cpp): opinion tracking, merge/dedup/relay of gossip entries, TTL
// expiry, the consensus quorum math, the verdict/alert decision helpers, and
// the min-send-gap throttle. Run with: pio test -e native
#include <unity.h>

#include "coop.h"

namespace {
constexpr uint16_t kOwnId = 1;
} // namespace

void setUp() {
  Coop::begin(); // fresh table (and send-gap state) for every test -- coop.cpp's state is file-scope static
}
void tearDown() {}

void test_set_own_opinion_arms_send_and_appears_in_collect() {
  Coop::setOwnOpinion(kOwnId, /*subjectId=*/2, Protocol::GossipStatus::Weak, /*active=*/true, /*nowMs=*/1000);
  TEST_ASSERT_TRUE(Coop::sendDue(1000)); // hasSent is still false here -- gap floor doesn't apply yet

  Protocol::GossipEntry out[8];
  uint8_t count = Coop::collectOutgoing(out, 8, 1000);
  TEST_ASSERT_EQUAL_UINT8(1, count);
  TEST_ASSERT_EQUAL_HEX16(kOwnId, out[0].observerId);
  TEST_ASSERT_EQUAL_HEX16(2, out[0].subjectId);
  TEST_ASSERT_EQUAL(static_cast<int>(Protocol::GossipStatus::Weak), static_cast<int>(out[0].status));
  TEST_ASSERT_EQUAL_UINT8(Coop::kTtlHops, out[0].ttl);
  TEST_ASSERT_EQUAL_UINT8(1, out[0].seq); // bumped 0->1 on the first-ever (re)transmission

  TEST_ASSERT_FALSE(Coop::sendDue(1000)); // consumed -- nothing left pending
}

void test_repeated_same_opinion_does_not_rearm_send() {
  Coop::setOwnOpinion(kOwnId, 2, Protocol::GossipStatus::Weak, true, 1000);
  Protocol::GossipEntry out[8];
  Coop::collectOutgoing(out, 8, 1000); // drain the initial event

  Coop::setOwnOpinion(kOwnId, 2, Protocol::GossipStatus::Weak, true, 1001); // no actual change
  // Checked past the min-send-gap floor so this purely reflects "no rearm
  // happened", not the (separately tested) gap throttle.
  TEST_ASSERT_FALSE(Coop::sendDue(1000 + Coop::kMinSendGapMs + 1));
}

void test_own_opinion_retract_frees_row() {
  Coop::setOwnOpinion(kOwnId, 2, Protocol::GossipStatus::Lost, true, 1000);
  Protocol::GossipEntry out[8];
  Coop::collectOutgoing(out, 8, 1000);

  Coop::setOwnOpinion(kOwnId, 2, Protocol::GossipStatus::Lost, /*active=*/false, 2000);
  TEST_ASSERT_FALSE(Coop::sendDue(2000)); // gap satisfied (2000-1000 == kMinSendGapMs)
  // Retracted -- with nobody else corroborating, and too small a group to
  // matter anyway here, there is nothing left to say about subject 2.
  TEST_ASSERT_EQUAL_UINT8(0, Coop::collectOutgoing(out, 8, 2000));
}

void test_merge_ignores_own_observer_id() {
  Protocol::GossipEntry echoOfSelf{kOwnId, 2, Protocol::GossipStatus::Weak, 3, 9};
  Coop::mergeEntry(kOwnId, echoOfSelf, 1000);
  TEST_ASSERT_FALSE(Coop::sendDue(1000)); // never stored -- we are authoritative for our own opinions
}

void test_merge_relay_decrements_ttl_and_stops_at_zero() {
  Protocol::GossipEntry viaOneHop{5, 6, Protocol::GossipStatus::Weak, 1, 10};
  Coop::mergeEntry(kOwnId, viaOneHop, 1000);
  TEST_ASSERT_TRUE(Coop::sendDue(1000)); // hasSent still false -- gap not armed yet
  Protocol::GossipEntry out[8];
  uint8_t count = Coop::collectOutgoing(out, 8, 1000);
  TEST_ASSERT_EQUAL_UINT8(1, count);
  TEST_ASSERT_EQUAL_UINT8(0, out[0].ttl); // decremented from 1 -> 0

  Protocol::GossipEntry atBudgetLimit{7, 8, Protocol::GossipStatus::Lost, 0, 20};
  Coop::mergeEntry(kOwnId, atBudgetLimit, 1000);
  // ttl already 0 -- stored, but not relayed further. Checked past the gap
  // (armed by the collect above) so this purely reflects "nothing to relay".
  TEST_ASSERT_FALSE(Coop::sendDue(1000 + Coop::kMinSendGapMs));
}

void test_merge_dedup_drops_stale_and_duplicate_seq() {
  Protocol::GossipEntry first{5, 6, Protocol::GossipStatus::Weak, 3, 10};
  Coop::mergeEntry(kOwnId, first, 1000);
  Protocol::GossipEntry out[8];
  Coop::collectOutgoing(out, 8, 1000); // drain the relay -- arms the min-send-gap floor from t=1000

  // Same seq again (duplicate copy via a different relay path) -- dropped.
  // Checked past the gap so each assertion reflects dedup, not the gap.
  Coop::mergeEntry(kOwnId, first, 1001);
  TEST_ASSERT_FALSE(Coop::sendDue(2000));

  // Older seq -- dropped.
  Protocol::GossipEntry older{5, 6, Protocol::GossipStatus::Weak, 3, 9};
  Coop::mergeEntry(kOwnId, older, 1002);
  TEST_ASSERT_FALSE(Coop::sendDue(2000));

  // Genuinely newer seq -- accepted and re-armed for relay.
  Protocol::GossipEntry newer{5, 6, Protocol::GossipStatus::Weak, 3, 11};
  Coop::mergeEntry(kOwnId, newer, 1003);
  TEST_ASSERT_TRUE(Coop::sendDue(2000));
}

// Regression test for Finding 2: a stale, out-of-order report with a
// DIFFERENT status than what's stored must not bypass the seq freshness
// check -- only a genuinely newer seq may change the stored status.
void test_merge_rejects_older_seq_even_with_different_status() {
  Protocol::GossipEntry lost{5, 6, Protocol::GossipStatus::Lost, 2, 6};
  Coop::mergeEntry(kOwnId, lost, 1000);
  Protocol::GossipEntry out[8];
  Coop::collectOutgoing(out, 8, 1000); // drain, arm the gap

  // An older report (seq 5 < 6) claiming Weak -- must be rejected despite
  // the differing status.
  Protocol::GossipEntry staleWeak{5, 6, Protocol::GossipStatus::Weak, 2, 5};
  Coop::mergeEntry(kOwnId, staleWeak, 2000);
  TEST_ASSERT_FALSE(Coop::sendDue(2000)); // rejected -- nothing new to relay

  // Confirm the stored status is still Lost, not overwritten to Weak: add a
  // second independent Lost observer and check the Lost quorum (2 at
  // groupSize=3) is reached by the ORIGINAL entry + this new one. If the
  // stale Weak had (bug) overwritten observer 5's row, this would only
  // reach the Weak quorum, not Lost.
  Protocol::GossipEntry secondObserver{7, 6, Protocol::GossipStatus::Lost, 2, 1};
  Coop::mergeEntry(kOwnId, secondObserver, 2000);
  TEST_ASSERT_EQUAL(static_cast<int>(Coop::Consensus::Lost),
                     static_cast<int>(Coop::subjectConsensus(3, 6, 2000)));
}

void test_merge_seq_comparison_handles_wraparound() {
  Protocol::GossipEntry atWrapBoundary{5, 6, Protocol::GossipStatus::Weak, 3, 254};
  Coop::mergeEntry(kOwnId, atWrapBoundary, 1000);
  Protocol::GossipEntry out[8];
  Coop::collectOutgoing(out, 8, 1000); // hasSent=true, arms the gap from t=1000

  // 1 comes after 254 once the 8-bit counter wraps (254 -> 255 -> 0 -> 1) --
  // must be treated as newer, not as a huge jump backward.
  Protocol::GossipEntry wrapped{5, 6, Protocol::GossipStatus::Weak, 3, 1};
  Coop::mergeEntry(kOwnId, wrapped, 2000);
  TEST_ASSERT_TRUE(Coop::sendDue(2000)); // gap satisfied (2000-1000 == kMinSendGapMs)
  Coop::collectOutgoing(out, 8, 2000); // re-arms the gap from t=2000

  // From 1, a report claiming 200 would mean wrapping almost all the way
  // around again -- within the entry TTL that never legitimately happens, so
  // it must be rejected as stale/out-of-order, not accepted as newer.
  Protocol::GossipEntry farBehind{5, 6, Protocol::GossipStatus::Weak, 3, 200};
  Coop::mergeEntry(kOwnId, farBehind, 3000);
  TEST_ASSERT_FALSE(Coop::sendDue(3000)); // gap satisfied (3000-2000 == kMinSendGapMs)
}

void test_entry_expires_and_row_is_freed() {
  Protocol::GossipEntry entry{5, 6, Protocol::GossipStatus::Weak, 2, 10};
  Coop::mergeEntry(kOwnId, entry, 1000);
  Protocol::GossipEntry out[8];
  Coop::collectOutgoing(out, 8, 1000);

  // Long past the TTL: tick() must free the row (the anomaly-only design's
  // implicit "back to OK" -- see coop.h).
  Coop::tick(kOwnId, 1000 + Coop::kEntryTtlMs + 1);

  // If the row still existed, a *lower* seq than what was stored (10) would
  // be rejected as stale. Since it was freed, this is treated as a brand new
  // report and accepted.
  Protocol::GossipEntry lowerSeq{5, 6, Protocol::GossipStatus::Weak, 2, 3};
  Coop::mergeEntry(kOwnId, lowerSeq, 1000 + Coop::kEntryTtlMs + 2);
  TEST_ASSERT_TRUE(Coop::sendDue(1000 + Coop::kEntryTtlMs + 2)); // gap long satisfied (>8s elapsed)
}

void test_own_opinion_refreshes_on_schedule() {
  Coop::setOwnOpinion(kOwnId, 2, Protocol::GossipStatus::Weak, true, 0);
  Protocol::GossipEntry out[8];
  Coop::collectOutgoing(out, 8, 0);
  TEST_ASSERT_EQUAL_UINT8(1, out[0].seq);

  Coop::tick(kOwnId, Coop::kRefreshMs - 1);
  TEST_ASSERT_FALSE(Coop::sendDue(Coop::kRefreshMs - 1)); // not due yet (refresh not armed)

  Coop::tick(kOwnId, Coop::kRefreshMs);
  TEST_ASSERT_TRUE(Coop::sendDue(Coop::kRefreshMs)); // refresh interval elapsed, gap long satisfied

  uint8_t count = Coop::collectOutgoing(out, 8, Coop::kRefreshMs);
  TEST_ASSERT_EQUAL_UINT8(1, count);
  // Fix 1 regression check: a refresh of an UNCHANGED opinion must still
  // bump seq (before the fix, this asserted the opposite -- "seq unchanged"
  // -- which was the bug: it meant a refresh looked identical to observers
  // and was silently deduped instead of renewing their copy's freshness).
  TEST_ASSERT_EQUAL_UINT8(2, out[0].seq);
}

// Regression test for Finding 1 (own-entry side): an active opinion that
// keeps getting refreshed on schedule must never expire, even long past the
// original kEntryTtlMs window from when it first appeared. Before the fix,
// collectOutgoing() never renewed lastUpdateMs, so tick() would free this
// row at t=8000 regardless of the refreshes below, and collectOutgoing
// would then permanently return 0.
void test_own_opinion_survives_repeated_refresh_past_old_ttl_window() {
  uint32_t now = 0;
  Coop::setOwnOpinion(kOwnId, 2, Protocol::GossipStatus::Lost, true, now);
  Protocol::GossipEntry out[8];
  uint8_t count = Coop::collectOutgoing(out, 8, now);
  TEST_ASSERT_EQUAL_UINT8(1, count);
  uint8_t prevSeq = out[0].seq;

  for (int cycle = 0; cycle < 5; cycle++) {
    now += Coop::kRefreshMs;
    Coop::tick(kOwnId, now);
    TEST_ASSERT_TRUE(Coop::sendDue(now));
    count = Coop::collectOutgoing(out, 8, now);
    TEST_ASSERT_EQUAL_UINT8(1, count); // still alive and being resent
    TEST_ASSERT_TRUE(out[0].seq != prevSeq); // each refresh is a fresh generation
    prevSeq = out[0].seq;
  }
  TEST_ASSERT_TRUE(now > Coop::kEntryTtlMs); // sanity: we really did go past the old TTL window
}

// Regression test for Finding 1 (receiver side): a foreign observer that
// keeps re-sending its opinion with a genuinely increasing seq (as a real
// device now does after the fix -- see collectOutgoing()) must have this
// device's copy of it renewed on each merge, surviving past the old
// kEntryTtlMs window.
void test_foreign_entry_survives_repeated_refresh_with_increasing_seq() {
  uint32_t now = 1000;
  Protocol::GossipEntry entry{5, 6, Protocol::GossipStatus::Lost, 2, 1};
  Coop::mergeEntry(kOwnId, entry, now);

  for (uint8_t seq = 2; seq <= 6; seq++) {
    now += Coop::kRefreshMs;
    entry.seq = seq;
    Coop::mergeEntry(kOwnId, entry, now);
  }
  TEST_ASSERT_TRUE(now - 1000 > Coop::kEntryTtlMs); // sanity: past the old TTL window from the first report

  // A second, independent observer confirms Lost right now -- if observer
  // 5's long-refreshed entry had (bug) expired, this would only be 1 fresh
  // vote (Ok at groupSize=3, quorum=2); since it's still alive, this is the
  // 2nd vote and consensus is reached.
  Protocol::GossipEntry second{7, 6, Protocol::GossipStatus::Lost, 2, 1};
  Coop::mergeEntry(kOwnId, second, now);
  TEST_ASSERT_EQUAL(static_cast<int>(Coop::Consensus::Lost),
                     static_cast<int>(Coop::subjectConsensus(3, 6, now)));
}

void test_collect_outgoing_respects_capacity() {
  Coop::setOwnOpinion(kOwnId, 2, Protocol::GossipStatus::Weak, true, 1000);
  Coop::setOwnOpinion(kOwnId, 3, Protocol::GossipStatus::Lost, true, 1000);

  Protocol::GossipEntry out[8];
  TEST_ASSERT_EQUAL_UINT8(1, Coop::collectOutgoing(out, 1, 1000));
  TEST_ASSERT_TRUE(Coop::sendDue(1000 + Coop::kMinSendGapMs)); // one entry still pending, past the gap
  TEST_ASSERT_EQUAL_UINT8(1, Coop::collectOutgoing(out, 8, 1000 + Coop::kMinSendGapMs));
  TEST_ASSERT_FALSE(Coop::sendDue(1000 + Coop::kMinSendGapMs));
}

void test_collect_outgoing_capacity_zero_writes_nothing() {
  Coop::setOwnOpinion(kOwnId, 2, Protocol::GossipStatus::Weak, true, 1000);
  Protocol::GossipEntry out[1];
  TEST_ASSERT_EQUAL_UINT8(0, Coop::collectOutgoing(out, 0, 1000));
  TEST_ASSERT_TRUE(Coop::sendDue(1000)); // still pending -- nothing was actually consumed
}

void test_collect_outgoing_mixes_own_and_relay_own_first() {
  Coop::setOwnOpinion(kOwnId, 2, Protocol::GossipStatus::Weak, true, 1000);
  Protocol::GossipEntry relay{5, 6, Protocol::GossipStatus::Lost, 2, 1};
  Coop::mergeEntry(kOwnId, relay, 1000);

  Protocol::GossipEntry out[8];
  // Capacity 1: own-first priority means the own opinion is collected, the
  // relay stays pending for a later call.
  uint8_t count = Coop::collectOutgoing(out, 1, 1000);
  TEST_ASSERT_EQUAL_UINT8(1, count);
  TEST_ASSERT_EQUAL_HEX16(kOwnId, out[0].observerId);
  TEST_ASSERT_TRUE(Coop::sendDue(1000 + Coop::kMinSendGapMs)); // relay still pending, past the gap

  count = Coop::collectOutgoing(out, 8, 1000 + Coop::kMinSendGapMs);
  TEST_ASSERT_EQUAL_UINT8(1, count);
  TEST_ASSERT_EQUAL_HEX16(5, out[0].observerId); // the relay, collected on the next call
}

// Regression test for Finding 8: even with a large backlog, this device
// must not key up for gossip more often than kMinSendGapMs allows.
void test_send_gap_throttles_repeated_sends() {
  Coop::setOwnOpinion(kOwnId, 2, Protocol::GossipStatus::Weak, true, 1000);
  Protocol::GossipEntry out[8];
  TEST_ASSERT_TRUE(Coop::sendDue(1000)); // first-ever send -- gap not applied yet
  Coop::collectOutgoing(out, 8, 1000);

  // A new opinion becomes pending immediately after -- the gap floor must
  // still block sendDue() regardless of what's pending.
  Coop::setOwnOpinion(kOwnId, 3, Protocol::GossipStatus::Lost, true, 1001);
  TEST_ASSERT_FALSE(Coop::sendDue(1001)); // 1ms since the last send
  TEST_ASSERT_FALSE(Coop::sendDue(1000 + Coop::kMinSendGapMs - 1)); // still just short of the gap
  TEST_ASSERT_TRUE(Coop::sendDue(1000 + Coop::kMinSendGapMs)); // gap elapsed -- due again
}

void test_merge_compound_key_distinguishes_subjects_from_same_observer() {
  Protocol::GossipEntry aboutSubjectA{5, 6, Protocol::GossipStatus::Weak, 2, 1};
  Protocol::GossipEntry aboutSubjectB{5, 7, Protocol::GossipStatus::Lost, 2, 1};
  Coop::mergeEntry(kOwnId, aboutSubjectA, 1000);
  Coop::mergeEntry(kOwnId, aboutSubjectB, 1000);

  // Observer 5 has an opinion about TWO different subjects -- both rows
  // must survive independently, keyed by the full (observerId, subjectId)
  // pair rather than observerId alone.
  Protocol::GossipEntry secondForA{7, 6, Protocol::GossipStatus::Weak, 2, 1};
  Protocol::GossipEntry secondForB{7, 7, Protocol::GossipStatus::Lost, 2, 1};
  Coop::mergeEntry(kOwnId, secondForA, 1000);
  Coop::mergeEntry(kOwnId, secondForB, 1000);

  TEST_ASSERT_EQUAL(static_cast<int>(Coop::Consensus::Weak),
                     static_cast<int>(Coop::subjectConsensus(3, 6, 1000)));
  TEST_ASSERT_EQUAL(static_cast<int>(Coop::Consensus::Lost),
                     static_cast<int>(Coop::subjectConsensus(3, 7, 1000)));
}

void test_subject_consensus_insufficient_when_group_too_small() {
  Protocol::GossipEntry entry{5, 6, Protocol::GossipStatus::Lost, 2, 1};
  Coop::mergeEntry(kOwnId, entry, 1000);
  TEST_ASSERT_EQUAL(static_cast<int>(Coop::Consensus::InsufficientObservers),
                     static_cast<int>(Coop::subjectConsensus(/*groupSize=*/2, /*subjectId=*/6, 1000)));
}

void test_subject_consensus_floor_of_two_observers() {
  // groupSize=3 -> both quorum percentages floor to 2 distinct observers --
  // a single reporter (even at 100% certainty) must never be enough alone.
  Protocol::GossipEntry onlyOne{5, 6, Protocol::GossipStatus::Lost, 2, 1};
  Coop::mergeEntry(kOwnId, onlyOne, 1000);
  TEST_ASSERT_EQUAL(static_cast<int>(Coop::Consensus::Ok),
                     static_cast<int>(Coop::subjectConsensus(3, 6, 1000)));

  // This device's own opinion counts too -- adding it as the 2nd reporter
  // (not a 3rd foreign one) must be enough to cross the floor.
  Coop::setOwnOpinion(kOwnId, 6, Protocol::GossipStatus::Lost, true, 1000);
  TEST_ASSERT_EQUAL(static_cast<int>(Coop::Consensus::Lost),
                     static_cast<int>(Coop::subjectConsensus(3, 6, 1000)));
}

void test_subject_consensus_weak_quorum_without_lost_quorum() {
  // groupSize=5: weak quorum = max(2, ceil(0.4*5)=2) = 2, lost quorum =
  // max(2, ceil(0.6*5)=3) = 3.
  Protocol::GossipEntry a{5, 9, Protocol::GossipStatus::Weak, 2, 1};
  Protocol::GossipEntry b{6, 9, Protocol::GossipStatus::Weak, 2, 1};
  Coop::mergeEntry(kOwnId, a, 1000);
  Coop::mergeEntry(kOwnId, b, 1000);
  TEST_ASSERT_EQUAL(static_cast<int>(Coop::Consensus::Weak),
                     static_cast<int>(Coop::subjectConsensus(5, 9, 1000)));
}

void test_subject_consensus_lost_quorum_supersedes_weak() {
  // groupSize=5: lost quorum = 3.
  Protocol::GossipEntry a{5, 9, Protocol::GossipStatus::Lost, 2, 1};
  Protocol::GossipEntry b{6, 9, Protocol::GossipStatus::Lost, 2, 1};
  Protocol::GossipEntry c{7, 9, Protocol::GossipStatus::Lost, 2, 1};
  Coop::mergeEntry(kOwnId, a, 1000);
  Coop::mergeEntry(kOwnId, b, 1000);
  Coop::mergeEntry(kOwnId, c, 1000);
  TEST_ASSERT_EQUAL(static_cast<int>(Coop::Consensus::Lost),
                     static_cast<int>(Coop::subjectConsensus(5, 9, 1000)));
}

// Regression test for Finding 11: the quorum's ceiling rounding must
// actually matter at a group size where it differs from a plain floor.
void test_subject_consensus_ceiling_rounding_at_group_size_six() {
  // groupSize=6: weak quorum = ceil(0.4*6)=ceil(2.4)=3 (a floor-rounding bug
  // would give 2), lost quorum = ceil(0.6*6)=ceil(3.6)=4 (a floor-rounding
  // bug would give 3).
  Protocol::GossipEntry a{5, 9, Protocol::GossipStatus::Weak, 2, 1};
  Protocol::GossipEntry b{6, 9, Protocol::GossipStatus::Weak, 2, 1};
  Coop::mergeEntry(kOwnId, a, 1000);
  Coop::mergeEntry(kOwnId, b, 1000);
  TEST_ASSERT_EQUAL(static_cast<int>(Coop::Consensus::Ok),
                     static_cast<int>(Coop::subjectConsensus(6, 9, 1000))); // 2 reports insufficient

  Protocol::GossipEntry c{7, 9, Protocol::GossipStatus::Weak, 2, 1};
  Coop::mergeEntry(kOwnId, c, 1000);
  TEST_ASSERT_EQUAL(static_cast<int>(Coop::Consensus::Weak),
                     static_cast<int>(Coop::subjectConsensus(6, 9, 1000))); // 3rd report crosses ceil(2.4)=3
}

void test_subject_consensus_ignores_expired_entries() {
  Protocol::GossipEntry a{5, 9, Protocol::GossipStatus::Weak, 2, 1};
  Protocol::GossipEntry b{6, 9, Protocol::GossipStatus::Weak, 2, 1};
  Coop::mergeEntry(kOwnId, a, 1000);
  Coop::mergeEntry(kOwnId, b, 1000);
  // Fresh: meets the weak quorum (2) for a 5-device group.
  TEST_ASSERT_EQUAL(static_cast<int>(Coop::Consensus::Weak),
                     static_cast<int>(Coop::subjectConsensus(5, 9, 1000)));
  // Long after both reports have gone stale, consensus must not still count
  // them even before tick() has run a garbage-collection pass.
  TEST_ASSERT_EQUAL(static_cast<int>(Coop::Consensus::Ok),
                     static_cast<int>(Coop::subjectConsensus(5, 9, 1000 + Coop::kEntryTtlMs + 1)));
}

void test_table_overflow_evicts_globally_oldest_row() {
  // Fill the table completely, each row a distinct (observer, subject) pair
  // with strictly increasing timestamps so entry 0 is unambiguously oldest.
  for (int i = 0; i < Coop::kMaxEntries; i++) {
    Protocol::GossipEntry e{static_cast<uint16_t>(100 + i), static_cast<uint16_t>(200 + i),
                             Protocol::GossipStatus::Weak, 2, 5};
    Coop::mergeEntry(kOwnId, e, 1000 + static_cast<uint32_t>(i));
  }
  // One more: the table is full, so this must evict the globally oldest row
  // (observer=100, subject=200, from i=0) to make room.
  Protocol::GossipEntry overflow{999, 999, Protocol::GossipStatus::Weak, 2, 5};
  Coop::mergeEntry(kOwnId, overflow, 1000 + static_cast<uint32_t>(Coop::kMaxEntries));

  // The evicted row is gone -- a *lower* seq than originally stored (5) for
  // the same (observer, subject) is now accepted as brand new.
  Protocol::GossipEntry replacement{100, 200, Protocol::GossipStatus::Weak, 2, 1};
  uint32_t replacementMs = 1000 + static_cast<uint32_t>(Coop::kMaxEntries) + 1;
  Coop::mergeEntry(kOwnId, replacement, replacementMs);
  TEST_ASSERT_TRUE(Coop::sendDue(replacementMs));
  Protocol::GossipEntry out[Coop::kMaxEntries + 1];
  uint8_t count = Coop::collectOutgoing(out, Coop::kMaxEntries + 1, replacementMs);
  bool sawReplacement = false;
  for (uint8_t i = 0; i < count; i++) {
    if (out[i].observerId == 100 && out[i].subjectId == 200 && out[i].seq == 1) {
      sawReplacement = true;
    }
  }
  TEST_ASSERT_TRUE(sawReplacement);
}

void test_resolve_verdict_insufficient_observers_falls_back_to_local() {
  Coop::EffectiveState s1 =
      Coop::resolveVerdict(Coop::Consensus::InsufficientObservers, /*localLost=*/true, /*localWeak=*/false);
  TEST_ASSERT_TRUE(s1.dropped);
  TEST_ASSERT_FALSE(s1.weak);

  Coop::EffectiveState s2 =
      Coop::resolveVerdict(Coop::Consensus::InsufficientObservers, /*localLost=*/false, /*localWeak=*/true);
  TEST_ASSERT_FALSE(s2.dropped);
  TEST_ASSERT_TRUE(s2.weak);

  Coop::EffectiveState s3 = Coop::resolveVerdict(Coop::Consensus::InsufficientObservers, false, false);
  TEST_ASSERT_FALSE(s3.dropped);
  TEST_ASSERT_FALSE(s3.weak);

  // localLost takes priority over localWeak even if both happen to be true.
  Coop::EffectiveState s4 = Coop::resolveVerdict(Coop::Consensus::InsufficientObservers, true, true);
  TEST_ASSERT_TRUE(s4.dropped);
  TEST_ASSERT_FALSE(s4.weak);
}

void test_resolve_verdict_ok_suppresses_regardless_of_local() {
  Coop::EffectiveState s = Coop::resolveVerdict(Coop::Consensus::Ok, /*localLost=*/true, /*localWeak=*/true);
  TEST_ASSERT_FALSE(s.dropped);
  TEST_ASSERT_FALSE(s.weak);
}

void test_resolve_verdict_weak_and_lost_ignore_local() {
  Coop::EffectiveState weak = Coop::resolveVerdict(Coop::Consensus::Weak, false, false);
  TEST_ASSERT_FALSE(weak.dropped);
  TEST_ASSERT_TRUE(weak.weak);

  Coop::EffectiveState lost = Coop::resolveVerdict(Coop::Consensus::Lost, false, false);
  TEST_ASSERT_TRUE(lost.dropped);
  TEST_ASSERT_FALSE(lost.weak);
}

void test_decide_alert_fires_dropped_on_rising_edge_only() {
  Coop::AlertDecision d1 = Coop::decideAlert(/*wasDropped=*/false, /*isDropped=*/true, false, false);
  TEST_ASSERT_TRUE(d1.fireDropped);
  TEST_ASSERT_FALSE(d1.fireWeak);

  Coop::AlertDecision d2 = Coop::decideAlert(/*wasDropped=*/true, /*isDropped=*/true, false, false);
  TEST_ASSERT_FALSE(d2.fireDropped); // still true, not a new event
  TEST_ASSERT_FALSE(d2.fireWeak);
}

void test_decide_alert_fires_weak_on_rising_edge_from_ok() {
  Coop::AlertDecision d = Coop::decideAlert(/*wasDropped=*/false, /*isDropped=*/false, /*wasWeak=*/false,
                                             /*isWeak=*/true);
  TEST_ASSERT_FALSE(d.fireDropped);
  TEST_ASSERT_TRUE(d.fireWeak);
}

// Regression test for Finding 6: a Lost->Weak transition (the peer
// recovering) must NOT fire a falling-back alert.
void test_decide_alert_suppresses_weak_on_deescalation_from_dropped() {
  Coop::AlertDecision d = Coop::decideAlert(/*wasDropped=*/true, /*isDropped=*/false, /*wasWeak=*/false,
                                             /*isWeak=*/true);
  TEST_ASSERT_FALSE(d.fireDropped);
  TEST_ASSERT_FALSE(d.fireWeak); // improvement, not a new event -- no beep
}

void test_decide_alert_fires_nothing_when_nothing_changes() {
  Coop::AlertDecision d = Coop::decideAlert(false, false, false, false);
  TEST_ASSERT_FALSE(d.fireDropped);
  TEST_ASSERT_FALSE(d.fireWeak);

  Coop::AlertDecision d2 = Coop::decideAlert(true, true, false, false);
  TEST_ASSERT_FALSE(d2.fireDropped);
  TEST_ASSERT_FALSE(d2.fireWeak);
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_set_own_opinion_arms_send_and_appears_in_collect);
  RUN_TEST(test_repeated_same_opinion_does_not_rearm_send);
  RUN_TEST(test_own_opinion_retract_frees_row);
  RUN_TEST(test_merge_ignores_own_observer_id);
  RUN_TEST(test_merge_relay_decrements_ttl_and_stops_at_zero);
  RUN_TEST(test_merge_dedup_drops_stale_and_duplicate_seq);
  RUN_TEST(test_merge_rejects_older_seq_even_with_different_status);
  RUN_TEST(test_merge_seq_comparison_handles_wraparound);
  RUN_TEST(test_entry_expires_and_row_is_freed);
  RUN_TEST(test_own_opinion_refreshes_on_schedule);
  RUN_TEST(test_own_opinion_survives_repeated_refresh_past_old_ttl_window);
  RUN_TEST(test_foreign_entry_survives_repeated_refresh_with_increasing_seq);
  RUN_TEST(test_collect_outgoing_respects_capacity);
  RUN_TEST(test_collect_outgoing_capacity_zero_writes_nothing);
  RUN_TEST(test_collect_outgoing_mixes_own_and_relay_own_first);
  RUN_TEST(test_send_gap_throttles_repeated_sends);
  RUN_TEST(test_merge_compound_key_distinguishes_subjects_from_same_observer);
  RUN_TEST(test_subject_consensus_insufficient_when_group_too_small);
  RUN_TEST(test_subject_consensus_floor_of_two_observers);
  RUN_TEST(test_subject_consensus_weak_quorum_without_lost_quorum);
  RUN_TEST(test_subject_consensus_lost_quorum_supersedes_weak);
  RUN_TEST(test_subject_consensus_ceiling_rounding_at_group_size_six);
  RUN_TEST(test_subject_consensus_ignores_expired_entries);
  RUN_TEST(test_table_overflow_evicts_globally_oldest_row);
  RUN_TEST(test_resolve_verdict_insufficient_observers_falls_back_to_local);
  RUN_TEST(test_resolve_verdict_ok_suppresses_regardless_of_local);
  RUN_TEST(test_resolve_verdict_weak_and_lost_ignore_local);
  RUN_TEST(test_decide_alert_fires_dropped_on_rising_edge_only);
  RUN_TEST(test_decide_alert_fires_weak_on_rising_edge_from_ok);
  RUN_TEST(test_decide_alert_suppresses_weak_on_deescalation_from_dropped);
  RUN_TEST(test_decide_alert_fires_nothing_when_nothing_changes);
  return UNITY_END();
}
