#include "client/view.h"

#include <cmath>

#include "sim/sim.h"

namespace client {

ScreenPos worldToScreen(float wx, float wy, float side, const Camera& cam) noexcept {
  const float scale = side / (2.0f * sim::kArenaHalf) * cam.zoom;
  return ScreenPos{side / 2.0f + (wx - cam.center_x) * scale,
                    side / 2.0f - (wy - cam.center_y) * scale};
}

float worldToScreenRadius(float r, float side, float zoom) noexcept {
  const float scale = side / (2.0f * sim::kArenaHalf) * zoom;
  return r * scale;
}

void screenToWorld(float sx, float sy, float side, const Camera& cam, float& wx,
                    float& wy) noexcept {
  const float scale = side / (2.0f * sim::kArenaHalf) * cam.zoom;
  wx = cam.center_x + (sx - side / 2.0f) / scale;
  wy = cam.center_y - (sy - side / 2.0f) / scale;
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
