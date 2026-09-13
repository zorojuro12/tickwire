// Deliberately broken copy of SpscRing: every atomic operation uses
// memory_order_relaxed and there is no alignas(64) padding. Proves TSan
// would have failed on a wrong ordering -- exercised only under the TSan
// build, never included by production code.
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <thread>

namespace {

template <typename T, size_t N>
class RelaxedRing {
 public:
  T* acquireWrite() noexcept {
    const uint64_t w = write_.load(std::memory_order_relaxed);
    const uint64_t r = read_.load(std::memory_order_relaxed);
    if (w - r == N) return nullptr;
    return &buf_[w & (N - 1)];
  }
  void commitWrite() noexcept {
    write_.store(write_.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
  }

  T* acquireRead() noexcept {
    const uint64_t r = read_.load(std::memory_order_relaxed);
    const uint64_t w = write_.load(std::memory_order_relaxed);
    if (w == r) return nullptr;
    return &buf_[r & (N - 1)];
  }
  void commitRead() noexcept {
    read_.store(read_.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
  }

 private:
  std::atomic<uint64_t> write_{0};
  std::atomic<uint64_t> read_{0};
  std::array<T, N> buf_{};
};

}  // namespace

int main() {
  constexpr uint64_t kItems = 10000;
  RelaxedRing<uint64_t, 64> ring;

  std::thread producer([&ring] {
    for (uint64_t v = 1; v <= kItems; ++v) {
      uint64_t* slot;
      while ((slot = ring.acquireWrite()) == nullptr) std::this_thread::yield();
      *slot = v;
      ring.commitWrite();
    }
  });

  std::thread consumer([&ring] {
    uint64_t observed = 0;
    while (observed < kItems) {
      uint64_t* slot = ring.acquireRead();
      if (slot == nullptr) {
        std::this_thread::yield();
        continue;
      }
      volatile uint64_t v = *slot;
      (void)v;
      ++observed;
      ring.commitRead();
    }
  });

  producer.join();
  consumer.join();
  return 0;
}
