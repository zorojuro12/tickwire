#pragma once

#include <array>
#include <cstdint>
#include <optional>

#include "sim/sim.h"

namespace sim {

class World {
 public:
  // False if full, id taken, id 0, or a non-finite coordinate.
  bool addPlayer(uint32_t id, float x, float y);
  bool removePlayer(uint32_t id);  // false if absent
  bool hasPlayer(uint32_t id) const noexcept;
  uint32_t playerCount() const noexcept;
  void applyInput(const InputCommand& in);  // silently ignores an unknown player_id
  // Overwrites an existing player's position, velocity and radius from `s`,
  // keyed by s.id. False when s.id is kInvalidPlayerId, is not present, or
  // any of x/y/vx/vy/radius is non-finite. The stored state is untouched
  // on false -- assigned only once every check has passed.
  bool setPlayerState(const PlayerState& s);
  void step();                              // advances exactly kTickDt
  void writeSnapshot(WorldSnapshot& out) const;  // caller-owned buffer
  uint32_t tick() const noexcept;

  // Nearest player (excluding the shooter) whose circle the ray from the
  // shooter's position along (aim_x, aim_y) intersects. std::nullopt when
  // nothing is hit.
  std::optional<uint32_t> resolveHitscan(uint32_t shooter, float aim_x, float aim_y) const;

 private:
  int findSlot(uint32_t id) const noexcept;  // -1 if not present

  std::array<PlayerState, kMaxPlayers> players_{};
  std::array<bool, kMaxPlayers> occupied_{};
  std::array<InputCommand, kMaxPlayers> latched_{};
  uint32_t count_ = 0;
  uint32_t tick_ = 0;
};

}  // namespace sim
