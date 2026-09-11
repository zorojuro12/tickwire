#pragma once

namespace client {

struct ScreenPos {
  float x, y;
};

// Maps arena coordinates ([-kArenaHalf, kArenaHalf] on both axes) to a
// square viewport of `side` pixels at origin (ox, oy). +y is up in world
// space and down on screen, so the y axis is flipped.
ScreenPos worldToScreen(float wx, float wy, float side, float ox, float oy) noexcept;
float worldToScreenRadius(float r, float side) noexcept;

// Aim direction from the player's world position to the cursor's world
// position; returns {0, 0} when they coincide or either input is non-finite.
void aimFromCursor(float px, float py, float cx, float cy, float& aim_x,
                    float& aim_y) noexcept;

}  // namespace client
