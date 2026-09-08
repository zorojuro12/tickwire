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

}  // namespace net
