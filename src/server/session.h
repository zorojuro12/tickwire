#pragma once

#include <array>
#include <cstdint>
#include <span>

#include "net/transport.h"
#include "sim/sim.h"

namespace server {

inline constexpr uint32_t kSessionTimeoutTicks = 300;  // 5 s at 60 Hz
inline constexpr uint32_t kFireCooldownTicks = 12;     // 5 shots per second
inline constexpr size_t kMaxExpired = sim::kMaxPlayers;

// Endpoint <-> player id binding. A fixed 32-entry linear scan -- no map,
// no allocation; this is one lookup per received packet, not a hot loop.
class SessionTable {
 public:
  // Returns the endpoint's player id, assigning one on first sight. Returns
  // kInvalidPlayerId only when the table is full and the endpoint is new.
  uint32_t joinOrGet(const net::Endpoint& from, uint32_t now_tick);
  uint32_t playerFor(const net::Endpoint& from) const noexcept;  // 0 if none
  bool endpointFor(uint32_t player_id, net::Endpoint& out) const noexcept;
  // True only when `from` holds a live session whose assigned id is player_id.
  bool authorize(const net::Endpoint& from, uint32_t player_id) const noexcept;
  void touch(const net::Endpoint& from, uint32_t now_tick, uint32_t input_tick) noexcept;
  uint32_t lastInputTick(uint32_t player_id) const noexcept;
  // Records that this endpoint's session has acknowledged holding the snapshot
  // at `snapshot_tick`. Monotonic: an older tick is ignored. No-op for an
  // endpoint with no live session.
  void noteSnapshotAck(const net::Endpoint& from, uint32_t snapshot_tick) noexcept;
  // The newest snapshot tick this player has acknowledged; 0 when the player is
  // unknown or has acknowledged nothing.
  uint32_t ackedSnapshotTick(uint32_t player_id) const noexcept;
  bool tryFire(uint32_t player_id, uint32_t now_tick) noexcept;  // false while cooling down
  bool remove(const net::Endpoint& from) noexcept;
  // Removes every session silent for >= kSessionTimeoutTicks; writes the
  // removed ids into `out` and returns how many. `out` must hold kMaxExpired
  // entries.
  size_t expire(uint32_t now_tick, std::span<uint32_t> out) noexcept;
  uint32_t count() const noexcept;
  // Iteration for broadcast: the i-th live session, i < count().
  net::Endpoint endpointAt(size_t i) const noexcept;
  uint32_t playerAt(size_t i) const noexcept;

 private:
  struct Entry {
    net::Endpoint peer;
    uint32_t player_id = sim::kInvalidPlayerId;
    uint32_t last_seen_tick = 0;
    uint32_t last_input_tick = 0;
    uint32_t last_fire_tick = 0;
    uint32_t acked_snapshot_tick = 0;
    bool ever_fired = false;
    bool live = false;
  };

  int findByEndpoint(const net::Endpoint& from) const noexcept;
  int findByPlayer(uint32_t player_id) const noexcept;
  void removeAt(size_t slot) noexcept;

  std::array<Entry, sim::kMaxPlayers> entries_{};
  uint32_t count_ = 0;
};

}  // namespace server
