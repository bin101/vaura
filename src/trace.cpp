#include "trace.h"

#include "config.h"
#include "roster.h"

namespace Trace {

namespace {
struct Sample {
  uint32_t tMs;
  uint16_t nodeId;
  int16_t rawDbm;
  int16_t fastDbm;
  int16_t slowDbm;
  int16_t baselineDbm;
  int16_t floorDbm;
  bool baselineEstablished;
  bool localWeak;
};

Sample *buffer_ = nullptr;
size_t capacity_ = 0;
size_t writeIndex_ = 0; // next slot to write
size_t count_ = 0; // valid samples currently stored, <= capacity_
bool enabled_ = false;

void push(const Sample &s) {
  if (capacity_ == 0) {
    return;
  }
  buffer_[writeIndex_] = s;
  writeIndex_ = (writeIndex_ + 1) % capacity_;
  if (count_ < capacity_) {
    count_++;
  }
}
} // namespace

void begin() {
  // psramFound()/ps_malloc() are standard Arduino-ESP32 APIs -- safe to call
  // even on a board without PSRAM fitted/enabled, they simply report/return
  // false/nullptr in that case, and the fallback below takes over.
  if (psramFound()) {
    buffer_ = static_cast<Sample *>(ps_malloc(sizeof(Sample) * TRACE_CAPACITY_PSRAM_SAMPLES));
    if (buffer_ != nullptr) {
      capacity_ = TRACE_CAPACITY_PSRAM_SAMPLES;
      Serial.printf("Trace: %u-sample buffer allocated in PSRAM.\n", static_cast<unsigned>(capacity_));
      return;
    }
    Serial.println("Trace: PSRAM present but allocation failed -- falling back to a small RAM buffer.");
  }
  buffer_ = static_cast<Sample *>(malloc(sizeof(Sample) * TRACE_CAPACITY_RAM_FALLBACK_SAMPLES));
  if (buffer_ != nullptr) {
    capacity_ = TRACE_CAPACITY_RAM_FALLBACK_SAMPLES;
    Serial.printf("Trace: %u-sample buffer allocated in RAM (no PSRAM) -- a short recording window only.\n",
                  static_cast<unsigned>(capacity_));
  } else {
    // Genuinely out of RAM (unusual, but the feature must degrade gracefully
    // rather than crash the whole device over a debug tool) -- trace stays a
    // permanent, harmless no-op.
    capacity_ = 0;
    Serial.println("Trace: buffer allocation failed entirely -- trace disabled for this boot.");
  }
}

void setEnabled(bool on) { enabled_ = on; }

bool isEnabled() { return enabled_; }

void clear() {
  writeIndex_ = 0;
  count_ = 0;
}

void sample(uint32_t nowMs) {
  if (!enabled_ || capacity_ == 0) {
    return;
  }
  int16_t baselineDbm = Roster::groupBaselineDbm();
  int16_t floorDbm = Roster::activeFloorDbm();
  bool established = Roster::baselineEstablished();

  Roster::PeerInfo p;
  int i = 0;
  while (Roster::peerInfo(i, p)) {
    Sample s;
    s.tMs = nowMs;
    s.nodeId = p.nodeId;
    s.rawDbm = p.lastRawRssiDbm;
    s.fastDbm = p.rssiDbm;
    s.slowDbm = p.rssiSlowDbm;
    s.baselineDbm = baselineDbm;
    s.floorDbm = floorDbm;
    s.baselineEstablished = established;
    s.localWeak = p.localFallingBack;
    push(s);
    i++;
  }
}

void dumpCsv() {
  if (capacity_ == 0) {
    Serial.println("Trace: no buffer allocated.");
    return;
  }
  Serial.println("t_ms,node_id,raw_dbm,fast_dbm,slow_dbm,baseline_dbm,floor_dbm,baseline_established,local_weak");
  // Oldest first: when the ring hasn't wrapped yet, that's just index 0..
  // count_-1; once wrapped, the oldest sample is the one about to be
  // overwritten next (writeIndex_), and index 0..count_-1 is read starting
  // there, wrapping around the array.
  size_t start = (count_ < capacity_) ? 0 : writeIndex_;
  for (size_t k = 0; k < count_; k++) {
    const Sample &s = buffer_[(start + k) % capacity_];
    Serial.printf("%lu,%04X,%d,%d,%d,%d,%d,%d,%d\n", static_cast<unsigned long>(s.tMs), s.nodeId, s.rawDbm,
                  s.fastDbm, s.slowDbm, s.baselineDbm, s.floorDbm, s.baselineEstablished ? 1 : 0,
                  s.localWeak ? 1 : 0);
  }
}

size_t capacity() { return capacity_; }

size_t count() { return count_; }

} // namespace Trace
