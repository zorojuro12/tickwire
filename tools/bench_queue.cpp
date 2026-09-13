// Producer/consumer microbenchmark for the queue handoff seam: measures
// ns-per-handoff for each ring arm, over a real net::PacketSlot (1208 B) --
// the size the real ingest seam actually carries, not a synthetic 4-byte
// payload, which would measure the wrong thing.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>

#include "net/transport.h"
#include "server/clock.h"
#include "server/jitter_stats.h"
#include "server/mutex_ring.h"
#include "server/spsc_ring.h"

namespace {

constexpr size_t kRingCapacity = 256;

void printUsage() {
  std::fprintf(stderr,
               "usage: bench_queue --ring <spsc|mutex> [--items <n>] [--rate <n>]\n"
               "  --ring <spsc|mutex>  ring arm to benchmark\n"
               "  --items <n>          items to transfer (default 10000)\n"
               "  --rate <n>           items/sec; 0 = as fast as possible (default 0)\n");
}

template <typename Ring>
void runBench(const std::string& ring_name, uint64_t items, uint64_t rate) {
  auto ring = std::make_unique<Ring>();
  auto stats = std::make_unique<server::JitterStats<1u << 20>>();

  std::thread producer([&] {
    const uint64_t interval_ns = rate > 0 ? 1'000'000'000ull / rate : 0;
    uint64_t next_send_ns = server::monotonicNs();
    for (uint64_t i = 0; i < items; ++i) {
      if (interval_ns > 0) {
        while (server::monotonicNs() < next_send_ns) {
        }
        next_send_ns += interval_ns;
      }
      net::PacketSlot* slot;
      while ((slot = ring->acquireWrite()) == nullptr) std::this_thread::yield();
      const uint64_t stamp = server::monotonicNs();
      std::memcpy(slot->data.data(), &stamp, sizeof(stamp));
      ring->commitWrite();
    }
  });

  const uint64_t start_ns = server::monotonicNs();
  uint64_t observed = 0;
  while (observed < items) {
    net::PacketSlot* slot = ring->acquireRead();
    if (slot == nullptr) {
      std::this_thread::yield();
      continue;
    }
    uint64_t stamp = 0;
    std::memcpy(&stamp, slot->data.data(), sizeof(stamp));
    const uint64_t now_ns = server::monotonicNs();
    // The stamp and this read happen on different threads (and, in
    // practice, different cores). CLOCK_MONOTONIC's monotonicity guarantee
    // is per-thread; observed on this environment, cross-core reads can
    // show a sub-microsecond apparent inversion, which unsigned subtraction
    // would wrap to a huge, benchmark-poisoning value. Clamp to zero rather
    // than let clock noise dominate every percentile.
    const int64_t delta_ns = static_cast<int64_t>(now_ns) - static_cast<int64_t>(stamp);
    stats->record(delta_ns > 0 ? static_cast<uint64_t>(delta_ns) : 0);
    ++observed;
    ring->commitRead();
  }
  const uint64_t end_ns = server::monotonicNs();
  producer.join();

  std::printf(
      "ring=%s items=%llu rate=%llu ns_per_handoff_p50=%llu ns_per_handoff_p99=%llu "
      "total_ms=%llu\n",
      ring_name.c_str(), static_cast<unsigned long long>(items),
      static_cast<unsigned long long>(rate),
      static_cast<unsigned long long>(stats->percentileNs(0.50)),
      static_cast<unsigned long long>(stats->percentileNs(0.99)),
      static_cast<unsigned long long>((end_ns - start_ns) / 1'000'000ull));
}

}  // namespace

int main(int argc, char** argv) {
  std::string ring;
  uint64_t items = 10000;
  uint64_t rate = 0;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--ring" && i + 1 < argc) {
      ring = argv[++i];
    } else if (arg == "--items" && i + 1 < argc) {
      items = static_cast<uint64_t>(std::atoll(argv[++i]));
    } else if (arg == "--rate" && i + 1 < argc) {
      rate = static_cast<uint64_t>(std::atoll(argv[++i]));
    } else {
      printUsage();
      return 1;
    }
  }

  if (ring == "spsc") {
    runBench<server::SpscRing<net::PacketSlot, kRingCapacity>>(ring, items, rate);
  } else if (ring == "mutex") {
    runBench<server::MutexRing<net::PacketSlot, kRingCapacity>>(ring, items, rate);
  } else {
    printUsage();
    return 1;
  }
  return 0;
}
