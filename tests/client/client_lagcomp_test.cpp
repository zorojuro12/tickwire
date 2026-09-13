#include "client/client.h"

#include <array>
#include <memory>
#include <span>

#include <gtest/gtest.h>

#include "net/framing.h"
#include "net/protocol.h"
#include "net/transport.h"
#include "sim/sim.h"
#include "support/recording_transport.h"

namespace client {
namespace {

using testsupport::RecordingTransport;

constexpr net::Endpoint kServerEp{0x7F000001u, 0x3000u};

void injectJoinAccept(RecordingTransport& tp, uint32_t player_id, uint16_t ack_seq,
                       uint32_t tick = 0) {
  std::array<std::byte, net::kJoinAcceptBytes> payload{};
  net::ByteWriter pw(payload);
  ASSERT_TRUE(net::encodeJoinAccept(player_id, pw));
  net::PacketHeader h;
  h.type = net::MsgType::kJoinAccept;
  h.ack_seq = ack_seq;
  h.tick = tick;
  std::array<std::byte, net::kMaxPacket> buf{};
  const size_t written = net::framePacket(h, payload, buf);
  ASSERT_GT(written, 0u);
  tp.inject(kServerEp, std::span<const std::byte>(buf).subspan(0, written));
}

void injectSnapshot(RecordingTransport& tp, uint32_t tick, uint32_t send_time_ms,
                     const sim::WorldSnapshot& snap, uint32_t ack_tick = 0) {
  std::array<std::byte, net::kMaxPacket> payload{};
  net::ByteWriter pw(payload);
  ASSERT_TRUE(net::encodeSnapshot(snap, pw));
  net::PacketHeader h;
  h.type = net::MsgType::kSnapshot;
  h.tick = tick;
  h.send_time_ms = send_time_ms;
  h.ack_tick = ack_tick;
  std::array<std::byte, net::kMaxPacket> buf{};
  const size_t written =
      net::framePacket(h, std::span<const std::byte>(payload).subspan(0, pw.size()), buf);
  ASSERT_GT(written, 0u);
  tp.inject(kServerEp, std::span<const std::byte>(buf).subspan(0, written));
}

void injectHitConfirm(RecordingTransport& tp, const net::Endpoint& from, uint32_t target_id,
                       uint32_t fire_tick) {
  const net::HitConfirm hc{.target_id = target_id, .fire_tick = fire_tick};
  std::array<std::byte, net::kHitConfirmBytes> payload{};
  net::ByteWriter pw(payload);
  ASSERT_TRUE(net::encodeHitConfirm(hc, pw));
  net::PacketHeader h;
  h.type = net::MsgType::kHitConfirm;
  std::array<std::byte, net::kMaxPacket> buf{};
  const size_t written = net::framePacket(h, payload, buf);
  ASSERT_GT(written, 0u);
  tp.inject(from, std::span<const std::byte>(buf).subspan(0, written));
}

// Injects a kHitConfirm whose payload is exactly `payload`, bypassing
// encodeHitConfirm's own validation -- for exercising the decoder's own
// rejection paths (a target_id == 0 payload, a wrong-length payload).
void injectRawHitConfirm(RecordingTransport& tp, const net::Endpoint& from,
                          std::span<const std::byte> payload) {
  net::PacketHeader h;
  h.type = net::MsgType::kHitConfirm;
  std::array<std::byte, net::kMaxPacket> buf{};
  const size_t written = net::framePacket(h, payload, buf);
  ASSERT_GT(written, 0u);
  tp.inject(from, std::span<const std::byte>(buf).subspan(0, written));
}

void lastSentInput(const RecordingTransport& tp, sim::InputCommand& out) {
  ASSERT_GT(tp.sentCount(), 0u);
  const RecordingTransport::Sent& s = tp.sentAt(tp.sentCount() - 1);
  net::ByteReader r(std::span<const std::byte>(s.data).subspan(0, s.len));
  net::PacketHeader h;
  ASSERT_TRUE(net::decodeHeader(r, h));
  ASSERT_EQ(h.type, net::MsgType::kInput);
  ASSERT_TRUE(net::decodeInput(r, out));
}

TEST(ClientLagCompTest, InputCarriesTheTickTheClientDrew) {
  auto tp = std::make_unique<RecordingTransport>();
  Client<RecordingTransport> c(*tp, kServerEp);
  c.beginJoin(0);
  injectJoinAccept(*tp, 1, kJoinSeq, 500);
  c.tick(16);

  sim::WorldSnapshot snap1{};
  snap1.tick = 100;
  snap1.count = 2;
  snap1.players[0] = {.id = 1, .x = 0.0f, .y = 0.0f, .vx = 0.0f, .vy = 0.0f, .radius = 0.5f};
  snap1.players[1] = {.id = 2, .x = 0.0f, .y = 0.0f, .vx = 0.0f, .vy = 0.0f, .radius = 0.5f};
  injectSnapshot(*tp, 100, 5000, snap1, 0);
  c.tick(32);

  sim::WorldSnapshot snap2 = snap1;
  snap2.tick = 104;
  snap2.players[1] = {.id = 2, .x = 10.0f, .y = 0.0f, .vx = 0.0f, .vy = 0.0f, .radius = 0.5f};
  injectSnapshot(*tp, 104, 5064, snap2, 100);

  uint32_t now_ms = 48;
  for (int i = 0; i < 64 && c.renderTick() != 102; ++i) {
    c.tick(now_ms);
    now_ms += 16;
  }
  ASSERT_EQ(c.renderTick(), 102u);

  EXPECT_TRUE(c.lagCompensationEnabled());
  sim::InputCommand in{};
  ASSERT_TRUE(c.sendInput(now_ms, 0.0f, 0.0f, 1.0f, 0.0f, false));
  lastSentInput(*tp, in);
  EXPECT_EQ(in.view_tick, 102u);

  c.setInterpolationEnabled(false);
  ASSERT_TRUE(c.sendInput(now_ms, 0.0f, 0.0f, 1.0f, 0.0f, false));
  lastSentInput(*tp, in);
  EXPECT_EQ(in.view_tick, 104u);
  c.setInterpolationEnabled(true);

  c.setLagCompensationEnabled(false);
  EXPECT_FALSE(c.lagCompensationEnabled());
  ASSERT_TRUE(c.sendInput(now_ms, 0.0f, 0.0f, 1.0f, 0.0f, false));
  lastSentInput(*tp, in);
  EXPECT_EQ(in.view_tick, 0u);
  c.setLagCompensationEnabled(true);

  bool starved = false;
  for (int i = 0; i < 64 && !starved; ++i) {
    c.tick(now_ms);
    now_ms += 16;
    if (c.renderTick() > 104) starved = true;
  }
  ASSERT_TRUE(starved);
  ASSERT_TRUE(c.sendInput(now_ms, 0.0f, 0.0f, 1.0f, 0.0f, false));
  lastSentInput(*tp, in);
  EXPECT_EQ(in.view_tick, 104u);

  auto tp2 = std::make_unique<RecordingTransport>();
  Client<RecordingTransport> c2(*tp2, kServerEp);
  c2.beginJoin(0);
  injectJoinAccept(*tp2, 1, kJoinSeq, 0);
  c2.tick(16);
  ASSERT_TRUE(c2.sendInput(16, 0.0f, 0.0f, 1.0f, 0.0f, false));
  sim::InputCommand in2{};
  lastSentInput(*tp2, in2);
  EXPECT_EQ(in2.view_tick, 0u);
}

TEST(ClientLagCompTest, CountsHitConfirmationsFromTheServerOnly) {
  constexpr net::Endpoint kOtherEp{0x7F000001u, 0x3999u};

  auto tp = std::make_unique<RecordingTransport>();
  Client<RecordingTransport> c(*tp, kServerEp);
  c.beginJoin(0);
  injectJoinAccept(*tp, 1, kJoinSeq, 0);
  c.tick(16);

  injectHitConfirm(*tp, kServerEp, 2, 777);
  c.tick(32);
  EXPECT_EQ(c.hitsConfirmed(), 1u);
  EXPECT_EQ(c.lastHitTarget(), 2u);
  EXPECT_EQ(c.lastHitFireTick(), 777u);

  injectHitConfirm(*tp, kOtherEp, 5, 900);
  c.tick(48);
  EXPECT_EQ(c.hitsConfirmed(), 1u);
  EXPECT_EQ(c.lastHitTarget(), 2u);
  EXPECT_EQ(c.lastHitFireTick(), 777u);

  const std::array<std::byte, net::kHitConfirmBytes> zero_target = {
      std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
      std::byte{0xD2}, std::byte{0x04}, std::byte{0x00}, std::byte{0x00}};
  injectRawHitConfirm(*tp, kServerEp, zero_target);
  const std::array<std::byte, net::kHitConfirmBytes - 1> too_short{};
  injectRawHitConfirm(*tp, kServerEp, too_short);
  c.tick(64);
  EXPECT_EQ(c.hitsConfirmed(), 1u);
  EXPECT_EQ(c.lastHitTarget(), 2u);
  EXPECT_EQ(c.lastHitFireTick(), 777u);

  auto idle_tp = std::make_unique<RecordingTransport>();
  Client<RecordingTransport> idle(*idle_tp, kServerEp);
  injectHitConfirm(*idle_tp, kServerEp, 3, 100);
  idle.tick(16);
  EXPECT_EQ(idle.hitsConfirmed(), 0u);
}

}  // namespace
}  // namespace client
