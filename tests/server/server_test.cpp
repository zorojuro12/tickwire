#include "server/server.h"

#include <array>
#include <memory>
#include <span>

#include <gtest/gtest.h>

#include "net/framing.h"
#include "net/loopback.h"
#include "net/protocol.h"
#include "net/transport.h"
#include "sim/sim.h"
#include "sim/world.h"

namespace server {
namespace {

constexpr net::Endpoint kClientEp{0x7F000001u, 0x1F90u};
constexpr net::Endpoint kServerEp{0x7F000001u, 0x1F91u};

// Frames and sends one packet of `type` carrying `payload`, with the given
// wire `seq`.
template <net::Transport T>
bool sendTo(T& transport, const net::Endpoint& to, net::MsgType type,
            std::span<const std::byte> payload, uint16_t seq) {
  net::PacketHeader h;
  h.type = type;
  h.seq = seq;
  std::array<std::byte, net::kMaxPacket> buf{};
  const size_t written = net::framePacket(h, payload, buf);
  if (written == 0) return false;
  return transport.send(to, std::span<const std::byte>(buf).subspan(0, written));
}

// A test-local transport stub satisfying net::Transport: records every send
// (endpoint, bytes) into a fixed array and lets the test inject inbound
// packets from arbitrary source endpoints -- something a point-to-point
// LoopbackTransport pair cannot do for the 32-distinct-endpoint case. Reused
// by Checkpoints 2-4.
class RecordingTransport {
 public:
  static constexpr size_t kCapacity = 512;

  struct Sent {
    net::Endpoint to;
    std::array<std::byte, net::kMaxPacket> data{};
    uint16_t len = 0;
  };

  bool send(const net::Endpoint& to, std::span<const std::byte> payload) noexcept {
    if (sent_count_ >= kCapacity || payload.size() > net::kMaxPacket) return false;
    Sent& s = sent_[sent_count_++];
    s.to = to;
    s.len = static_cast<uint16_t>(payload.size());
    std::copy(payload.begin(), payload.end(), s.data.begin());
    return true;
  }

  bool tryReceive(net::PacketSlot& slot) noexcept {
    if (inbox_read_ >= inbox_write_) return false;
    slot = inbox_[inbox_read_ % kCapacity];
    ++inbox_read_;
    return true;
  }

  void inject(const net::Endpoint& from, std::span<const std::byte> bytes) noexcept {
    net::PacketSlot slot;
    slot.peer = from;
    slot.len = static_cast<uint16_t>(bytes.size());
    std::copy(bytes.begin(), bytes.end(), slot.data.begin());
    inbox_[inbox_write_ % kCapacity] = slot;
    ++inbox_write_;
  }

  size_t sentCount() const noexcept { return sent_count_; }
  const Sent& sentAt(size_t i) const noexcept { return sent_[i]; }
  void clearSent() noexcept { sent_count_ = 0; }

