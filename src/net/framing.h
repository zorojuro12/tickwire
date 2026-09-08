#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "net/bytes.h"
#include "net/protocol.h"

namespace net {

inline constexpr size_t kJoinAcceptBytes = 4;

bool encodeJoinAccept(uint32_t player_id, ByteWriter& w);
bool decodeJoinAccept(ByteReader& r, uint32_t& out);

// Frames header + payload into out. Sets h.payload_len from payload.size().
// Returns bytes written, or 0 on failure (payload too large, out too small).
size_t framePacket(PacketHeader h, std::span<const std::byte> payload,
                    std::span<std::byte> out);

}  // namespace net
