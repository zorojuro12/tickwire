#include "net/loopback.h"

#include <array>
#include <cstring>
#include <memory>

#include <gtest/gtest.h>

namespace net {
namespace {

constexpr Endpoint kEndpointA{0x7F000001u, 0x1F90u};
constexpr Endpoint kEndpointB{0x7F000001u, 0x1F91u};

TEST(LoopbackTransportTest, DeliversPacketIntactWithSenderEndpoint) {
  auto a = std::make_unique<LoopbackTransport>(kEndpointA);
  auto b = std::make_unique<LoopbackTransport>(kEndpointB);
  a->connect(*b);

  PacketSlot slot;
  EXPECT_FALSE(b->tryReceive(slot));
  EXPECT_EQ(slot.len, 0u);

  const std::array<std::byte, 5> payload = {std::byte{0x01}, std::byte{0x02}, std::byte{0x03},
                                             std::byte{0x04}, std::byte{0x05}};
  EXPECT_TRUE(a->send(b->self(), payload));

  ASSERT_TRUE(b->tryReceive(slot));
  EXPECT_EQ(slot.len, 5u);
  EXPECT_EQ(std::memcmp(slot.data.data(), payload.data(), payload.size()), 0);
  EXPECT_EQ(slot.peer, a->self());

  EXPECT_FALSE(b->tryReceive(slot));

  EXPECT_TRUE(b->send(a->self(), payload));
  PacketSlot slot_a;
  ASSERT_TRUE(a->tryReceive(slot_a));
  EXPECT_EQ(slot_a.peer, b->self());
}

}  // namespace
}  // namespace net
