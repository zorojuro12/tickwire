#include "net/bytes.h"

#include <cstring>

namespace net {

ByteWriter::ByteWriter(std::span<std::byte> buf) noexcept : buf_(buf) {}

void ByteWriter::u8(uint8_t v) noexcept {
  if (!ok_) return;
  if (cursor_ + 1 > buf_.size()) { ok_ = false; return; }
  buf_[cursor_] = std::byte{v};
  cursor_ += 1;
}

void ByteWriter::u16(uint16_t v) noexcept {
  if (!ok_) return;
  if (cursor_ + 2 > buf_.size()) { ok_ = false; return; }
  buf_[cursor_ + 0] = std::byte(v & 0xFFu);
  buf_[cursor_ + 1] = std::byte((v >> 8) & 0xFFu);
  cursor_ += 2;
}

void ByteWriter::u32(uint32_t v) noexcept {
  if (!ok_) return;
  if (cursor_ + 4 > buf_.size()) { ok_ = false; return; }
  buf_[cursor_ + 0] = std::byte(v & 0xFFu);
  buf_[cursor_ + 1] = std::byte((v >> 8) & 0xFFu);
  buf_[cursor_ + 2] = std::byte((v >> 16) & 0xFFu);
  buf_[cursor_ + 3] = std::byte((v >> 24) & 0xFFu);
  cursor_ += 4;
}

void ByteWriter::f32(float v) noexcept {
  uint32_t bits;
  std::memcpy(&bits, &v, sizeof(bits));
  u32(bits);
}

bool ByteWriter::ok() const noexcept { return ok_; }
size_t ByteWriter::size() const noexcept { return cursor_; }

}  // namespace net
