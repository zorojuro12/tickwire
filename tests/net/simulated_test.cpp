#include "net/simulated.h"

#include <array>
#include <cstring>
#include <memory>

#include <gtest/gtest.h>

#include "net/loopback.h"

namespace net {
namespace {

constexpr Endpoint kEndpointA{0x7F000001u, 0x1F90u};
constexpr Endpoint kEndpointB{0x7F000001u, 0x1F91u};

TEST(SimulatedTransportTest, TransparentWhenEverythingIsZeroed) {
  auto a = std::make_unique<LoopbackTransport>(kEndpointA);
  auto b = std::make_unique<LoopbackTransport>(kEndpointB);
  a->connect(*b);

  SimulatedTransport<LoopbackTransport> sim_b(*b, SimConfig{});
  EXPECT_EQ(sim_b.tick(), 0u);

  const std::array<std::byte, 5> payload = {std::byte{0x01}, std::byte{0x02}, std::byte{0x03},
                                             std::byte{0x04}, std::byte{0x05}};
  ASSERT_TRUE(a->send(b->self(), payload));

  PacketSlot slot;
  ASSERT_TRUE(sim_b.tryReceive(slot));
  EXPECT_EQ(slot.len, 5u);
  EXPECT_EQ(std::memcmp(slot.data.data(), payload.data(), payload.size()), 0);
  EXPECT_EQ(slot.peer, a->self());

  EXPECT_FALSE(sim_b.tryReceive(slot));
  EXPECT_EQ(sim_b.droppedByLoss(), 0u);
  EXPECT_EQ(sim_b.droppedByCapacity(), 0u);

  ASSERT_TRUE(sim_b.send(a->self(), payload));
  PacketSlot slot_a;
  ASSERT_TRUE(a->tryReceive(slot_a));
  EXPECT_EQ(slot_a.peer, b->self());

  EXPECT_EQ(msToTicks(0), 0u);
  EXPECT_EQ(msToTicks(16), 1u);
  EXPECT_EQ(msToTicks(100), 6u);
  EXPECT_EQ(msToTicks(200), 12u);
}

}  // namespace
}  // namespace net
