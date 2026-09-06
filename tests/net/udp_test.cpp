#include "net/udp.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cstring>
#include <vector>

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

bool pollReceive(UdpTransport& t, PacketSlot& slot, int iterations = 1000) {
  for (int i = 0; i < iterations; ++i) {
    if (t.tryReceive(slot)) return true;
  }
  return false;
}

// Sends `len` raw bytes to `to` from a socket the test controls directly,
// bypassing UdpTransport::send, which would itself refuse an oversized
// payload.
void sendRawDatagram(const Endpoint& to, size_t len) {
  const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
  ASSERT_GE(fd, 0);
  std::vector<std::byte> buf(len, std::byte{0x42});
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = to.addr_be;
  addr.sin_port = to.port_be;
  const ssize_t sent =
      ::sendto(fd, buf.data(), buf.size(), 0, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
  ASSERT_EQ(sent, static_cast<ssize_t>(len));
  ::close(fd);
}

TEST(UdpTransportTest, OversizedDatagramsAreDroppedNotTruncated) {
  const uint32_t loopback_be = htonl(INADDR_LOOPBACK);
  UdpTransport a;
  UdpTransport b;
  ASSERT_TRUE(a.bind(loopback_be, 0));
  ASSERT_TRUE(b.bind(loopback_be, 0));

  const std::array<std::byte, 5> small_payload = {std::byte{0x01}, std::byte{0x02},
                                                    std::byte{0x03}, std::byte{0x04},
                                                    std::byte{0x05}};

  PacketSlot slot;
  sendRawDatagram(b.localEndpoint(), 1201);
  EXPECT_FALSE(pollReceive(b, slot));
  ASSERT_TRUE(a.send(b.localEndpoint(), small_payload));
  ASSERT_TRUE(pollReceive(b, slot));
  EXPECT_EQ(slot.len, 5u);

  sendRawDatagram(b.localEndpoint(), 1500);
  EXPECT_FALSE(pollReceive(b, slot));
  ASSERT_TRUE(a.send(b.localEndpoint(), small_payload));
  ASSERT_TRUE(pollReceive(b, slot));
  EXPECT_EQ(slot.len, 5u);

  sendRawDatagram(b.localEndpoint(), kMaxPacket);
  ASSERT_TRUE(pollReceive(b, slot));
  EXPECT_EQ(slot.len, kMaxPacket);
}

TEST(UdpTransportTest, SendRejectsPayloadsOverKMaxPacket) {
  const uint32_t loopback_be = htonl(INADDR_LOOPBACK);
  UdpTransport a;
  UdpTransport b;
  ASSERT_TRUE(a.bind(loopback_be, 0));
  ASSERT_TRUE(b.bind(loopback_be, 0));

  const std::vector<std::byte> too_big_1201(1201, std::byte{0x01});
  const std::vector<std::byte> too_big_4096(4096, std::byte{0x01});
  const std::vector<std::byte> exactly_max(kMaxPacket, std::byte{0x01});

  EXPECT_FALSE(a.send(b.localEndpoint(), too_big_1201));
  EXPECT_FALSE(a.send(b.localEndpoint(), too_big_4096));
  EXPECT_TRUE(a.send(b.localEndpoint(), exactly_max));

  PacketSlot slot;
  ASSERT_TRUE(pollReceive(b, slot));
  EXPECT_EQ(slot.len, kMaxPacket);
  EXPECT_FALSE(pollReceive(b, slot, 100));
}

}  // namespace
}  // namespace net
