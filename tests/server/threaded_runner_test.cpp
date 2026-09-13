#include "server/threaded_runner.h"

#include <cstdint>
#include <memory>

#include <gtest/gtest.h>

#include "net/loopback.h"
#include "server/mutex_ring.h"
#include "server/server.h"
#include "sim/sim.h"

namespace server {
namespace {

constexpr net::Endpoint kServerEp{0x7F000001u, 0x2100u};

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
}

}  // namespace
}  // namespace server
