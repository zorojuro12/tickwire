#pragma once

#include <cstddef>
#include <cstdint>

#include "net/bytes.h"
#include "net/transport.h"
#include "sim/sim.h"

namespace net {

inline constexpr uint32_t kProtocolMagic = 0x52495754u;  // LE bytes spell "TWIR"
inline constexpr uint8_t kProtocolVersion = 2;  // was 1; aim_x/aim_y added to InputCommand
inline constexpr size_t kHeaderBytes = 24;

enum class MsgType : uint8_t {
  kInvalid = 0,
  kInput = 1,
  kSnapshot = 2,
  kJoinRequest = 3,
  kJoinAccept = 4,
  kLeave = 5,
  kSnapshotDelta = 6,
};
inline constexpr uint8_t kMaxMsgType = 6;

struct PacketHeader {
  uint32_t magic = kProtocolMagic;
  uint8_t version = kProtocolVersion;
  MsgType type = MsgType::kInvalid;
  uint16_t payload_len = 0;    // bytes following the header
  uint32_t tick = 0;           // sender's simulation tick        (P3)
  uint32_t send_time_ms = 0;   // sender's monotonic ms           (P3, RTT)
  uint32_t ack_tick = 0;       // highest tick seen from the peer (P3, drift)
  uint16_t seq = 0;            // reliable channel sequence       (P2, join/leave)
  uint16_t ack_seq = 0;        // reliable channel ack            (P2, join/leave)
};

bool encodeHeader(const PacketHeader& h, ByteWriter& w);
bool decodeHeader(ByteReader& r, PacketHeader& out);

inline constexpr size_t kInputBytes = 25;  // was 17
inline constexpr size_t kPlayerStateBytes = 24;
inline constexpr size_t kSnapshotFixedBytes = 8;  // tick + count
static_assert(kHeaderBytes + kSnapshotFixedBytes + sim::kMaxPlayers * kPlayerStateBytes <=
              kMaxPacket);

bool encodeInput(const sim::InputCommand& in, ByteWriter& w);
bool decodeInput(ByteReader& r, sim::InputCommand& out);
bool encodeSnapshot(const sim::WorldSnapshot& s, ByteWriter& w);
bool decodeSnapshot(ByteReader& r, sim::WorldSnapshot& out);

}  // namespace net
