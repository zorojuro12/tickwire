#pragma once

namespace sim {

inline constexpr float kTickDt = 1.0f / 60.0f;

float advance(float pos, float vel);

}  // namespace sim
