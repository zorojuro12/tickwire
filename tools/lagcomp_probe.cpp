#include <arpa/inet.h>
#include <netinet/in.h>

#include <cstdint>
#include <cstdio>
#include <memory>
#include <random>

#include "client/client.h"
#include "client/view.h"
#include "net/simulated.h"
#include "net/udp.h"
#include "server/server.h"

namespace {

using Transport = net::SimulatedTransport<net::UdpTransport>;

struct Topology {
  uint32_t shooter_latency_ms;
  uint32_t shooter_jitter_ms;
  uint32_t shooter_seed;
  uint32_t other_latency_ms;
  uint32_t other_jitter_ms;
  uint32_t other_seed;
};

// "test" matches lagcomp_hitrate_test.cpp: both legs delayed 100ms +/-10ms
// jitter. "demo" matches the real demo: only the shooter (tw_client) carries
// a SimulatedTransport wrapper (200ms, no jitter); the server and the target
// bot (tw_server/tw_loadclient) run undelayed, real UdpTransport traffic.
constexpr Topology kTestTopology{100, 10, 7, 100, 10, 7};
constexpr Topology kDemoTopology{200, 0, 1, 0, 0, 7};

struct ArmResult {
  bool shooter_joined = false;
  bool target_joined = false;
  uint64_t shots = 0;
  uint64_t hits = 0;
};

// The lockstep harness from lagcomp_hitrate_test.cpp's runArm, generalized
// over topology, target sweep period/axis, and simulated human aim error.
ArmResult runArm(const Topology& topo, uint32_t reverse_ticks, char axis, float aim_sigma,
                  bool lag_comp) {
  auto shooter_udp = std::make_unique<net::UdpTransport>();
  shooter_udp->bind(htonl(INADDR_LOOPBACK), 0);
  auto target_udp = std::make_unique<net::UdpTransport>();
  target_udp->bind(htonl(INADDR_LOOPBACK), 0);
  auto server_udp = std::make_unique<net::UdpTransport>();
  server_udp->bind(htonl(INADDR_LOOPBACK), 0);
  const net::Endpoint server_ep = server_udp->localEndpoint();
  net::SimConfig shooter_cfg;
  shooter_cfg.latency_ms = topo.shooter_latency_ms;
  shooter_cfg.jitter_ms = topo.shooter_jitter_ms;
  shooter_cfg.loss_permille = 0;
  shooter_cfg.seed = topo.shooter_seed;
  net::SimConfig other_cfg;
  other_cfg.latency_ms = topo.other_latency_ms;
  other_cfg.jitter_ms = topo.other_jitter_ms;
  other_cfg.loss_permille = 0;
  other_cfg.seed = topo.other_seed;
  auto shooter_tp = std::make_unique<Transport>(*shooter_udp, shooter_cfg);
  auto target_tp = std::make_unique<Transport>(*target_udp, other_cfg);
  auto server_tp = std::make_unique<Transport>(*server_udp, other_cfg);
  auto srv = std::make_unique<server::Server<Transport>>(*server_tp);
  auto shooter = std::make_unique<client::Client<Transport>>(*shooter_tp, server_ep);
  auto target = std::make_unique<client::Client<Transport>>(*target_tp, server_ep);
  shooter->setLagCompensationEnabled(lag_comp);
  shooter->beginJoin(0);
  target->beginJoin(0);

  constexpr int kSettleTicks = 700;
  constexpr int kShootTicks = 1800;
  constexpr uint32_t kShooterMoveTicks = 60;
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
    if (shooter->state() == client::State::kJoined) {
      const float move_y = shooter_joined_ticks < kShooterMoveTicks ? 1.0f : 0.0f;
      ++shooter_joined_ticks;
      shooter->sendInput(now_ms, 0.0f, move_y, 0.0f, 0.0f, false);
    }
    if (target->state() == client::State::kJoined) {
      target->sendInput(now_ms, 0.0f, 0.0f, 0.0f, 0.0f, false);
    }
  }

  ArmResult result;
  result.shooter_joined = shooter->state() == client::State::kJoined;
  result.target_joined = target->state() == client::State::kJoined;
  std::mt19937 aim_rng(42);
  std::normal_distribution<float> aim_noise(0.0f, aim_sigma);

