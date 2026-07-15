#include "stats.h"

namespace Stats {

namespace {
uint32_t warningsSent_ = 0;
uint32_t warningsReceived_ = 0;
uint32_t dropOffs_ = 0;
} // namespace

void countWarningSent() { warningsSent_++; }
void countWarningReceived() { warningsReceived_++; }
void countDropOff() { dropOffs_++; }

uint32_t warningsSent() { return warningsSent_; }
uint32_t warningsReceived() { return warningsReceived_; }
uint32_t dropOffs() { return dropOffs_; }

} // namespace Stats
