#include "sim/world.h"

#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>

#include <gtest/gtest.h>

namespace sim {
namespace {

const PlayerState* findPlayer(const WorldSnapshot& s, uint32_t id) {
  for (uint32_t i = 0; i < s.count; ++i) {
    if (s.players[i].id == id) return &s.players[i];
  }
  return nullptr;
}

std::optional<PlayerState> snapshotFor(const World& w, uint32_t id) {
  WorldSnapshot snap{};
  w.writeSnapshot(snap);
  const PlayerState* p = findPlayer(snap, id);
  if (p == nullptr) return std::nullopt;
  return *p;
}

TEST(WorldTest, RosterIsBoundedIdUniqueAndVisibleInSnapshot) {
  auto world = std::make_unique<World>();

  EXPECT_EQ(world->playerCount(), 0u);
  EXPECT_EQ(world->tick(), 0u);
  EXPECT_FALSE(world->hasPlayer(1));

  EXPECT_TRUE(world->addPlayer(1, 2.0f, 3.0f));
  EXPECT_EQ(world->playerCount(), 1u);
  EXPECT_TRUE(world->hasPlayer(1));

  EXPECT_FALSE(world->addPlayer(1, 0.0f, 0.0f));
  EXPECT_EQ(world->playerCount(), 1u);

  EXPECT_FALSE(world->addPlayer(0, 0.0f, 0.0f));

  EXPECT_FALSE(world->addPlayer(2, std::numeric_limits<float>::quiet_NaN(), 0.0f));
  EXPECT_FALSE(world->addPlayer(2, 0.0f, std::numeric_limits<float>::infinity()));
  EXPECT_EQ(world->playerCount(), 1u);

  for (uint32_t id = 2; id <= 32; ++id) {
    EXPECT_TRUE(world->addPlayer(id, 0.0f, 0.0f)) << "id " << id;
  }
  EXPECT_EQ(world->playerCount(), 32u);
  EXPECT_FALSE(world->addPlayer(33, 0.0f, 0.0f));

  WorldSnapshot out{};
  world->writeSnapshot(out);
  EXPECT_EQ(out.tick, 0u);
  EXPECT_EQ(out.count, 32u);
  for (uint32_t id = 1; id <= 32; ++id) {
    const PlayerState* p = findPlayer(out, id);
    ASSERT_NE(p, nullptr) << "id " << id;
    EXPECT_EQ(p->radius, kPlayerRadius);
  }
  const PlayerState* p1 = findPlayer(out, 1);
  ASSERT_NE(p1, nullptr);
  EXPECT_EQ(p1->x, 2.0f);
  EXPECT_EQ(p1->y, 3.0f);
  EXPECT_EQ(p1->vx, 0.0f);
  EXPECT_EQ(p1->vy, 0.0f);

  EXPECT_TRUE(world->removePlayer(1));
  EXPECT_EQ(world->playerCount(), 31u);
  WorldSnapshot out2{};
  world->writeSnapshot(out2);
  EXPECT_EQ(out2.count, 31u);
  EXPECT_EQ(findPlayer(out2, 1), nullptr);
  EXPECT_FALSE(world->removePlayer(1));

  EXPECT_TRUE(world->addPlayer(33, 0.0f, 0.0f));
}

uint32_t bits(float f) {
  uint32_t b = 0;
  std::memcpy(&b, &f, sizeof(b));
  return b;
}

TEST(WorldTest, InputSetsVelocityStepIntegratesAndLatches) {
  auto world = std::make_unique<World>();
  ASSERT_TRUE(world->addPlayer(1, 0.0f, 0.0f));

  world->applyInput(InputCommand{
      .player_id = 1, .tick = 0, .move_x = 1.0f, .move_y = 0.0f, .aim_x = 0.0f,
      .aim_y = 0.0f, .fire = false});
  world->step();
  auto p1 = snapshotFor(*world, 1);
  ASSERT_TRUE(p1.has_value());
  EXPECT_EQ(p1->vx, kMoveSpeed);
  EXPECT_EQ(p1->vy, 0.0f);
  EXPECT_EQ(bits(p1->x), bits(kMoveSpeed * kTickDt));
  EXPECT_EQ(world->tick(), 1u);
  WorldSnapshot snap{};
  world->writeSnapshot(snap);
  EXPECT_EQ(snap.tick, 1u);

  // Latching: a second step with no further input keeps moving.
  world->step();
  p1 = snapshotFor(*world, 1);
  ASSERT_TRUE(p1.has_value());
  EXPECT_EQ(p1->vx, kMoveSpeed);
  EXPECT_EQ(bits(p1->x), bits(2.0f * kMoveSpeed * kTickDt));

  // Normalization: move_x=3, move_y=4 (length 5).
  auto world2 = std::make_unique<World>();
  ASSERT_TRUE(world2->addPlayer(1, 0.0f, 0.0f));
  world2->applyInput(InputCommand{
      .player_id = 1, .tick = 0, .move_x = 3.0f, .move_y = 4.0f, .aim_x = 0.0f,
      .aim_y = 0.0f, .fire = false});
  auto n1 = snapshotFor(*world2, 1);
  ASSERT_TRUE(n1.has_value());
  EXPECT_EQ(bits(n1->vx), bits(kMoveSpeed * 0.6f));
  EXPECT_EQ(bits(n1->vy), bits(kMoveSpeed * 0.8f));

  // Sub-unit input passes through unnormalized.
  auto world3 = std::make_unique<World>();
  ASSERT_TRUE(world3->addPlayer(1, 0.0f, 0.0f));
  world3->applyInput(InputCommand{
      .player_id = 1, .tick = 0, .move_x = 0.5f, .move_y = 0.0f, .aim_x = 0.0f,
      .aim_y = 0.0f, .fire = false});
  auto s1 = snapshotFor(*world3, 1);
  ASSERT_TRUE(s1.has_value());
  EXPECT_EQ(bits(s1->vx), bits(kMoveSpeed * 0.5f));

  // Zero input stops the player.
  auto world4 = std::make_unique<World>();
  ASSERT_TRUE(world4->addPlayer(1, 0.0f, 0.0f));
  world4->applyInput(InputCommand{
      .player_id = 1, .tick = 0, .move_x = 1.0f, .move_y = 0.0f, .aim_x = 0.0f,
      .aim_y = 0.0f, .fire = false});
  world4->applyInput(InputCommand{
      .player_id = 1, .tick = 0, .move_x = 0.0f, .move_y = 0.0f, .aim_x = 0.0f,
      .aim_y = 0.0f, .fire = false});
  auto z1 = snapshotFor(*world4, 1);
  ASSERT_TRUE(z1.has_value());
  EXPECT_EQ(z1->vx, 0.0f);
  EXPECT_EQ(z1->vy, 0.0f);
  float x_before = z1->x;
  world4->step();
  auto z1b = snapshotFor(*world4, 1);
  ASSERT_TRUE(z1b.has_value());
  EXPECT_EQ(z1b->x, x_before);

  // Degenerate input is treated as zero.
  auto degenerate = [](float mx, float my) {
    auto w = std::make_unique<World>();
    w->addPlayer(1, 0.0f, 0.0f);
    w->applyInput(InputCommand{.player_id = 1,
                                .tick = 0,
                                .move_x = mx,
                                .move_y = my,
                                .aim_x = 0.0f,
                                .aim_y = 0.0f,
                                .fire = false});
    auto p = snapshotFor(*w, 1);
    ASSERT_TRUE(p.has_value());
    EXPECT_EQ(p->vx, 0.0f);
    EXPECT_EQ(p->vy, 0.0f);
    w->step();
    p = snapshotFor(*w, 1);
    ASSERT_TRUE(p.has_value());
    EXPECT_TRUE(std::isfinite(p->x));
    EXPECT_TRUE(std::isfinite(p->y));
  };
  degenerate(std::numeric_limits<float>::quiet_NaN(), 0.0f);
  degenerate(std::numeric_limits<float>::infinity(), 0.0f);
  degenerate(1e30f, 1e30f);

  // Unknown player: applyInput is a no-op.
  auto world5 = std::make_unique<World>();
  ASSERT_TRUE(world5->addPlayer(1, 5.0f, 5.0f));
  world5->applyInput(InputCommand{.player_id = 99,
                                   .tick = 0,
                                   .move_x = 1.0f,
                                   .move_y = 0.0f,
                                   .aim_x = 0.0f,
                                   .aim_y = 0.0f,
                                   .fire = false});
  world5->applyInput(InputCommand{.player_id = 0,
                                   .tick = 0,
                                   .move_x = 1.0f,
                                   .move_y = 0.0f,
                                   .aim_x = 0.0f,
                                   .aim_y = 0.0f,
                                   .fire = false});
  EXPECT_EQ(world5->playerCount(), 1u);
  auto u1 = snapshotFor(*world5, 1);
  ASSERT_TRUE(u1.has_value());
  EXPECT_EQ(u1->x, 5.0f);
  EXPECT_EQ(u1->y, 5.0f);
  EXPECT_EQ(world5->tick(), 0u);

  // step() on an empty world increments tick and does nothing else.
  auto empty = std::make_unique<World>();
  empty->step();
  EXPECT_EQ(empty->tick(), 1u);
  EXPECT_EQ(empty->playerCount(), 0u);
}

TEST(WorldTest, PlayersAreClampedInsideTheArenaOnEveryWall) {
  constexpr float kBound = kArenaHalf - kPlayerRadius;

  {
    auto w = std::make_unique<World>();
    ASSERT_TRUE(w->addPlayer(1, kBound - 0.01f, 0.0f));
    w->applyInput(InputCommand{.player_id = 1,
                                .tick = 0,
                                .move_x = 1.0f,
                                .move_y = 0.0f,
                                .aim_x = 0.0f,
                                .aim_y = 0.0f,
                                .fire = false});
    for (int i = 0; i < 10; ++i) w->step();
    auto p = snapshotFor(*w, 1);
    ASSERT_TRUE(p.has_value());
    EXPECT_EQ(p->x, kBound);
    EXPECT_EQ(p->vx, kMoveSpeed);
  }
  {
    auto w = std::make_unique<World>();
    ASSERT_TRUE(w->addPlayer(1, -(kBound - 0.01f), 0.0f));
    w->applyInput(InputCommand{.player_id = 1,
                                .tick = 0,
                                .move_x = -1.0f,
                                .move_y = 0.0f,
                                .aim_x = 0.0f,
                                .aim_y = 0.0f,
                                .fire = false});
    for (int i = 0; i < 10; ++i) w->step();
    auto p = snapshotFor(*w, 1);
    ASSERT_TRUE(p.has_value());
    EXPECT_EQ(p->x, -kBound);
  }
  {
    auto w = std::make_unique<World>();
    ASSERT_TRUE(w->addPlayer(1, 0.0f, kBound - 0.01f));
    w->applyInput(InputCommand{.player_id = 1,
                                .tick = 0,
                                .move_x = 0.0f,
                                .move_y = 1.0f,
                                .aim_x = 0.0f,
                                .aim_y = 0.0f,
                                .fire = false});
    for (int i = 0; i < 10; ++i) w->step();
    auto p = snapshotFor(*w, 1);
    ASSERT_TRUE(p.has_value());
    EXPECT_EQ(p->y, kBound);
  }
  {
    auto w = std::make_unique<World>();
    ASSERT_TRUE(w->addPlayer(1, 0.0f, -(kBound - 0.01f)));
    w->applyInput(InputCommand{.player_id = 1,
                                .tick = 0,
                                .move_x = 0.0f,
                                .move_y = -1.0f,
                                .aim_x = 0.0f,
                                .aim_y = 0.0f,
                                .fire = false});
    for (int i = 0; i < 10; ++i) w->step();
    auto p = snapshotFor(*w, 1);
    ASSERT_TRUE(p.has_value());
    EXPECT_EQ(p->y, -kBound);
  }
  {
    auto w = std::make_unique<World>();
    ASSERT_TRUE(w->addPlayer(1, kBound - 0.01f, kBound - 0.01f));
    w->applyInput(InputCommand{.player_id = 1,
                                .tick = 0,
                                .move_x = 1.0f,
                                .move_y = 1.0f,
                                .aim_x = 0.0f,
                                .aim_y = 0.0f,
                                .fire = false});
    for (int i = 0; i < 200; ++i) w->step();
    auto p = snapshotFor(*w, 1);
    ASSERT_TRUE(p.has_value());
    EXPECT_EQ(p->x, kBound);
    EXPECT_EQ(p->y, kBound);
    for (int i = 0; i < 5; ++i) w->step();
    p = snapshotFor(*w, 1);
    ASSERT_TRUE(p.has_value());
    EXPECT_EQ(p->x, kBound);
    EXPECT_EQ(p->y, kBound);
  }
  {
    auto w = std::make_unique<World>();
    ASSERT_TRUE(w->addPlayer(1, kBound, 0.0f));
    w->applyInput(InputCommand{.player_id = 1,
                                .tick = 0,
                                .move_x = -1.0f,
                                .move_y = 0.0f,
                                .aim_x = 0.0f,
                                .aim_y = 0.0f,
                                .fire = false});
    w->step();
    auto p = snapshotFor(*w, 1);
    ASSERT_TRUE(p.has_value());
    EXPECT_LT(p->x, kBound);
  }
}

TEST(WorldTest, HitscanReturnsNearestTargetAlongTheRay) {
  {
    auto w = std::make_unique<World>();
    ASSERT_TRUE(w->addPlayer(1, 0.0f, 0.0f));
    ASSERT_TRUE(w->addPlayer(2, 10.0f, 0.0f));
    ASSERT_TRUE(w->addPlayer(3, 20.0f, 0.0f));
    EXPECT_EQ(w->resolveHitscan(1, 1.0f, 0.0f), 2u);
  }
  {
    auto w = std::make_unique<World>();
    ASSERT_TRUE(w->addPlayer(1, 0.0f, 0.0f));
    ASSERT_TRUE(w->addPlayer(3, 20.0f, 0.0f));
    ASSERT_TRUE(w->addPlayer(2, 10.0f, 0.0f));
    EXPECT_EQ(w->resolveHitscan(1, 1.0f, 0.0f), 2u);
  }
  {
    auto w = std::make_unique<World>();
    ASSERT_TRUE(w->addPlayer(1, 0.0f, 0.0f));
    ASSERT_TRUE(w->addPlayer(2, 10.0f, 0.0f));
    ASSERT_TRUE(w->addPlayer(3, 20.0f, 0.0f));
    EXPECT_EQ(w->resolveHitscan(1, -1.0f, 0.0f), std::nullopt);
  }
  {
    auto w = std::make_unique<World>();
    ASSERT_TRUE(w->addPlayer(1, 0.0f, 0.0f));
    ASSERT_TRUE(w->addPlayer(2, 10.0f, 0.0f));
    EXPECT_EQ(w->resolveHitscan(1, 1.0f, 1.0f), std::nullopt);
  }
  {
    // Grazing is a hit; just past the radius is not.
    auto w = std::make_unique<World>();
    ASSERT_TRUE(w->addPlayer(1, 0.0f, 0.0f));
    ASSERT_TRUE(w->addPlayer(2, 10.0f, 0.4f));
    EXPECT_EQ(w->resolveHitscan(1, 1.0f, 0.0f), 2u);
  }
  {
    auto w = std::make_unique<World>();
    ASSERT_TRUE(w->addPlayer(1, 0.0f, 0.0f));
    ASSERT_TRUE(w->addPlayer(2, 10.0f, 0.6f));
    EXPECT_EQ(w->resolveHitscan(1, 1.0f, 0.0f), std::nullopt);
  }
  {
    // Aim need not be normalized.
    auto w = std::make_unique<World>();
    ASSERT_TRUE(w->addPlayer(1, 0.0f, 0.0f));
    ASSERT_TRUE(w->addPlayer(2, 10.0f, 0.0f));
    EXPECT_EQ(w->resolveHitscan(1, 7.5f, 0.0f), 2u);
  }
  {
    // The shooter never hits itself.
    auto w = std::make_unique<World>();
    ASSERT_TRUE(w->addPlayer(1, 0.0f, 0.0f));
    ASSERT_TRUE(w->addPlayer(2, 10.0f, 0.0f));
    EXPECT_EQ(w->resolveHitscan(1, 1.0f, 0.0f), 2u);
    ASSERT_TRUE(w->removePlayer(2));
    EXPECT_EQ(w->resolveHitscan(1, 1.0f, 0.0f), std::nullopt);
  }
  {
    // Degenerate inputs return nullopt and must not crash.
    auto w = std::make_unique<World>();
    ASSERT_TRUE(w->addPlayer(1, 0.0f, 0.0f));
    EXPECT_EQ(w->resolveHitscan(1, 0.0f, 0.0f), std::nullopt);
    EXPECT_EQ(w->resolveHitscan(1, std::numeric_limits<float>::quiet_NaN(), 0.0f),
              std::nullopt);
    EXPECT_EQ(w->resolveHitscan(1, std::numeric_limits<float>::infinity(), 0.0f),
              std::nullopt);
    EXPECT_EQ(w->resolveHitscan(99, 1.0f, 0.0f), std::nullopt);
    EXPECT_EQ(w->resolveHitscan(0, 1.0f, 0.0f), std::nullopt);
  }
  {
    // A target behind the shooter is never hit, even when overlapping.
    auto w = std::make_unique<World>();
    ASSERT_TRUE(w->addPlayer(1, 0.0f, 0.0f));
    ASSERT_TRUE(w->addPlayer(2, -0.1f, 0.0f));
    EXPECT_EQ(w->resolveHitscan(1, 1.0f, 0.0f), std::nullopt);
  }
}

TEST(WorldTest, HitscanDistanceTiesResolveToTheLowerPlayerId) {
  {
    auto w = std::make_unique<World>();
    ASSERT_TRUE(w->addPlayer(1, 0.0f, 0.0f));
    ASSERT_TRUE(w->addPlayer(7, 10.0f, 0.3f));
    ASSERT_TRUE(w->addPlayer(3, 10.0f, -0.3f));
    EXPECT_EQ(w->resolveHitscan(1, 1.0f, 0.0f), 3u);
  }
  {
    auto w = std::make_unique<World>();
    ASSERT_TRUE(w->addPlayer(1, 0.0f, 0.0f));
    ASSERT_TRUE(w->addPlayer(3, 10.0f, -0.3f));
    ASSERT_TRUE(w->addPlayer(7, 10.0f, 0.3f));
    EXPECT_EQ(w->resolveHitscan(1, 1.0f, 0.0f), 3u);
  }
}

TEST(WorldTest, SetPlayerStateOverwritesPositionAndVelocity) {
  auto w = std::make_unique<World>();
  ASSERT_TRUE(w->addPlayer(1, 0.0f, 0.0f));

  EXPECT_TRUE(w->setPlayerState(
      PlayerState{1, 10.0f, -10.0f, sim::kMoveSpeed, 0.0f, sim::kPlayerRadius}));

  auto p = snapshotFor(*w, 1);
  ASSERT_TRUE(p.has_value());
  EXPECT_EQ(p->x, 10.0f);
  EXPECT_EQ(p->y, -10.0f);
  EXPECT_EQ(p->vx, sim::kMoveSpeed);
  EXPECT_EQ(p->vy, 0.0f);

  w->step();
  auto after = snapshotFor(*w, 1);
  ASSERT_TRUE(after.has_value());
  EXPECT_EQ(after->x, 10.0f + sim::kMoveSpeed * sim::kTickDt);
}

TEST(WorldTest, SetPlayerStateRejectsUnknownIdsAndNonFiniteFields) {
  auto w = std::make_unique<World>();
  ASSERT_TRUE(w->addPlayer(7, 3.0f, 4.0f));

  auto assertUnchanged = [&] {
    auto p = snapshotFor(*w, 7);
    ASSERT_TRUE(p.has_value());
    EXPECT_EQ(p->x, 3.0f);
    EXPECT_EQ(p->y, 4.0f);
    EXPECT_EQ(p->vx, 0.0f);
    EXPECT_EQ(p->vy, 0.0f);
  };

  const float nan = std::nanf("");
  const float inf = std::numeric_limits<float>::infinity();

  EXPECT_FALSE(w->setPlayerState(PlayerState{8, 1, 1, 1, 1, sim::kPlayerRadius}));
  assertUnchanged();
  EXPECT_FALSE(w->setPlayerState(PlayerState{0, 1, 1, 1, 1, sim::kPlayerRadius}));
  assertUnchanged();
  EXPECT_FALSE(w->setPlayerState(PlayerState{7, nan, 1, 1, 1, sim::kPlayerRadius}));
  assertUnchanged();
  EXPECT_FALSE(w->setPlayerState(PlayerState{7, 1, inf, 1, 1, sim::kPlayerRadius}));
  assertUnchanged();
  EXPECT_FALSE(w->setPlayerState(PlayerState{7, 1, 1, nan, 1, sim::kPlayerRadius}));
  assertUnchanged();
  EXPECT_FALSE(w->setPlayerState(PlayerState{7, 1, 1, 1, -inf, sim::kPlayerRadius}));
  assertUnchanged();
  EXPECT_FALSE(w->setPlayerState(PlayerState{7, 1, 1, 1, 1, nan}));
  assertUnchanged();
}

}  // namespace
}  // namespace sim
