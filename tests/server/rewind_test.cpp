#include "server/rewind.h"

#include <cstring>
#include <memory>

#include <gtest/gtest.h>

#include "net/snapshot_ring.h"
#include "net/transport.h"
#include "server/session.h"
#include "sim/sim.h"
#include "sim/world.h"

namespace server {
namespace {

net::Endpoint kEp1{0x7F000001u, 0x1001};
net::Endpoint kEp2{0x7F000001u, 0x1002};
net::Endpoint kEp3{0x7F000001u, 0x1003};

const sim::PlayerState* findById(const sim::WorldSnapshot& s, uint32_t id) {
  for (uint32_t i = 0; i < s.count; ++i) {
    if (s.players[i].id == id) return &s.players[i];
  }
  return nullptr;
}

struct Fixture {
  std::unique_ptr<sim::World> live = std::make_unique<sim::World>();
  std::unique_ptr<net::SnapshotRing> history = std::make_unique<net::SnapshotRing>();
  std::unique_ptr<SessionTable> sessions = std::make_unique<SessionTable>();

  Fixture() {
    live->addPlayer(1, -5.0f, 0.0f);
    live->addPlayer(2, 20.0f, 0.0f);
    live->addPlayer(3, 0.0f, 30.0f);

    sim::WorldSnapshot s90{};
    s90.tick = 90;
    s90.count = 2;
    s90.players[0] = {.id = 1, .x = -10.0f, .y = 0.0f, .vx = 0.0f, .vy = 0.0f, .radius = sim::kPlayerRadius};
    s90.players[1] = {.id = 2, .x = 0.0f, .y = 0.0f, .vx = 0.0f, .vy = 0.0f, .radius = sim::kPlayerRadius};
    history->store(s90);

    sim::WorldSnapshot s93{};
    s93.tick = 93;
    s93.count = 2;
    s93.players[0] = {.id = 1, .x = -10.0f, .y = 0.0f, .vx = 0.0f, .vy = 0.0f, .radius = sim::kPlayerRadius};
    s93.players[1] = {.id = 2, .x = 3.0f, .y = -3.0f, .vx = 0.0f, .vy = 0.0f, .radius = sim::kPlayerRadius};
    history->store(s93);

    sessions->joinOrGet(kEp1, 0);
    sessions->joinOrGet(kEp2, 0);
    sessions->joinOrGet(kEp3, 0);
    sessions->noteSnapshotAck(kEp1, 93);
  }
};

TEST(RewindTest, PlacesTargetsAtTheSampledPositionAndTheShooterLive) {
  Fixture f;

  sim::WorldSnapshot out{};
  const RewindRequest req{.shooter = 1, .view_tick = 91, .fire_tick = 100};
  ASSERT_TRUE(buildRewoundView(*f.live, *f.history, *f.sessions, req, out));
  EXPECT_EQ(out.tick, 91u);
  EXPECT_EQ(out.count, 2u);

  const sim::PlayerState* shooter = findById(out, 1);
  ASSERT_NE(shooter, nullptr);
  EXPECT_EQ(shooter->x, -5.0f);
  EXPECT_EQ(shooter->y, 0.0f);
  EXPECT_EQ(shooter->radius, sim::kPlayerRadius);

  const sim::PlayerState* target = findById(out, 2);
  ASSERT_NE(target, nullptr);
  EXPECT_EQ(target->radius, sim::kPlayerRadius);
  float expected_x = 0.0f, expected_y = 0.0f;
  ASSERT_TRUE(net::samplePlayerAt(*f.history, 2, 91, expected_x, expected_y));
  EXPECT_EQ(std::memcmp(&target->x, &expected_x, sizeof(float)), 0);
  EXPECT_EQ(std::memcmp(&target->y, &expected_y, sizeof(float)), 0);

  EXPECT_EQ(findById(out, 3), nullptr);
}

}  // namespace
}  // namespace server
