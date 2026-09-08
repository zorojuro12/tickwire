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

}  // namespace
}  // namespace sim
