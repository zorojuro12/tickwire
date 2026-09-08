#include "net/framing.h"

#include "sim/sim.h"

namespace net {

bool encodeJoinAccept(uint32_t player_id, ByteWriter& w) {
  if (player_id == sim::kInvalidPlayerId) return false;
  w.u32(player_id);
  return w.ok();
}

bool decodeJoinAccept(ByteReader& r, uint32_t& out) {
  if (r.remaining() != kJoinAcceptBytes) return false;
  const uint32_t v = r.u32();
  if (!r.ok()) return false;
  if (v == sim::kInvalidPlayerId) return false;
  out = v;
  return true;
}

size_t framePacket(PacketHeader h, std::span<const std::byte> payload,
                    std::span<std::byte> out) {
  if (payload.size() > kMaxPacket - kHeaderBytes) return 0;
  if (out.size() < kHeaderBytes + payload.size()) return 0;

  h.payload_len = static_cast<uint16_t>(payload.size());
  ByteWriter w(out);
  if (!encodeHeader(h, w)) return 0;
  w.bytes(payload);
  if (!w.ok()) return 0;
  return w.size();
}

}  // namespace net
