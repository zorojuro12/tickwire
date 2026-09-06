#pragma once

#include <array>
#include <cstdint>

namespace sim {

inline constexpr float kTickDt = 1.0f / 60.0f;
inline constexpr uint32_t kTickHz = 60;
inline constexpr uint32_t kMaxPlayers = 32;  // derived from the ~1200 B MTU; see resolution doc Q2
static_assert(kTickDt == 1.0f / static_cast<float>(kTickHz));

float advance(float pos, float vel);

struct PlayerState {
  uint32_t id;
  float x, y, vx, vy;
  float radius;
};

struct InputCommand {
  uint32_t player_id;
  uint32_t tick;
  float move_x, move_y;
  bool fire;
};

struct WorldSnapshot {
  uint32_t tick;
  uint32_t count;
  std::array<PlayerState, kMaxPlayers> players;
};

}  // namespace sim
