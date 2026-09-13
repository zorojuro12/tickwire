#include "server/threaded_runner.h"

#include <array>
#include <cstdint>
#include <memory>
#include <span>

#include <gtest/gtest.h>

#include "net/framing.h"
#include "net/loopback.h"
#include "net/protocol.h"
#include "server/mutex_ring.h"
#include "server/server.h"
#include "sim/sim.h"

namespace server {
namespace {

constexpr net::Endpoint kServerEp{0x7F000001u, 0x2100u};
constexpr net::Endpoint kClientEp{0x7F000001u, 0x2101u};

TEST(ThreadedRunnerTest, DrivesAFullJoinTickCycleAcrossTwoThreads) {
  auto server_tp = std::make_unique<net::LoopbackTransport>(kServerEp);
  auto srv = std::make_unique<
      Server<net::LoopbackTransport, MutexRing<net::PacketSlot, kIngestCapacity>>>(*server_tp);

  uint64_t ns = 0;
  uint32_t ms = 0;
  ThreadedRunner<net::LoopbackTransport, MutexRing<net::PacketSlot, kIngestCapacity>> runner(
      *srv, *server_tp, sim::kTickHz, [&ns] { return ns += 16'666'667ull; },
      [&ms] { return ms += 16u; });

  EXPECT_TRUE(runner.run(30));
  EXPECT_EQ(runner.ticksRun(), 30u);
  EXPECT_EQ(srv->worldTick(), 30u);
  EXPECT_EQ(runner.jitter().count(), 29u);
  EXPECT_EQ(runner.jitter().maxNs(), 16'666'667u);
}

TEST(ThreadedRunnerTest, APacketSentFromThePeerReachesTheWorldAcrossTheSeam) {
  auto client_tp = std::make_unique<net::LoopbackTransport>(kClientEp);
  auto server_tp = std::make_unique<net::LoopbackTransport>(kServerEp);
  client_tp->connect(*server_tp);
  auto srv = std::make_unique<
      Server<net::LoopbackTransport, MutexRing<net::PacketSlot, kIngestCapacity>>>(*server_tp);

  net::PacketHeader h;
  h.type = net::MsgType::kJoinRequest;
  h.seq = 1;
  std::array<std::byte, net::kMaxPacket> buf{};
  const size_t written = net::framePacket(h, {}, buf);
  ASSERT_GT(written, 0u);
  ASSERT_TRUE(client_tp->send(kServerEp, std::span<const std::byte>(buf).subspan(0, written)));

  uint64_t ns = 0;
  uint32_t ms = 0;
  ThreadedRunner<net::LoopbackTransport, MutexRing<net::PacketSlot, kIngestCapacity>> runner(
      *srv, *server_tp, sim::kTickHz, [&ns] { return ns += 16'666'667ull; },
      [&ms] { return ms += 16u; });

  EXPECT_TRUE(runner.run(30));
  EXPECT_EQ(srv->sessions().count(), 1u);
  EXPECT_GE(runner.packetsIngested(), 1u);
}

}  // namespace
}  // namespace server
