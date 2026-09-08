#pragma once

#include <cstddef>
#include <cstdint>

#include "net/bytes.h"

namespace net {

inline constexpr size_t kJoinAcceptBytes = 4;

bool encodeJoinAccept(uint32_t player_id, ByteWriter& w);
bool decodeJoinAccept(ByteReader& r, uint32_t& out);

}  // namespace net