 private:
  std::array<Sent, kCapacity> sent_{};
  size_t sent_count_ = 0;
  std::array<net::PacketSlot, kCapacity> inbox_{};
  size_t inbox_write_ = 0;
  size_t inbox_read_ = 0;
};
static_assert(net::Transport<RecordingTransport>);

TEST(ServerTest, JoinRequestIsAcceptedBoundAndAnswered) {
  auto client_tp = std::make_unique<net::LoopbackTransport>(kClientEp);
  auto server_tp = std::make_unique<net::LoopbackTransport>(kServerEp);
  client_tp->connect(*server_tp);
  auto srv = std::make_unique<Server<net::LoopbackTransport>>(*server_tp);

  EXPECT_EQ(srv->queuedPackets(), 0u);
  EXPECT_EQ(srv->worldTick(), 0u);
  EXPECT_EQ(srv->world().playerCount(), 0u);
  EXPECT_EQ(srv->droppedPackets(), 0u);

  ASSERT_TRUE(sendTo(*client_tp, kServerEp, net::MsgType::kJoinRequest, {}, 1));
  EXPECT_EQ(srv->ingest(), 1u);
  EXPECT_EQ(srv->queuedPackets(), 1u);
  EXPECT_EQ(srv->world().playerCount(), 0u);

  srv->tick(1000);
  EXPECT_EQ(srv->queuedPackets(), 0u);
  EXPECT_EQ(srv->world().playerCount(), 1u);
  EXPECT_EQ(srv->worldTick(), 1u);
  EXPECT_EQ(srv->playerFor(kClientEp), 1u);

  net::PacketSlot slot;
  ASSERT_TRUE(client_tp->tryReceive(slot));
  {
    net::ByteReader r(std::span<const std::byte>(slot.data).subspan(0, slot.len));
    net::PacketHeader h;
    ASSERT_TRUE(net::decodeHeader(r, h));
    EXPECT_EQ(h.type, net::MsgType::kJoinAccept);
    EXPECT_EQ(h.version, net::kProtocolVersion);
    EXPECT_EQ(h.ack_seq, 1u);
    EXPECT_EQ(h.payload_len, net::kJoinAcceptBytes);
    EXPECT_EQ(h.send_time_ms, 1000u);
    EXPECT_EQ(h.tick, 1u);
    uint32_t accepted_id = 0;
    ASSERT_TRUE(net::decodeJoinAccept(r, accepted_id));
    EXPECT_EQ(accepted_id, 1u);
  }

  sim::WorldSnapshot snap{};
  srv->world().writeSnapshot(snap);
  ASSERT_EQ(snap.count, 1u);
  EXPECT_EQ(snap.players[0].id, 1u);
  EXPECT_EQ(snap.players[0].x, -35.0f);
  EXPECT_EQ(snap.players[0].y, -35.0f);

  // Idempotent re-join: same seq, not re-allocated, re-answered.
  ASSERT_TRUE(sendTo(*client_tp, kServerEp, net::MsgType::kJoinRequest, {}, 1));
  srv->ingest();
  srv->tick(1016);
  EXPECT_EQ(srv->world().playerCount(), 1u);
  EXPECT_EQ(srv->playerFor(kClientEp), 1u);

  ASSERT_TRUE(client_tp->tryReceive(slot));
  net::ByteReader r2(std::span<const std::byte>(slot.data).subspan(0, slot.len));
  net::PacketHeader h2;
  ASSERT_TRUE(net::decodeHeader(r2, h2));
  EXPECT_EQ(h2.type, net::MsgType::kJoinAccept);
  uint32_t accepted_id2 = 0;
  ASSERT_TRUE(net::decodeJoinAccept(r2, accepted_id2));
  EXPECT_EQ(accepted_id2, 1u);

  sim::WorldSnapshot snap2{};
  srv->world().writeSnapshot(snap2);
  ASSERT_EQ(snap2.count, 1u);
  EXPECT_EQ(snap2.players[0].x, -35.0f);
  EXPECT_EQ(snap2.players[0].y, -35.0f);
}

TEST(ServerTest, FullSessionTableRejectsWithLeave) {
  auto tp = std::make_unique<RecordingTransport>();
  auto srv = std::make_unique<Server<RecordingTransport>>(*tp);

  net::PacketHeader join_h;
  join_h.type = net::MsgType::kJoinRequest;
  std::array<std::byte, net::kMaxPacket> join_buf{};

  for (int i = 0; i < 32; ++i) {
    net::Endpoint ep{0x7F000001u, static_cast<uint16_t>(0x1F90 + i)};
    join_h.seq = static_cast<uint16_t>(i + 1);
    const size_t written = net::framePacket(join_h, {}, join_buf);
    ASSERT_GT(written, 0u);
    tp->inject(ep, std::span<const std::byte>(join_buf).subspan(0, written));
  }
  srv->ingest();
  srv->tick(0);
  EXPECT_EQ(srv->world().playerCount(), 32u);

  tp->clearSent();
  net::Endpoint ep33{0x7F000001u, static_cast<uint16_t>(0x1F90 + 32)};
  join_h.seq = 99;
  const size_t written = net::framePacket(join_h, {}, join_buf);
  ASSERT_GT(written, 0u);
  tp->inject(ep33, std::span<const std::byte>(join_buf).subspan(0, written));
  srv->ingest();
  srv->tick(0);

  EXPECT_EQ(srv->world().playerCount(), 32u);
  ASSERT_EQ(tp->sentCount(), 1u);
  const RecordingTransport::Sent& s = tp->sentAt(0);
  EXPECT_EQ(s.to, ep33);
  net::ByteReader r(std::span<const std::byte>(s.data).subspan(0, s.len));
  net::PacketHeader h;
  ASSERT_TRUE(net::decodeHeader(r, h));
  EXPECT_EQ(h.type, net::MsgType::kLeave);
  EXPECT_EQ(h.ack_seq, 99u);
  EXPECT_EQ(h.payload_len, 0u);
}

}  // namespace
}  // namespace server
