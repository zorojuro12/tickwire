#include "net/udp.h"

#include <arpa/inet.h>
#include <netinet/in.h>

#include <array>
#include <cstring>

#include <gtest/gtest.h>

namespace net {
namespace {

TEST(UdpTransportTest, TwoBoundSocketsExchangeADatagramOnLocalhost) {
  const uint32_t loopback_be = htonl(INADDR_LOOPBACK);

  UdpTransport a;
  UdpTransport b;
  ASSERT_TRUE(a.bind(loopback_be, 0));
  ASSERT_TRUE(b.bind(loopback_be, 0));
  EXPECT_NE(a.localEndpoint().port_be, 0u);
  EXPECT_NE(b.localEndpoint().port_be, 0u);
  EXPECT_NE(a.localEndpoint().port_be, b.localEndpoint().port_be);

  PacketSlot slot;
  EXPECT_FALSE(b.tryReceive(slot));

  const std::array<std::byte, 5> payload = {std::byte{0x01}, std::byte{0x02}, std::byte{0x03},
                                             std::byte{0x04}, std::byte{0x05}};
  ASSERT_TRUE(a.send(b.localEndpoint(), payload));

  bool received = false;
  for (int i = 0; i < 1000 && !received; ++i) {
    received = b.tryReceive(slot);
  }
  ASSERT_TRUE(received);
  EXPECT_EQ(slot.len, 5u);
  EXPECT_EQ(std::memcmp(slot.data.data(), payload.data(), payload.size()), 0);
  EXPECT_EQ(slot.peer, a.localEndpoint());
}

}  // namespace
}  // namespace net
