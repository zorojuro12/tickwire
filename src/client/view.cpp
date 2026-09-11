#include "client/view.h"

#include <cmath>

#include "sim/sim.h"

namespace client {

ScreenPos worldToScreen(float wx, float wy, float side, float ox, float oy) noexcept {
  const float scale = side / (2.0f * sim::kArenaHalf);
  return ScreenPos{ox + (wx + sim::kArenaHalf) * scale, oy + (sim::kArenaHalf - wy) * scale};
}

float worldToScreenRadius(float r, float side) noexcept {
  const float scale = side / (2.0f * sim::kArenaHalf);
  return r * scale;
}

void aimFromCursor(float px, float py, float cx, float cy, float& aim_x,
                    float& aim_y) noexcept {
  aim_x = 0.0f;
  aim_y = 0.0f;
  if (!std::isfinite(px) || !std::isfinite(py) || !std::isfinite(cx) || !std::isfinite(cy)) {
    return;
  }
  const float dx = cx - px;
  const float dy = cy - py;
  const float len2 = dx * dx + dy * dy;
  if (!std::isfinite(len2) || len2 == 0.0f) return;
  const float inv_len = 1.0f / std::sqrt(len2);
  aim_x = dx * inv_len;
  aim_y = dy * inv_len;
}

}  // namespace client
