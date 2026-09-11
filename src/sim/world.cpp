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

void World::applyInput(const InputCommand& in) {
  int slot = findSlot(in.player_id);
  if (slot < 0) return;
  uint32_t i = static_cast<uint32_t>(slot);
  latched_[i] = in;

  float len2 = in.move_x * in.move_x + in.move_y * in.move_y;
  if (!std::isfinite(len2) || len2 == 0.0f) {
    players_[i].vx = 0.0f;
    players_[i].vy = 0.0f;
  } else if (len2 > 1.0f) {
    float scale = kMoveSpeed / std::sqrt(len2);
    players_[i].vx = in.move_x * scale;
    players_[i].vy = in.move_y * scale;
  } else {
    players_[i].vx = in.move_x * kMoveSpeed;
    players_[i].vy = in.move_y * kMoveSpeed;
  }
}

bool World::setPlayerState(const PlayerState& s) {
  int slot = findSlot(s.id);
  if (slot < 0) return false;
  if (!std::isfinite(s.x) || !std::isfinite(s.y) || !std::isfinite(s.vx) ||
      !std::isfinite(s.vy) || !std::isfinite(s.radius)) {
    return false;
  }
  players_[static_cast<uint32_t>(slot)] = s;
  return true;
}

void World::step() {
  constexpr float kBound = kArenaHalf - kPlayerRadius;
  for (uint32_t i = 0; i < kMaxPlayers; ++i) {
    if (!occupied_[i]) continue;
    players_[i].x = players_[i].x + players_[i].vx * kTickDt;
    players_[i].y = players_[i].y + players_[i].vy * kTickDt;
    if (players_[i].x > kBound) players_[i].x = kBound;
    if (players_[i].x < -kBound) players_[i].x = -kBound;
    if (players_[i].y > kBound) players_[i].y = kBound;
    if (players_[i].y < -kBound) players_[i].y = -kBound;
  }
  ++tick_;
}

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

std::optional<uint32_t> World::resolveHitscan(uint32_t shooter, float aim_x,
                                               float aim_y) const {
  int shooter_slot = findSlot(shooter);
  if (shooter_slot < 0) return std::nullopt;

  float aim_len2 = aim_x * aim_x + aim_y * aim_y;
  if (!std::isfinite(aim_len2) || aim_len2 == 0.0f) return std::nullopt;
  float inv_len = 1.0f / std::sqrt(aim_len2);
  float dx = aim_x * inv_len;
  float dy = aim_y * inv_len;

  const PlayerState& origin = players_[static_cast<uint32_t>(shooter_slot)];
  std::optional<uint32_t> best_id;
  float best_t = 0.0f;

  for (uint32_t i = 0; i < kMaxPlayers; ++i) {
    if (!occupied_[i]) continue;
    if (players_[i].id == shooter) continue;
    float mx = players_[i].x - origin.x;
    float my = players_[i].y - origin.y;
    float t = mx * dx + my * dy;
    if (t < 0.0f) continue;
    float perp2 = mx * mx + my * my - t * t;
    if (perp2 > kPlayerRadius * kPlayerRadius) continue;
    if (!best_id.has_value() || t < best_t ||
        (t == best_t && players_[i].id < *best_id)) {
      best_id = players_[i].id;
      best_t = t;
    }
  }
  return best_id;
}

}  // namespace sim
