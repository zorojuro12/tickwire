#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace net {

// Bounds-checked little-endian write cursor over a caller-owned buffer.
// A write that would not fit entirely writes nothing and does not advance
// the cursor.
class ByteWriter {
 public:
  explicit ByteWriter(std::span<std::byte> buf) noexcept;

  void u8(uint8_t v) noexcept;
  void u16(uint16_t v) noexcept;
  void u32(uint32_t v) noexcept;
  void f32(float v) noexcept;

  bool ok() const noexcept;
  size_t size() const noexcept;

 private:
  std::span<std::byte> buf_;
  size_t cursor_ = 0;
};

// Bounds-checked little-endian read cursor over a caller-owned buffer.
// A read that would run past the end consumes nothing and returns 0.
class ByteReader {
 public:
  explicit ByteReader(std::span<const std::byte> buf) noexcept;

  uint8_t u8() noexcept;
  uint16_t u16() noexcept;
  uint32_t u32() noexcept;
  float f32() noexcept;

  bool ok() const noexcept;
  size_t remaining() const noexcept;

 private:
  std::span<const std::byte> buf_;
  size_t cursor_ = 0;
};

}  // namespace net
