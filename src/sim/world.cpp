#include "sim/world.h"

#include <cmath>

namespace sim {

int World::findSlot(uint32_t id) const noexcept {
  if (id == kInvalidPlayerId) return -1;
  for (uint32_t i = 0; i < kMaxPlayers; ++i) {
    if (occupied_[i] && players_[i].id == id) return static_cast<int>(i);
  }
  return -1;
}

bool World::addPlayer(uint32_t id, float x, float y) {
  if (id == kInvalidPlayerId) return false;
  if (!std::isfinite(x) || !std::isfinite(y)) return false;
  if (findSlot(id) >= 0) return false;
  if (count_ == kMaxPlayers) return false;

  for (uint32_t i = 0; i < kMaxPlayers; ++i) {
    if (!occupied_[i]) {
      occupied_[i] = true;
      players_[i] = PlayerState{id, x, y, 0.0f, 0.0f, kPlayerRadius};
      latched_[i] = InputCommand{};
      ++count_;
      return true;
    }
  }
  return false;
}

bool World::removePlayer(uint32_t id) {
  int slot = findSlot(id);
  if (slot < 0) return false;
  occupied_[static_cast<uint32_t>(slot)] = false;
  --count_;
  return true;
}

bool World::hasPlayer(uint32_t id) const noexcept { return findSlot(id) >= 0; }

uint32_t World::playerCount() const noexcept { return count_; }

void World::writeSnapshot(WorldSnapshot& out) const {
  out.tick = tick_;
  out.count = count_;
  uint32_t written = 0;
  for (uint32_t i = 0; i < kMaxPlayers && written < count_; ++i) {
    if (occupied_[i]) {
      out.players[written] = players_[i];
      ++written;
    }
  }
}

uint32_t World::tick() const noexcept { return tick_; }

}  // namespace sim
