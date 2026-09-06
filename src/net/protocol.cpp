#include "net/protocol.h"

#include <cmath>

namespace net {

bool encodeHeader(const PacketHeader& h, ByteWriter& w) {
  w.u32(h.magic);
  w.u8(h.version);
  w.u8(static_cast<uint8_t>(h.type));
  w.u16(h.payload_len);
  w.u32(h.tick);
  w.u32(h.send_time_ms);
  w.u32(h.ack_tick);
  w.u16(h.seq);
  w.u16(h.ack_seq);
  return w.ok();
}

bool decodeHeader(ByteReader& r, PacketHeader& out) {
  PacketHeader h;
  h.magic = r.u32();
  h.version = r.u8();
  const uint8_t raw_type = r.u8();
  h.payload_len = r.u16();
  h.tick = r.u32();
  h.send_time_ms = r.u32();
  h.ack_tick = r.u32();
  h.seq = r.u16();
  h.ack_seq = r.u16();

  if (!r.ok()) return false;
  if (h.magic != kProtocolMagic) return false;
  if (h.version != kProtocolVersion) return false;
  if (raw_type == 0 || raw_type > kMaxMsgType) return false;
  if (h.payload_len > kMaxPacket - kHeaderBytes) return false;
  if (r.remaining() != h.payload_len) return false;

  h.type = static_cast<MsgType>(raw_type);
  out = h;
  return true;
}

bool encodeInput(const sim::InputCommand& in, ByteWriter& w) {
  w.u32(in.player_id);
  w.u32(in.tick);
  w.f32(in.move_x);
  w.f32(in.move_y);
  w.u8(in.fire ? 1 : 0);
  return w.ok();
}

bool decodeInput(ByteReader& r, sim::InputCommand& out) {
  if (r.remaining() != kInputBytes) return false;

  sim::InputCommand in{};
  in.player_id = r.u32();
  in.tick = r.u32();
  in.move_x = r.f32();
  in.move_y = r.f32();
  in.fire = r.u8() != 0;

  if (!r.ok()) return false;
  if (!std::isfinite(in.move_x) || !std::isfinite(in.move_y)) return false;

  out = in;
  return true;
}

bool encodeSnapshot(const sim::WorldSnapshot& s, ByteWriter& w) {
  if (s.count > sim::kMaxPlayers) return false;

  w.u32(s.tick);
  w.u32(s.count);
  for (uint32_t i = 0; i < s.count; ++i) {
    const sim::PlayerState& p = s.players[i];
    w.u32(p.id);
    w.f32(p.x);
    w.f32(p.y);
    w.f32(p.vx);
    w.f32(p.vy);
    w.f32(p.radius);
  }
  return w.ok();
}

bool decodeSnapshot(ByteReader& r, sim::WorldSnapshot& out) {
  sim::WorldSnapshot s{};
  s.tick = r.u32();
  s.count = r.u32();

  if (!r.ok()) return false;
  if (s.count > sim::kMaxPlayers) return false;
  if (r.remaining() != static_cast<size_t>(s.count) * kPlayerStateBytes) return false;

  for (uint32_t i = 0; i < s.count; ++i) {
    sim::PlayerState& p = s.players[i];
    p.id = r.u32();
    p.x = r.f32();
    p.y = r.f32();
    p.vx = r.f32();
    p.vy = r.f32();
    p.radius = r.f32();
  }

  if (!r.ok()) return false;
  for (uint32_t i = 0; i < s.count; ++i) {
    const sim::PlayerState& p = s.players[i];
    if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.vx) ||
        !std::isfinite(p.vy) || !std::isfinite(p.radius)) {
      return false;
    }
  }

  out = s;
  return true;
}

}  // namespace net
