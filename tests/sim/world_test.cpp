#include "sim/world.h"

#include <cmath>
#include <limits>
#include <memory>

#include <gtest/gtest.h>

namespace sim {
namespace {

const PlayerState* findPlayer(const WorldSnapshot& s, uint32_t id) {
  for (uint32_t i = 0; i < s.count; ++i) {
    if (s.players[i].id == id) return &s.players[i];
  }
  return nullptr;
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

}  // namespace
}  // namespace sim
