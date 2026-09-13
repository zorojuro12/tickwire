#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <thread>

#include "net/transport.h"
#include "server/jitter_stats.h"
#include "server/poll_set.h"
#include "server/server.h"
#include "server/tick_timer.h"

namespace server {

// Splits Server<T, Ring>'s I/O and simulation across two real threads, along
// the existing ingest()/tick() seam. The I/O thread owns ingest(); the sim
// loop runs on the thread that calls run(), which stays synchronous (no
// separate join() needed in the caller's happy path) and joins the I/O
// thread before returning.
//
// What's shared between the threads: only `ring_` inside Server (the reason
// Ring is swappable) and the transport, which needs no mutex -- send() and
// tryReceive() touch only the fd and mutate no shared member state, and
// POSIX allows concurrent sendto/recvfrom on one socket. Everything else in
// Server is touched only by tick(), i.e. only by the calling thread.
// `packets_ingested_` is written only by the I/O thread and read only after
// both threads have joined, so it needs no atomic either -- only `stop_`
// does, since it's the one flag both threads touch concurrently.
template <net::Transport T, typename Ring, size_t JitterSamples = 65536>
class ThreadedRunner {
 public:
  ThreadedRunner(Server<T, Ring>& srv, T& transport, uint32_t tick_hz,
                 std::function<uint64_t()> clock_ns,
                 std::function<uint32_t()> clock_ms) noexcept
      : srv_(srv),
        transport_(transport),
        tick_hz_(tick_hz),
        clock_ns_(std::move(clock_ns)),
        clock_ms_(std::move(clock_ms)) {}

  bool run(uint32_t ticks_limit) noexcept {
    TickTimer timer(tick_hz_);
    if (!timer.valid()) return false;

    stop_.store(false, std::memory_order_relaxed);
    ticks_run_ = 0;
    packets_ingested_ = 0;
    std::thread io_thread([this] { ioLoop(); });

    PollSet sim_poll;
    sim_poll.add(timer.fd(), 1);
    std::array<uint32_t, 4> ready{};

    uint64_t prev_ns = 0;
    bool have_prev = false;
    while (ticks_run_ < ticks_limit) {
      const size_t n = sim_poll.wait(50, ready);
      for (size_t i = 0; i < n && ticks_run_ < ticks_limit; ++i) {
        if (ready[i] != 1) continue;
        uint64_t expirations = timer.consumeExpirations();
        while (expirations > 0 && ticks_run_ < ticks_limit) {
          const uint64_t now_ns = clock_ns_();
          if (have_prev) jitter_.record(now_ns - prev_ns);
          prev_ns = now_ns;
          have_prev = true;
          srv_.tick(clock_ms_());
          ++ticks_run_;
          --expirations;
        }
      }
    }

    stop_.store(true, std::memory_order_relaxed);
    io_thread.join();
    return true;
  }

  void requestStop() noexcept { stop_.store(true, std::memory_order_relaxed); }

  uint32_t ticksRun() const noexcept { return ticks_run_; }
  JitterStats<JitterSamples>& jitter() noexcept { return jitter_; }
  uint64_t packetsIngested() const noexcept { return packets_ingested_; }

 private:
  void ioLoop() noexcept {
    if constexpr (requires { transport_.nativeHandle(); }) {
      PollSet poll;
      poll.add(transport_.nativeHandle(), 1);
      std::array<uint32_t, 4> ready{};
      while (!stop_.load(std::memory_order_relaxed)) {
        const size_t n = poll.wait(50, ready);
        for (size_t i = 0; i < n; ++i) packets_ingested_ += srv_.ingest();
      }
    } else {
      // LoopbackTransport has no pollable fd -- drain in a yield loop.
      while (!stop_.load(std::memory_order_relaxed)) {
        packets_ingested_ += srv_.ingest();
        std::this_thread::yield();
      }
    }
  }

  Server<T, Ring>& srv_;
  T& transport_;
  uint32_t tick_hz_;
  std::function<uint64_t()> clock_ns_;
  std::function<uint32_t()> clock_ms_;

  std::atomic<bool> stop_{false};
  uint32_t ticks_run_ = 0;
  uint64_t packets_ingested_ = 0;
  JitterStats<JitterSamples> jitter_;
};

}  // namespace server
