#include "server/session.h"

#include <array>
#include <cstdint>

#include <gtest/gtest.h>

#include "net/transport.h"
#include "sim/sim.h"

namespace server {
namespace {

net::Endpoint ep(int i) {
  return net::Endpoint{0x7F000001u, static_cast<uint16_t>(0x1F90 + i)};
}

TEST(SessionTableTest, EndpointGetsOneIdKeepsItAndTheTableIsBounded) {
  SessionTable table;

  EXPECT_EQ(table.count(), 0u);
  EXPECT_EQ(table.playerFor(ep(0)), 0u);
  EXPECT_FALSE(table.authorize(ep(0), 1));

  const uint32_t id0 = table.joinOrGet(ep(0), 0);
  EXPECT_NE(id0, 0u);
  EXPECT_EQ(table.count(), 1u);
  EXPECT_EQ(table.playerFor(ep(0)), id0);

  // A retransmitted join is idempotent.
  EXPECT_EQ(table.joinOrGet(ep(0), 5), id0);
  EXPECT_EQ(table.count(), 1u);

  const uint32_t id1 = table.joinOrGet(ep(1), 0);
  EXPECT_NE(id1, 0u);
  EXPECT_NE(id1, id0);
  EXPECT_EQ(table.count(), 2u);
}

TEST(SessionTableTest, IdsAreOneBasedUniqueAndTheTableIsBoundedTo32) {
  SessionTable table;

  std::array<uint32_t, 32> ids{};
  for (int i = 0; i < 32; ++i) {
    ids[static_cast<size_t>(i)] = table.joinOrGet(ep(i), 0);
    EXPECT_GE(ids[static_cast<size_t>(i)], 1u) << "i=" << i;
    EXPECT_LE(ids[static_cast<size_t>(i)], 32u) << "i=" << i;
  }
  for (int i = 0; i < 32; ++i) {
    for (int j = i + 1; j < 32; ++j) {
      EXPECT_NE(ids[static_cast<size_t>(i)], ids[static_cast<size_t>(j)]) << i << " vs " << j;
    }
  }
  EXPECT_EQ(table.count(), 32u);

  EXPECT_EQ(table.joinOrGet(ep(32), 0), sim::kInvalidPlayerId);
  EXPECT_EQ(table.count(), 32u);
}

TEST(SessionTableTest, EndpointForAndRemoveFreeAReusableSlot) {
  SessionTable table;

  const uint32_t id0 = table.joinOrGet(ep(0), 0);

  net::Endpoint out{};
  EXPECT_TRUE(table.endpointFor(id0, out));
  EXPECT_EQ(out, ep(0));

  net::Endpoint untouched{0xDEADBEEFu, 1234};
  net::Endpoint before = untouched;
  EXPECT_FALSE(table.endpointFor(99, untouched));
  EXPECT_EQ(untouched, before);

  EXPECT_TRUE(table.remove(ep(0)));
  EXPECT_EQ(table.count(), 0u);
  EXPECT_EQ(table.playerFor(ep(0)), 0u);
  EXPECT_FALSE(table.remove(ep(0)));

  const uint32_t new_id = table.joinOrGet(ep(1), 0);
  EXPECT_EQ(new_id, id0);
}

TEST(SessionTableTest, IterationCoversExactlyTheLiveSessions) {
  SessionTable table;

  std::array<uint32_t, 32> ids{};
  for (int i = 0; i < 32; ++i) ids[static_cast<size_t>(i)] = table.joinOrGet(ep(i), 0);
  ASSERT_TRUE(table.remove(ep(0)));

  EXPECT_EQ(table.count(), 31u);
  std::array<bool, 33> seen{};
  for (size_t i = 0; i < table.count(); ++i) {
    uint32_t pid = table.playerAt(i);
    EXPECT_NE(pid, ids[0]);
    ASSERT_LE(pid, 32u);
    EXPECT_FALSE(seen[pid]) << "duplicate player " << pid;
    seen[pid] = true;

    net::Endpoint want{};
    ASSERT_TRUE(table.endpointFor(pid, want));
    EXPECT_EQ(table.endpointAt(i), want);
  }
}

}  // namespace
}  // namespace server