  for (int i = 0; i < kShootTicks; ++i) {
    now_ms = static_cast<uint32_t>(kSettleTicks + i) * 16;
    srv->ingest();
    srv->tick(now_ms);
    server_tp->advanceTick();
    shooter->tick(now_ms);
    shooter_tp->advanceTick();
    target->tick(now_ms);
    target_tp->advanceTick();
    if (target->state() == client::State::kJoined) {
      const float sign = (i / static_cast<int>(reverse_ticks)) % 2 == 0 ? 1.0f : -1.0f;
      const float move_x = axis == 'x' ? sign : 0.0f;
      const float move_y = axis == 'y' ? sign : 0.0f;
      target->sendInput(now_ms, move_x, move_y, 0.0f, 0.0f, false);
    }
    if (shooter->state() != client::State::kJoined) continue;

    float local_x = 0.0f, local_y = 0.0f, drawn_x = 0.0f, drawn_y = 0.0f;
    const bool have_local = shooter->localPosition(local_x, local_y);
    const bool have_drawn = shooter->remotePosition(target->playerId(), drawn_x, drawn_y);
    if (have_local && have_drawn) {
      float aim_at_x = drawn_x;
      float aim_at_y = drawn_y;
      if (aim_sigma > 0.0f) {
        aim_at_x += aim_noise(aim_rng);
        aim_at_y += aim_noise(aim_rng);
      }
      float aim_x = 0.0f, aim_y = 0.0f;
      client::aimFromCursor(local_x, local_y, aim_at_x, aim_at_y, aim_x, aim_y);
      shooter->sendInput(now_ms, 0.0f, 0.0f, aim_x, aim_y, true);
    } else {
      shooter->sendInput(now_ms, 0.0f, 0.0f, 0.0f, 0.0f, false);
    }
  }

  const uint32_t shooter_id = shooter->playerId();
  result.shots = srv->shots(shooter_id);
  result.hits = srv->hits(shooter_id);
  return result;
}

struct Row {
  const char* topology;
  bool demo;
  uint32_t reverse_ticks;
  char axis;
  float aim_sigma;
};

}  // namespace

int main(int argc, char**) {
  if (argc != 1) {
    std::fprintf(stderr, "usage: lagcomp_probe\n");
    return 1;
  }

  // aim_sigma models human tracking error: pixels of hand error / (8 * zoom)
  // = world units (8 px/world-unit at zoom 1, so at zoom 4 a 32 px error is
  // sigma 1.0).
  const Row rows[] = {
      {"test", false, 120, 'y', 0.0f}, {"demo", true, 120, 'y', 0.0f},
      {"test", false, 30, 'y', 0.0f},  {"demo", true, 30, 'x', 0.0f},
      {"demo", true, 30, 'x', 0.25f},  {"demo", true, 30, 'x', 0.5f},
      {"demo", true, 30, 'x', 1.0f},   {"demo", true, 120, 'x', 0.0f},
      {"demo", true, 120, 'x', 0.25f}, {"demo", true, 120, 'x', 0.5f},
      {"demo", true, 120, 'x', 1.0f},
  };
  bool any_join_failed = false;

  for (const Row& row : rows) {
    const Topology& topo = row.demo ? kDemoTopology : kTestTopology;
    const ArmResult on = runArm(topo, row.reverse_ticks, row.axis, row.aim_sigma, true);
    const ArmResult off = runArm(topo, row.reverse_ticks, row.axis, row.aim_sigma, false);
    if (!on.shooter_joined || !on.target_joined || !off.shooter_joined || !off.target_joined) {
      any_join_failed = true;
    }
    const double on_rate =
        on.shots == 0 ? 0.0 : static_cast<double>(on.hits) / static_cast<double>(on.shots);
    const double off_rate =
        off.shots == 0 ? 0.0 : static_cast<double>(off.hits) / static_cast<double>(off.shots);
    std::printf(
        "topology=%s reverse_ticks=%u axis=%c aim_sigma=%.2f on=%.3f on_hits=%llu/%llu "
        "off=%.3f off_hits=%llu/%llu gap=%+.3f\n",
        row.topology, row.reverse_ticks, row.axis, row.aim_sigma, on_rate,
        static_cast<unsigned long long>(on.hits), static_cast<unsigned long long>(on.shots),
        off_rate, static_cast<unsigned long long>(off.hits),
        static_cast<unsigned long long>(off.shots), on_rate - off_rate);
    std::fflush(stdout);
  }

  return any_join_failed ? 1 : 0;
}
