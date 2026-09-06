#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <span>

#include "net/transport.h"
#include "sim/sim.h"

namespace net {

struct SimConfig {
  uint32_t latency_ms = 0;
  uint32_t jitter_ms = 0;
  uint32_t loss_permille = 0;  // 0..1000; integer so no float enters the schedule
  uint64_t seed = 0;
};

inline constexpr size_t kDelayCapacity = 128;

// Milliseconds to ticks, rounded to nearest. Integer arithmetic only.
constexpr uint32_t msToTicks(uint32_t ms) noexcept {
  return (ms * sim::kTickHz + 500u) / 1000u;
}

// Wraps an inner Transport to add deterministic latency, jitter, and loss.
// send() is a pure pass-through; delay is applied on receive only, so each
// endpoint delays its own inbound traffic and a two-ended test's RTT is the
// sum of both sides' latency.
template <Transport Inner>
class SimulatedTransport {
 public:
  SimulatedTransport(Inner& inner, const SimConfig& cfg) noexcept
      : inner_(inner),
        cfg_(cfg),
        latency_ticks_(msToTicks(cfg.latency_ms)),
        jitter_ticks_(msToTicks(cfg.jitter_ms)) {}

  bool send(const Endpoint& to, std::span<const std::byte> payload) {
    return inner_.send(to, payload);
  }

  bool tryReceive(PacketSlot& slot) {
    PacketSlot tmp;
    while (inner_.tryReceive(tmp)) {
      const uint64_t delivery_tick = tick_ + latency_ticks_;

      bool inserted = false;
      for (Entry& e : delay_buf_) {
        if (e.occupied) continue;
        e.occupied = true;
        e.delivery_tick = delivery_tick;
        e.seq = seq_counter_++;
        e.packet = tmp;
        inserted = true;
        break;
      }
      if (!inserted) ++dropped_by_capacity_;
    }

    int best = -1;
    for (size_t i = 0; i < delay_buf_.size(); ++i) {
      const Entry& e = delay_buf_[i];
      if (!e.occupied) continue;
      if (e.delivery_tick > tick_) continue;
      if (best < 0 || e.delivery_tick < delay_buf_[static_cast<size_t>(best)].delivery_tick ||
          (e.delivery_tick == delay_buf_[static_cast<size_t>(best)].delivery_tick &&
           e.seq < delay_buf_[static_cast<size_t>(best)].seq)) {
        best = static_cast<int>(i);
      }
    }
    if (best < 0) return false;

    slot = delay_buf_[static_cast<size_t>(best)].packet;
    delay_buf_[static_cast<size_t>(best)].occupied = false;
    return true;
  }

  void advanceTick() noexcept { ++tick_; }
  uint64_t tick() const noexcept { return tick_; }
  uint64_t droppedByLoss() const noexcept { return dropped_by_loss_; }
  uint64_t droppedByCapacity() const noexcept { return dropped_by_capacity_; }

 private:
  struct Entry {
    bool occupied = false;
    uint64_t delivery_tick = 0;
    uint64_t seq = 0;
    PacketSlot packet;
  };

  Inner& inner_;
  SimConfig cfg_;
  uint32_t latency_ticks_;
  uint32_t jitter_ticks_;
  uint64_t tick_ = 0;
  uint64_t seq_counter_ = 0;
  uint64_t dropped_by_loss_ = 0;
  uint64_t dropped_by_capacity_ = 0;
  std::array<Entry, kDelayCapacity> delay_buf_{};
};

}  // namespace net
