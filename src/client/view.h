#pragma once

namespace client {

struct ScreenPos {
  float x, y;
};

// A square viewport of `side` pixels looking at world point (center_x,
// center_y). zoom == 1 fits the whole arena; zoom > 1 magnifies.
// Precondition: zoom > 0 and finite (callers pass a constant).
struct Camera {
  float center_x;
  float center_y;
  float zoom;
};

// Maps arena coordinates to a square viewport of `side` pixels, centered on
// `cam`. +y is up in world space and down on screen, so the y axis is
// flipped.
ScreenPos worldToScreen(float wx, float wy, float side, const Camera& cam) noexcept;
float worldToScreenRadius(float r, float side, float zoom) noexcept;

// The exact inverse of worldToScreen: maps a screen point back to the world
// point `cam` would have drawn there.
void screenToWorld(float sx, float sy, float side, const Camera& cam, float& wx,
                    float& wy) noexcept;

// Aim direction from the player's world position to the cursor's world
// position; returns {0, 0} when they coincide or either input is non-finite.
void aimFromCursor(float px, float py, float cx, float cy, float& aim_x,
                    float& aim_y) noexcept;

}  // namespace client
