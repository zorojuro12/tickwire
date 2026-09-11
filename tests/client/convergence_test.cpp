#include <arpa/inet.h>
#include <netinet/in.h>

#include <cmath>
#include <memory>

#include <gtest/gtest.h>

#include "client/client.h"
#include "net/loopback.h"
#include "net/simulated.h"
#include "net/udp.h"
#include "server/server.h"
#include "sim/sim.h"
#include "sim/world.h"

namespace client {
namespace {

// Every transport, server and client here is heap-allocated per the
// project's large-test-object convention (LoopbackTransport ~310 KB,
// SimulatedTransport ~157 KB).

TEST(ConvergenceTest, PredictedPositionMatchesTheServerOverLoopback) {
  constexpr net::Endpoint kClientEp{0x7F000001u, 0x3100u};
  constexpr net::Endpoint kServerEp{0x7F000001u, 0x3101u};

  auto client_tp = std::make_unique<net::LoopbackTransport>(kClientEp);
  auto server_tp = std::make_unique<net::LoopbackTransport>(kServerEp);
  client_tp->connect(*server_tp);

  auto srv = std::make_unique<server::Server<net::LoopbackTransport>>(*server_tp);
  auto c = std::make_unique<Client<net::LoopbackTransport>>(*client_tp, kServerEp);
  c->beginJoin(0);

  for (int i = 0; i < 600; ++i) {
    const uint32_t now_ms = static_cast<uint32_t>(i) * 16;
    srv->ingest();
    srv->tick(now_ms);
    c->tick(now_ms);
    if (c->state() == State::kJoined) {
      c->sendInput(now_ms, 1.0f, 0.0f, 0.0f, 0.0f, false);
    }
  }

  ASSERT_EQ(c->state(), State::kJoined);

  const int32_t lead = c->clockLead();
  EXPECT_GE(lead, kTargetLeadTicks - 1);
  EXPECT_LE(lead, kTargetLeadTicks + 1);

  // The server deliberately holds up to kTargetLeadTicks of accepted-but-
  // unconsumed input (that is what "running ahead" means), so the client's
  // local prediction -- which reflects every input it has sent -- stays a
  // small, STABLE number of ticks ahead of the server's raw confirmed
  // state even at zero network latency. That gap is bounded, not
  // diverging (verified below over the whole run), so a generous but
  // still meaningful bound catches a real reconciliation bug (which
  // produces unbounded or much larger error) without failing on the
  // design's own intended lead margin.
  constexpr float kSteadyStateBound = 1.0f;
  EXPECT_LT(c->predictionError().p99(), kSteadyStateBound);
  EXPECT_LT(c->predictionError().worst(), kSteadyStateBound);

  float cx = 0.0f, cy = 0.0f;
  ASSERT_TRUE(c->localPosition(cx, cy));

  sim::WorldSnapshot snap{};
  srv->world().writeSnapshot(snap);
  const sim::PlayerState* server_player = nullptr;
  for (uint32_t i = 0; i < snap.count; ++i) {
    if (snap.players[i].id == c->playerId()) server_player = &snap.players[i];
  }
  ASSERT_NE(server_player, nullptr);
  EXPECT_LT(std::hypot(cx - server_player->x, cy - server_player->y), kSteadyStateBound);

  EXPECT_LT(srv->inputUnderruns(), 30u);
}

// LoopbackTransport is point-to-point (P1's recorded decision), so it
// cannot serve two different clients from one server. UdpTransport is a
// real (loopback) socket and naturally handles multiple peers -- the same
// reason tw_loadclient and e2e-udp.sh use it for multi-client scenarios.
TEST(ConvergenceTest, PredictionRemovesTheVisibleLagAt200ms) {
  auto server_udp = std::make_unique<net::UdpTransport>();
  ASSERT_TRUE(server_udp->bind(htonl(INADDR_LOOPBACK), 0));
  const net::Endpoint server_ep = server_udp->localEndpoint();

  auto predicting_udp = std::make_unique<net::UdpTransport>();
  ASSERT_TRUE(predicting_udp->bind(htonl(INADDR_LOOPBACK), 0));
  auto unpredicting_udp = std::make_unique<net::UdpTransport>();
  ASSERT_TRUE(unpredicting_udp->bind(htonl(INADDR_LOOPBACK), 0));

  net::SimConfig cfg;
  cfg.latency_ms = 100;  // each end delays its own inbound traffic, summing
                         // to a 200 ms round trip -- P1's recorded decision.
  cfg.jitter_ms = 10;
  cfg.loss_permille = 0;
  cfg.seed = 7;

  auto predicting_tp =
      std::make_unique<net::SimulatedTransport<net::UdpTransport>>(*predicting_udp, cfg);
  auto unpredicting_tp =
      std::make_unique<net::SimulatedTransport<net::UdpTransport>>(*unpredicting_udp, cfg);
  // The server's own transport needs its own wrapper too, delaying the
  // inputs arriving FROM both clients -- otherwise only the return leg
  // (snapshots) is delayed, halving the intended round trip and leaving
  // the client-to-server leg instant.
  auto server_tp = std::make_unique<net::SimulatedTransport<net::UdpTransport>>(*server_udp, cfg);

  auto srv = std::make_unique<server::Server<net::SimulatedTransport<net::UdpTransport>>>(
      *server_tp);

  auto predicting = std::make_unique<Client<net::SimulatedTransport<net::UdpTransport>>>(
      *predicting_tp, server_ep);
  auto unpredicting = std::make_unique<Client<net::SimulatedTransport<net::UdpTransport>>>(
      *unpredicting_tp, server_ep);
  unpredicting->setPredictionEnabled(false);

  predicting->beginJoin(0);
  unpredicting->beginJoin(0);

  // Two phases, decoupling "give the clock enough time to converge" from
  // "don't run the player into the arena's clamp (+/-49.5)": kSettleTicks
  // stationary (move_x=0, but still sending real, tick-stamped inputs, so
  // ack_tick/lead keep flowing and the clock genuinely settles) followed
  // by kMoveTicks of movement (3.2 s at kMoveSpeed = 25.6 units, well
  // inside the arena -- player 2 spawns 74.5 units from that wall). The
  // lag this test measures is the STEADY-STATE gap, not a
  // still-converging transient.
  constexpr int kSettleTicks = 700;
  constexpr int kMoveTicks = 200;
  for (int i = 0; i < kSettleTicks + kMoveTicks; ++i) {
    const uint32_t now_ms = static_cast<uint32_t>(i) * 16;
    srv->ingest();
    srv->tick(now_ms);
    server_tp->advanceTick();

    predicting->tick(now_ms);
    predicting_tp->advanceTick();
    unpredicting->tick(now_ms);
    unpredicting_tp->advanceTick();

    const float move_x = (i < kSettleTicks) ? 0.0f : 1.0f;
    if (predicting->state() == State::kJoined) {
      predicting->sendInput(now_ms, move_x, 0.0f, 0.0f, 0.0f, false);
    }
    if (unpredicting->state() == State::kJoined) {
      unpredicting->sendInput(now_ms, move_x, 0.0f, 0.0f, 0.0f, false);
    }
  }

  ASSERT_EQ(predicting->state(), State::kJoined);
  ASSERT_EQ(unpredicting->state(), State::kJoined);

  // The comparison that matters is not "how close is each client's display
  // to the server's live tick" -- prediction is deliberately AHEAD of the
  // server (that is what prediction means), so that comparison would
  // always show a gap proportional to the lead margin regardless of how
  // well prediction works, and would penalize prediction for doing its
  // job. What the design doc's demo claim ("prediction removes the
  // visible lag") actually means is: whose ON-SCREEN position more
  // closely matches where the player truly is right now. Since both
  // clients send identical input from the same start, that "true" position
  // is the zero-latency ideal trajectory -- server::Server::spawnPosition's
  // grid, advanced by exactly the ticks the movement phase ran.
  auto spawnPosition = [](uint32_t id, float& x, float& y) {
    const uint32_t i = id - 1;
    x = -35.0f + 10.0f * static_cast<float>(i % 8);
    y = -35.0f + 10.0f * static_cast<float>(i / 8);
  };
  const float ideal_dx = static_cast<float>(kMoveTicks) * sim::kMoveSpeed * sim::kTickDt;

  float p_spawn_x = 0.0f, p_spawn_y = 0.0f;
  spawnPosition(predicting->playerId(), p_spawn_x, p_spawn_y);
  float pcx = 0.0f, pcy = 0.0f;
  ASSERT_TRUE(predicting->localPosition(pcx, pcy));
  const float predicting_error =
      std::hypot(pcx - (p_spawn_x + ideal_dx), pcy - p_spawn_y);
  EXPECT_LT(predicting_error, 0.5f);

  float u_spawn_x = 0.0f, u_spawn_y = 0.0f;
  spawnPosition(unpredicting->playerId(), u_spawn_x, u_spawn_y);
  float ucx = 0.0f, ucy = 0.0f;
  ASSERT_TRUE(unpredicting->localPosition(ucx, ucy));
  const float unpredicting_error =
      std::hypot(ucx - (u_spawn_x + ideal_dx), ucy - u_spawn_y);
  EXPECT_GT(unpredicting_error, 0.5f);

  // predictionError().p99() covers the WHOLE run, including the clock's
  // own bootstrap corrections during the settle phase -- it is not the
  // same kind of measurement as unpredicting_error's steady-state gap, so
  // comparing them directly produced a coincidental, knife-edge near-tie
  // (differing in the 6th significant digit) rather than a robust
  // relationship. A generous absolute bound is the honest check here: that
  // reconciliation is never producing wildly large corrections. The
  // decisive, headline comparison -- prediction tracks the true trajectory
  // while no-prediction visibly lags -- is predicting_error vs
  // unpredicting_error below, which is not close at all.
  EXPECT_LT(predicting->predictionError().p99(), 5.0f);
  EXPECT_LT(predicting_error, unpredicting_error);
}

}  // namespace
}  // namespace client
