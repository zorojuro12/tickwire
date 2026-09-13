#include <arpa/inet.h>
#include <netinet/in.h>

#include <algorithm>
#include <cstdio>
#include <memory>

#include <gtest/gtest.h>

#include "client/client.h"
#include "client/view.h"
#include "net/simulated.h"
#include "net/udp.h"
#include "server/server.h"
#include "sim/sim.h"

namespace client {
namespace {

using Transport = net::SimulatedTransport<net::UdpTransport>;

struct ArmResult {
  bool shooter_joined = false;
  bool target_joined = false;
  uint64_t shots = 0;
  uint64_t hits = 0;
  uint64_t rewound = 0;
  uint64_t rejected = 0;
  uint32_t max_depth = 0;
};

// Real UdpTransports on loopback, each delayed 100 ms +/- 10 ms jitter (a
// 200 ms round trip for client<->server traffic, P1's recorded delay-on-
// receive model), running server and both clients in lockstep. A moving
// target sweeps vertically past the stationary shooter's row; the shooter
// aims at wherever it currently draws the target and fires every tick its
// cooldown allows. Everything heap-allocated per the project's convention.
ArmResult runArm(bool lag_comp) {
  auto shooter_udp = std::make_unique<net::UdpTransport>();
  EXPECT_TRUE(shooter_udp->bind(htonl(INADDR_LOOPBACK), 0));
  auto target_udp = std::make_unique<net::UdpTransport>();
  EXPECT_TRUE(target_udp->bind(htonl(INADDR_LOOPBACK), 0));
  auto server_udp = std::make_unique<net::UdpTransport>();
  EXPECT_TRUE(server_udp->bind(htonl(INADDR_LOOPBACK), 0));
  const net::Endpoint server_ep = server_udp->localEndpoint();

  net::SimConfig cfg;
  cfg.latency_ms = 100;
  cfg.jitter_ms = 10;
  cfg.loss_permille = 0;
  cfg.seed = 7;

  auto shooter_tp = std::make_unique<Transport>(*shooter_udp, cfg);
  auto target_tp = std::make_unique<Transport>(*target_udp, cfg);
  auto server_tp = std::make_unique<Transport>(*server_udp, cfg);

  auto srv = std::make_unique<server::Server<Transport>>(*server_tp);
  auto shooter = std::make_unique<Client<Transport>>(*shooter_tp, server_ep);
  auto target = std::make_unique<Client<Transport>>(*target_tp, server_ep);
  shooter->setLagCompensationEnabled(lag_comp);

  shooter->beginJoin(0);
  target->beginJoin(0);

  constexpr int kSettleTicks = 700;
  constexpr int kShootTicks = 900;
  constexpr uint32_t kShooterMoveTicks = 60;  // 8 units up at kMoveSpeed

  uint32_t now_ms = 0;
  uint32_t shooter_joined_ticks = 0;

  for (int i = 0; i < kSettleTicks; ++i) {
    now_ms = static_cast<uint32_t>(i) * 16;
    srv->ingest();
    srv->tick(now_ms);
    server_tp->advanceTick();
    shooter->tick(now_ms);
    shooter_tp->advanceTick();
    target->tick(now_ms);
    target_tp->advanceTick();

    if (shooter->state() == State::kJoined) {
      const float move_y = shooter_joined_ticks < kShooterMoveTicks ? 1.0f : 0.0f;
      ++shooter_joined_ticks;
      shooter->sendInput(now_ms, 0.0f, move_y, 0.0f, 0.0f, false);
    }
    if (target->state() == State::kJoined) {
      target->sendInput(now_ms, 0.0f, 0.0f, 0.0f, 0.0f, false);
    }
  }

  ArmResult result;
  result.shooter_joined = shooter->state() == State::kJoined;
  result.target_joined = target->state() == State::kJoined;
  EXPECT_TRUE(result.shooter_joined);
  EXPECT_TRUE(result.target_joined);

  for (int i = 0; i < kShootTicks; ++i) {
    now_ms = static_cast<uint32_t>(kSettleTicks + i) * 16;
    srv->ingest();
    srv->tick(now_ms);
    server_tp->advanceTick();
    shooter->tick(now_ms);
    shooter_tp->advanceTick();
    target->tick(now_ms);
    target_tp->advanceTick();

    if (target->state() == State::kJoined) {
      const float move_y = (i / 120) % 2 == 0 ? 1.0f : -1.0f;
      target->sendInput(now_ms, 0.0f, move_y, 0.0f, 0.0f, false);
    }

    if (shooter->state() != State::kJoined) continue;

    float local_x = 0.0f, local_y = 0.0f, drawn_x = 0.0f, drawn_y = 0.0f;
    const bool have_local = shooter->localPosition(local_x, local_y);
    const bool have_drawn = shooter->remotePosition(target->playerId(), drawn_x, drawn_y);
    if (have_local && have_drawn) {
      float aim_x = 0.0f, aim_y = 0.0f;
      aimFromCursor(local_x, local_y, drawn_x, drawn_y, aim_x, aim_y);
      shooter->sendInput(now_ms, 0.0f, 0.0f, aim_x, aim_y, true);

      const uint32_t view_tick = std::min(shooter->renderTick(), shooter->latestSnapshotTick());
      const uint32_t depth = shooter->clientTick() >= view_tick ? shooter->clientTick() - view_tick : 0;
      result.max_depth = std::max(result.max_depth, depth);
    } else {
      shooter->sendInput(now_ms, 0.0f, 0.0f, 0.0f, 0.0f, false);
    }
  }

  const uint32_t shooter_id = shooter->playerId();
  result.shots = srv->shots(shooter_id);
  result.hits = srv->hits(shooter_id);
  result.rewound = srv->rewoundShots();
  result.rejected = srv->rewindsRejected();

  const double hit_rate = result.shots == 0 ? 0.0
                                             : static_cast<double>(result.hits) /
                                                   static_cast<double>(result.shots);
  std::printf("lagcomp=%s shots=%llu hits=%llu hit_rate=%.3f rewound=%llu rejected=%llu max_depth_ticks=%u\n",
              lag_comp ? "on" : "off", static_cast<unsigned long long>(result.shots),
              static_cast<unsigned long long>(result.hits), hit_rate,
              static_cast<unsigned long long>(result.rewound),
              static_cast<unsigned long long>(result.rejected), result.max_depth);

  return result;
}

TEST(LagCompHitRateTest, ShotsAtTheDrawnTargetRegisterOnlyWithCompensation) {
  const ArmResult on = runArm(true);
  const ArmResult off = runArm(false);

  EXPECT_GE(on.shots, 60u);
  EXPECT_GE(off.shots, 60u);

  const double on_rate = static_cast<double>(on.hits) / static_cast<double>(on.shots);
  const double off_rate = static_cast<double>(off.hits) / static_cast<double>(off.shots);

  EXPECT_GE(on_rate, 0.90);
  EXPECT_EQ(on.rejected, 0u);
  EXPECT_EQ(on.rewound, on.shots);
  EXPECT_LT(on.max_depth, server::kMaxRewindTicks);

  EXPECT_EQ(off.rewound, 0u);
  EXPECT_LE(off_rate, 0.50);

  EXPECT_GE(on_rate - off_rate, 0.50);
}

}  // namespace
}  // namespace client
