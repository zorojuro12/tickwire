#include "net/protocol.h"

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
  h.type = static_cast<MsgType>(raw_type);
  h.payload_len = r.u16();
  h.tick = r.u32();
  h.send_time_ms = r.u32();
  h.ack_tick = r.u32();
  h.seq = r.u16();
  h.ack_seq = r.u16();

  if (!r.ok()) return false;

  out = h;
  return true;
}

}  // namespace net
