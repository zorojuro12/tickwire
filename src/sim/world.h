#pragma once

#include <array>
#include <cstdint>

#include "sim/sim.h"

namespace sim {

class World {
 public:
  // False if full, id taken, id 0, or a non-finite coordinate.
  bool addPlayer(uint32_t id, float x, float y);
  bool removePlayer(uint32_t id);  // false if absent
  bool hasPlayer(uint32_t id) const noexcept;
  uint32_t playerCount() const noexcept;
  void writeSnapshot(WorldSnapshot& out) const;  // caller-owned buffer
  uint32_t tick() const noexcept;

 private:
  int findSlot(uint32_t id) const noexcept;  // -1 if not present

  std::array<PlayerState, kMaxPlayers> players_{};
  std::array<bool, kMaxPlayers> occupied_{};
  std::array<InputCommand, kMaxPlayers> latched_{};
  uint32_t count_ = 0;
  uint32_t tick_ = 0;
};

}  // namespace sim
