#pragma once

#include <cstdint>
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
  SimulatedTransport(Inner& inner, const SimConfig& cfg) noexcept : inner_(inner), cfg_(cfg) {}

  bool send(const Endpoint& to, std::span<const std::byte> payload) {
    return inner_.send(to, payload);
  }

  bool tryReceive(PacketSlot& slot) { return inner_.tryReceive(slot); }

  void advanceTick() noexcept { ++tick_; }
  uint64_t tick() const noexcept { return tick_; }
  uint64_t droppedByLoss() const noexcept { return dropped_by_loss_; }
  uint64_t droppedByCapacity() const noexcept { return dropped_by_capacity_; }

 private:
  Inner& inner_;
  SimConfig cfg_;
  uint64_t tick_ = 0;
  uint64_t dropped_by_loss_ = 0;
  uint64_t dropped_by_capacity_ = 0;
};

}  // namespace net
