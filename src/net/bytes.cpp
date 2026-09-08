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

void ByteWriter::bytes(std::span<const std::byte> src) noexcept {
  if (!ok_) return;
  if (src.empty()) return;
  if (cursor_ + src.size() > buf_.size()) { ok_ = false; return; }
  std::memcpy(buf_.data() + cursor_, src.data(), src.size());
  cursor_ += src.size();
}

bool ByteWriter::ok() const noexcept { return ok_; }
size_t ByteWriter::size() const noexcept { return cursor_; }

ByteReader::ByteReader(std::span<const std::byte> buf) noexcept : buf_(buf) {}

uint8_t ByteReader::u8() noexcept {
  if (!ok_) return 0;
  if (cursor_ + 1 > buf_.size()) { ok_ = false; return 0; }
  const uint8_t v = std::to_integer<uint8_t>(buf_[cursor_]);
  cursor_ += 1;
  return v;
}

uint16_t ByteReader::u16() noexcept {
  if (!ok_) return 0;
  if (cursor_ + 2 > buf_.size()) { ok_ = false; return 0; }
  const uint16_t v =
      static_cast<uint16_t>(std::to_integer<uint8_t>(buf_[cursor_ + 0])) |
      static_cast<uint16_t>(static_cast<uint16_t>(std::to_integer<uint8_t>(buf_[cursor_ + 1])) << 8);
  cursor_ += 2;
  return v;
}

uint32_t ByteReader::u32() noexcept {
  if (!ok_) return 0;
  if (cursor_ + 4 > buf_.size()) { ok_ = false; return 0; }
  const uint32_t v =
      static_cast<uint32_t>(std::to_integer<uint8_t>(buf_[cursor_ + 0])) |
      (static_cast<uint32_t>(std::to_integer<uint8_t>(buf_[cursor_ + 1])) << 8) |
      (static_cast<uint32_t>(std::to_integer<uint8_t>(buf_[cursor_ + 2])) << 16) |
      (static_cast<uint32_t>(std::to_integer<uint8_t>(buf_[cursor_ + 3])) << 24);
  cursor_ += 4;
  return v;
}

float ByteReader::f32() noexcept {
  const uint32_t bits = u32();
  float v;
  std::memcpy(&v, &bits, sizeof(v));
  return v;
}

bool ByteReader::ok() const noexcept { return ok_; }
size_t ByteReader::remaining() const noexcept {
  if (!ok_) return 0;
  return buf_.size() - cursor_;
}

}  // namespace net
