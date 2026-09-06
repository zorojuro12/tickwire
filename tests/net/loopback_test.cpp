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

TEST(LoopbackTransportTest, InboxDeliversInFifoOrder) {
  auto a = std::make_unique<LoopbackTransport>(kEndpointA);
  auto b = std::make_unique<LoopbackTransport>(kEndpointB);
  a->connect(*b);

  auto sendByte = [&](std::byte v) {
    const std::array<std::byte, 1> payload{v};
    ASSERT_TRUE(a->send(b->self(), payload));
  };
  auto receiveByte = [&]() -> std::byte {
    PacketSlot slot;
    EXPECT_TRUE(b->tryReceive(slot));
    return slot.data[0];
  };

  sendByte(std::byte{0xAA});
  sendByte(std::byte{0xBB});
  sendByte(std::byte{0xCC});

  EXPECT_EQ(receiveByte(), std::byte{0xAA});
  EXPECT_EQ(receiveByte(), std::byte{0xBB});
  EXPECT_EQ(receiveByte(), std::byte{0xCC});

  PacketSlot slot;
  EXPECT_FALSE(b->tryReceive(slot));

  sendByte(std::byte{0x11});
  sendByte(std::byte{0x22});
  sendByte(std::byte{0x33});

  EXPECT_EQ(receiveByte(), std::byte{0x11});
  EXPECT_EQ(receiveByte(), std::byte{0x22});
  EXPECT_EQ(receiveByte(), std::byte{0x33});
}

TEST(LoopbackTransportTest, FullInboxDropsNewPacketWithoutDisturbingQueued) {
  auto a = std::make_unique<LoopbackTransport>(kEndpointA);
  auto b = std::make_unique<LoopbackTransport>(kEndpointB);
  a->connect(*b);

  for (size_t i = 0; i < kLoopbackCapacity; ++i) {
    const std::array<std::byte, 1> payload{static_cast<std::byte>(i & 0xFF)};
    EXPECT_TRUE(a->send(b->self(), payload)) << "send " << i;
  }
  EXPECT_EQ(b->inboxSize(), kLoopbackCapacity);

  const std::array<std::byte, 1> overflow_payload{std::byte{0xFF}};
  EXPECT_FALSE(a->send(b->self(), overflow_payload));

  for (size_t i = 0; i < kLoopbackCapacity; ++i) {
    PacketSlot slot;
    ASSERT_TRUE(b->tryReceive(slot)) << "receive " << i;
    EXPECT_EQ(slot.data[0], static_cast<std::byte>(i & 0xFF)) << "receive " << i;
  }
  PacketSlot slot;
  EXPECT_FALSE(b->tryReceive(slot));
}

TEST(LoopbackTransportTest, RejectsSendsToAnythingButTheConnectedPeer) {
  auto a = std::make_unique<LoopbackTransport>(kEndpointA);
  auto b = std::make_unique<LoopbackTransport>(kEndpointB);
  auto c = std::make_unique<LoopbackTransport>(Endpoint{0x7F000001u, 0x2001u});
  a->connect(*b);

  const std::array<std::byte, 1> payload{std::byte{0x01}};

  EXPECT_FALSE(a->send(Endpoint{0x7F000001u, 0x2000u}, payload));
  EXPECT_EQ(b->inboxSize(), 0u);

  EXPECT_FALSE(a->send(Endpoint{}, payload));
  EXPECT_EQ(b->inboxSize(), 0u);

  EXPECT_FALSE(c->send(a->self(), payload));
  EXPECT_EQ(b->inboxSize(), 0u);

  EXPECT_TRUE(a->send(b->self(), payload));
}

}  // namespace
}  // namespace net
