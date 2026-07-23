#include "protocol.h"

#include <string.h>

namespace Protocol {

namespace {
void writeU16(uint8_t *out, size_t offset, uint16_t value) {
  out[offset] = static_cast<uint8_t>(value & 0xFF);
  out[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xFF);
}

uint16_t readU16(const uint8_t *data, size_t offset) {
  return static_cast<uint16_t>(data[offset]) | (static_cast<uint16_t>(data[offset + 1]) << 8);
}

size_t writeHeader(uint8_t *out, MsgType type, uint16_t senderId, uint8_t seq) {
  out[0] = kMagic;
  out[1] = kVersion;
  out[2] = static_cast<uint8_t>(type);
  writeU16(out, 3, senderId);
  out[5] = seq;
  return kHeaderLen;
}
} // namespace

const char *warningLabel(WarningType type) {
  switch (type) {
    case WarningType::CarBehind:
      return "CAR BEHIND";
    case WarningType::HazardAhead:
      return "HAZARD AHEAD";
    case WarningType::Stopping:
      return "BRAKING/STOP";
    case WarningType::Regroup:
      return "REGROUP/WAIT";
    case WarningType::Attention:
      return "ATTENTION!";
    default:
      return "?";
  }
}

size_t encodeHeartbeat(uint8_t *out, uint16_t senderId, uint8_t seq, uint16_t batteryMillivolts,
                        const char *nickname) {
  size_t offset = writeHeader(out, MsgType::Heartbeat, senderId, seq);
  writeU16(out, offset, batteryMillivolts);
  offset += 2;
  // Copy up to kNicknameFieldLen chars, space-padding the rest (not
  // null-terminated on the wire to keep the packet fixed-size).
  size_t nameLen = strnlen(nickname, kNicknameFieldLen);
  memcpy(out + offset, nickname, nameLen);
  for (size_t i = nameLen; i < kNicknameFieldLen; i++) {
    out[offset + i] = ' ';
  }
  offset += kNicknameFieldLen;
  return offset;
}

size_t encodeWarning(uint8_t *out, uint16_t senderId, uint8_t seq, WarningType type) {
  size_t offset = writeHeader(out, MsgType::Warning, senderId, seq);
  out[offset++] = static_cast<uint8_t>(type);
  return offset;
}

size_t encodeGossip(uint8_t *out, uint16_t senderId, uint8_t seq, const GossipEntry *entries, uint8_t count) {
  if (count > kMaxGossipEntries) {
    count = static_cast<uint8_t>(kMaxGossipEntries); // defensive -- callers should already cap
  }
  size_t offset = writeHeader(out, MsgType::Gossip, senderId, seq);
  out[offset++] = count;
  for (uint8_t i = 0; i < count; i++) {
    writeU16(out, offset, entries[i].observerId);
    offset += 2;
    writeU16(out, offset, entries[i].subjectId);
    offset += 2;
    // status in the top bit, ttl in the low nibble -- ttl only ever needs
    // COOP_TTL_HOPS worth of range (a handful), 4 bits is generous headroom.
    out[offset++] = static_cast<uint8_t>((static_cast<uint8_t>(entries[i].status) << 7) | (entries[i].ttl & 0x0F));
    out[offset++] = entries[i].seq;
  }
  return offset;
}

bool decode(const uint8_t *data, size_t len, DecodedPacket &result) {
  if (len < kHeaderLen || data[0] != kMagic || data[1] != kVersion) {
    return false;
  }
  MsgType type = static_cast<MsgType>(data[2]);
  result.type = type;
  result.senderId = readU16(data, 3);
  result.seq = data[5];

  switch (type) {
    case MsgType::Heartbeat: {
      if (len < kHeaderLen + 2 + kNicknameFieldLen) {
        return false;
      }
      size_t offset = kHeaderLen;
      result.batteryMillivolts = readU16(data, offset);
      offset += 2;
      memcpy(result.nickname, data + offset, kNicknameFieldLen);
      result.nickname[kNicknameFieldLen] = '\0';
      // Trim trailing padding spaces for cleaner display.
      for (int i = static_cast<int>(kNicknameFieldLen) - 1; i >= 0 && result.nickname[i] == ' '; i--) {
        result.nickname[i] = '\0';
      }
      return true;
    }
    case MsgType::Warning: {
      if (len < kHeaderLen + 1) {
        return false;
      }
      uint8_t rawType = data[kHeaderLen];
      // Reject an out-of-enum byte outright rather than letting it flow
      // through as an unrecognized WarningType that only degrades late, at
      // display time, to warningLabel()'s "?" fallback -- MsgType's own
      // unknown-value case is rejected the same way just below.
      switch (static_cast<WarningType>(rawType)) {
        case WarningType::CarBehind:
        case WarningType::HazardAhead:
        case WarningType::Stopping:
        case WarningType::Regroup:
        case WarningType::Attention:
          break;
        default:
          return false;
      }
      result.warningType = static_cast<WarningType>(rawType);
      return true;
    }
    case MsgType::Gossip: {
      if (len < kHeaderLen + kGossipCountLen) {
        return false;
      }
      uint8_t count = data[kHeaderLen];
      if (count > kMaxGossipEntries) {
        return false; // malformed -- more entries than could ever legally fit
      }
      size_t needed = kHeaderLen + kGossipCountLen + static_cast<size_t>(count) * kGossipEntryWireLen;
      if (len < needed) {
        return false;
      }
      size_t offset = kHeaderLen + kGossipCountLen;
      for (uint8_t i = 0; i < count; i++) {
        result.gossipEntries[i].observerId = readU16(data, offset);
        offset += 2;
        result.gossipEntries[i].subjectId = readU16(data, offset);
        offset += 2;
        uint8_t statusTtl = data[offset++];
        result.gossipEntries[i].status = static_cast<GossipStatus>((statusTtl >> 7) & 0x01);
        result.gossipEntries[i].ttl = statusTtl & 0x0F;
        result.gossipEntries[i].seq = data[offset++];
      }
      result.gossipCount = count;
      return true;
    }
    default:
      return false;
  }
}

} // namespace Protocol
