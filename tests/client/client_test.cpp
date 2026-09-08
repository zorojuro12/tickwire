#include "client/client.h"

#include <array>
#include <cstdint>
#include <memory>

#include <gtest/gtest.h>

#include "net/framing.h"
#include "net/loopback.h"
#include "net/protocol.h"
#include "net/simulated.h"
#include "server/server.h"
#include "sim/sim.h"
#include "support/recording_transport.h"

namespace client {
namespace {

using testsupport::RecordingTransport;

constexpr net::Endpoint kServerEp{0x7F000001u, 0x3000u};

void injectJoinAccept(RecordingTransport& tp, uint32_t player_id, uint16_t ack_seq) {
  std::array<std::byte, net::kJoinAcceptBytes> payload{};
  net::ByteWriter pw(payload);
  ASSERT_TRUE(net::encodeJoinAccept(player_id, pw));
  net::PacketHeader h;
  h.type = net::MsgType::kJoinAccept;
  h.ack_seq = ack_seq;
  std::array<std::byte, net::kMaxPacket> buf{};
  const size_t written = net::framePacket(h, payload, buf);
  ASSERT_GT(written, 0u);
  tp.inject(kServerEp, std::span<const std::byte>(buf).subspan(0, written));
}

void injectLeaveAck(RecordingTransport& tp, uint16_t ack_seq) {
  net::PacketHeader h;
  h.type = net::MsgType::kLeave;
  h.ack_seq = ack_seq;
  std::array<std::byte, net::kMaxPacket> buf{};
  const size_t written = net::framePacket(h, {}, buf);
  ASSERT_GT(written, 0u);
  tp.inject(kServerEp, std::span<const std::byte>(buf).subspan(0, written));
}

TEST(ClientTest, FreshClientIsIdle) {
  RecordingTransport tp;
  Client<RecordingTransport> c(tp, kServerEp);

  EXPECT_EQ(c.state(), State::kIdle);
  EXPECT_EQ(c.playerId(), 0u);
  EXPECT_EQ(c.latestSnapshotTick(), 0u);
  EXPECT_EQ(c.snapshotsReceived(), 0u);
  EXPECT_EQ(c.joinAttempts(), 0u);

  EXPECT_FALSE(c.sendInput(0, 1.0f, 0.0f, 0.0f, 0.0f, false));
  EXPECT_EQ(tp.sentCount(), 0u);
}

TEST(ClientTest, JoinHandshakeRetransmitsUntilAnsweredThenStops) {
  RecordingTransport tp;
  Client<RecordingTransport> c(tp, kServerEp);

  c.beginJoin(1000);
  EXPECT_EQ(c.state(), State::kJoining);
  EXPECT_EQ(c.joinAttempts(), 1u);
  ASSERT_EQ(tp.sentCount(), 1u);
  {
    const RecordingTransport::Sent& s = tp.sentAt(0);
    EXPECT_EQ(s.to, kServerEp);
    net::ByteReader r(std::span<const std::byte>(s.data).subspan(0, s.len));
    net::PacketHeader h;
    ASSERT_TRUE(net::decodeHeader(r, h));
    EXPECT_EQ(h.type, net::MsgType::kJoinRequest);
    EXPECT_EQ(h.version, net::kProtocolVersion);
    EXPECT_EQ(h.payload_len, 0u);
    EXPECT_EQ(h.seq, kJoinSeq);
    EXPECT_EQ(h.send_time_ms, 1000u);
  }

  // 14 further ticks send nothing more.
  for (int i = 0; i < 14; ++i) c.tick(1000 + static_cast<uint32_t>(i) * 16);
  EXPECT_EQ(tp.sentCount(), 1u);

  // The 15th sends a second identical join request.
  c.tick(1000 + 14 * 16);
  EXPECT_EQ(tp.sentCount(), 2u);
  EXPECT_EQ(c.joinAttempts(), 2u);
  {
    const RecordingTransport::Sent& s = tp.sentAt(1);
    net::ByteReader r(std::span<const std::byte>(s.data).subspan(0, s.len));
    net::PacketHeader h;
    ASSERT_TRUE(net::decodeHeader(r, h));
    EXPECT_EQ(h.type, net::MsgType::kJoinRequest);
    EXPECT_EQ(h.seq, kJoinSeq);
  }
}

TEST(ClientTest, AcceptanceStopsRetransmissionAndAssignsTheId) {
  RecordingTransport tp;
  Client<RecordingTransport> c(tp, kServerEp);
  c.beginJoin(0);

  injectJoinAccept(tp, 4, kJoinSeq);
  c.tick(16);

  EXPECT_EQ(c.state(), State::kJoined);
  EXPECT_EQ(c.playerId(), 4u);

  const size_t sent_before = tp.sentCount();
  for (int i = 0; i < 60; ++i) c.tick(32 + static_cast<uint32_t>(i) * 16);
  EXPECT_EQ(tp.sentCount(), sent_before);
}

TEST(ClientTest, MismatchedAckIsIgnored) {
  RecordingTransport tp;
  Client<RecordingTransport> c(tp, kServerEp);
  c.beginJoin(0);

  injectJoinAccept(tp, 4, 9);
  c.tick(16);

  EXPECT_EQ(c.state(), State::kJoining);
  EXPECT_EQ(c.playerId(), 0u);
}

TEST(ClientTest, RejectionMovesToRejectedAndStopsSending) {
  RecordingTransport tp;
  Client<RecordingTransport> c(tp, kServerEp);
  c.beginJoin(0);

  injectLeaveAck(tp, kJoinSeq);
  c.tick(16);

  EXPECT_EQ(c.state(), State::kRejected);
  const size_t sent_before = tp.sentCount();
  for (int i = 0; i < 60; ++i) c.tick(32 + static_cast<uint32_t>(i) * 16);
  EXPECT_EQ(tp.sentCount(), sent_before);
}

TEST(ClientTest, GivingUpAfterMaxAttemptsFails) {
  RecordingTransport tp;
  Client<RecordingTransport> c(tp, kServerEp);
  c.beginJoin(0);

  uint32_t now_ms = 16;
  while (c.state() == State::kJoining && c.joinAttempts() < kJoinMaxAttempts) {
    for (uint32_t i = 0; i < kJoinRetryTicks; ++i) {
      c.tick(now_ms);
      now_ms += 16;
    }
  }
  // One more retry window pushes past kJoinMaxAttempts.
  for (uint32_t i = 0; i < kJoinRetryTicks && c.state() == State::kJoining; ++i) {
    c.tick(now_ms);
    now_ms += 16;
  }

  EXPECT_EQ(c.state(), State::kFailed);
  const size_t sent_before = tp.sentCount();
  c.tick(now_ms);
  EXPECT_EQ(tp.sentCount(), sent_before);
}

TEST(ClientTest, JunkPacketsAreIgnored) {
  RecordingTransport tp;
  Client<RecordingTransport> c(tp, kServerEp);
  c.beginJoin(0);
  const uint32_t attempts_before = c.joinAttempts();

  std::array<std::byte, 3> tiny{};
  tp.inject(kServerEp, tiny);

  std::array<std::byte, net::kHeaderBytes> bad_magic{};
  tp.inject(kServerEp, bad_magic);

  {
    const sim::InputCommand in{.player_id = 1,
                                .tick = 0,
                                .move_x = 0.0f,
                                .move_y = 0.0f,
                                .aim_x = 0.0f,
                                .aim_y = 0.0f,
                                .fire = false};
    std::array<std::byte, net::kInputBytes> payload{};
    net::ByteWriter pw(payload);
    ASSERT_TRUE(net::encodeInput(in, pw));
    net::PacketHeader h;
    h.type = net::MsgType::kInput;
    std::array<std::byte, net::kMaxPacket> buf{};
    const size_t written = net::framePacket(h, payload, buf);
    ASSERT_GT(written, 0u);
    tp.inject(kServerEp, std::span<const std::byte>(buf).subspan(0, written));
  }

  c.tick(16);

  EXPECT_EQ(c.state(), State::kJoining);
  EXPECT_EQ(c.playerId(), 0u);
  EXPECT_EQ(c.joinAttempts(), attempts_before);
  EXPECT_EQ(c.snapshotsReceived(), 0u);
}

void injectSnapshot(RecordingTransport& tp, uint32_t tick, uint32_t send_time_ms,
                     const sim::WorldSnapshot& snap) {
  std::array<std::byte, net::kMaxPacket> payload{};
  net::ByteWriter pw(payload);
  ASSERT_TRUE(net::encodeSnapshot(snap, pw));
  net::PacketHeader h;
  h.type = net::MsgType::kSnapshot;
  h.tick = tick;
  h.send_time_ms = send_time_ms;
  std::array<std::byte, net::kMaxPacket> buf{};
  const size_t written =
      net::framePacket(h, std::span<const std::byte>(payload).subspan(0, pw.size()), buf);
  ASSERT_GT(written, 0u);
  tp.inject(kServerEp, std::span<const std::byte>(buf).subspan(0, written));
}

sim::WorldSnapshot twoPlayerSnapshot(uint32_t tick) {
  sim::WorldSnapshot s{};
  s.tick = tick;
  s.count = 2;
  s.players[0] = {.id = 1, .x = 1.0f, .y = 2.0f, .vx = 0.0f, .vy = 0.0f, .radius = 0.5f};
  s.players[1] = {.id = 2, .x = -3.0f, .y = 4.0f, .vx = 1.0f, .vy = -1.0f, .radius = 0.5f};
  return s;
}

TEST(ClientTest, InputsGoOutAndSnapshotsLandNewestWins) {
  RecordingTransport tp;
  Client<RecordingTransport> c(tp, kServerEp);
  c.beginJoin(0);
  injectJoinAccept(tp, 4, kJoinSeq);
  c.tick(16);
  ASSERT_EQ(c.playerId(), 4u);
  const size_t sent_before_input = tp.sentCount();

  EXPECT_TRUE(c.sendInput(2000, 1.0f, 0.0f, 0.0f, 1.0f, true));
  ASSERT_EQ(tp.sentCount(), sent_before_input + 1);
  {
    const RecordingTransport::Sent& s = tp.sentAt(sent_before_input);
    net::ByteReader r(std::span<const std::byte>(s.data).subspan(0, s.len));
    net::PacketHeader h;
    ASSERT_TRUE(net::decodeHeader(r, h));
    EXPECT_EQ(h.type, net::MsgType::kInput);
    EXPECT_EQ(h.send_time_ms, 2000u);
    EXPECT_EQ(h.payload_len, net::kInputBytes);
    sim::InputCommand in{};
    ASSERT_TRUE(net::decodeInput(r, in));
    EXPECT_EQ(in.player_id, 4u);
    EXPECT_EQ(in.move_x, 1.0f);
    EXPECT_EQ(in.move_y, 0.0f);
    EXPECT_EQ(in.aim_x, 0.0f);
    EXPECT_EQ(in.aim_y, 1.0f);
    EXPECT_TRUE(in.fire);
  }

  injectSnapshot(tp, 30, 5000, twoPlayerSnapshot(30));
  c.tick(2016);
  EXPECT_EQ(c.snapshotsReceived(), 1u);
  EXPECT_EQ(c.latestSnapshotTick(), 30u);
  EXPECT_EQ(c.serverTimeMs(), 5000u);
  ASSERT_EQ(c.latestSnapshot().count, 2u);
  EXPECT_EQ(c.latestSnapshot().players[0].id, 1u);
  EXPECT_EQ(c.latestSnapshot().players[0].x, 1.0f);
  EXPECT_EQ(c.latestSnapshot().players[1].id, 2u);

  // Stale (tick 27 after tick 30) is dropped, but still counted as received.
  injectSnapshot(tp, 27, 5016, twoPlayerSnapshot(27));
  c.tick(2032);
  EXPECT_EQ(c.snapshotsReceived(), 2u);
  EXPECT_EQ(c.latestSnapshotTick(), 30u);

  // A newer one (tick 33) is adopted.
  injectSnapshot(tp, 33, 5032, twoPlayerSnapshot(33));
  c.tick(2048);
  EXPECT_EQ(c.snapshotsReceived(), 3u);
  EXPECT_EQ(c.latestSnapshotTick(), 33u);

  // A malformed snapshot (count = 33) is ignored entirely.
  {
    std::array<std::byte, net::kSnapshotFixedBytes> bad_payload{};
    net::ByteWriter pw(bad_payload);
    pw.u32(99);
    pw.u32(33);
    net::PacketHeader h;
    h.type = net::MsgType::kSnapshot;
    h.tick = 40;
    std::array<std::byte, net::kMaxPacket> buf{};
    const size_t written = net::framePacket(h, bad_payload, buf);
    ASSERT_GT(written, 0u);
    tp.inject(kServerEp, std::span<const std::byte>(buf).subspan(0, written));
  }
  c.tick(2064);
  EXPECT_EQ(c.latestSnapshotTick(), 33u);

  // leave() sends one kLeave and returns to kIdle.
  const size_t sent_before_leave = tp.sentCount();
  c.leave(3000);
  ASSERT_EQ(tp.sentCount(), sent_before_leave + 1);
  {
    const RecordingTransport::Sent& s = tp.sentAt(sent_before_leave);
    net::ByteReader r(std::span<const std::byte>(s.data).subspan(0, s.len));
    net::PacketHeader h;
    ASSERT_TRUE(net::decodeHeader(r, h));
    EXPECT_EQ(h.type, net::MsgType::kLeave);
    EXPECT_EQ(h.seq, kLeaveSeq);
    EXPECT_EQ(h.payload_len, 0u);
  }
  EXPECT_EQ(c.state(), State::kIdle);
  EXPECT_FALSE(c.sendInput(3016, 0.0f, 0.0f, 0.0f, 0.0f, false));
}

TEST(ClientTest, ClientAndServerConvergeInMemory) {
  constexpr net::Endpoint kClientEp{0x7F000001u, 0x3010u};
  constexpr net::Endpoint kServerLoopEp{0x7F000001u, 0x3011u};

  auto client_tp = std::make_unique<net::LoopbackTransport>(kClientEp);
  auto server_tp = std::make_unique<net::LoopbackTransport>(kServerLoopEp);
  client_tp->connect(*server_tp);

  auto srv = std::make_unique<server::Server<net::LoopbackTransport>>(*server_tp);
  auto c = std::make_unique<Client<net::LoopbackTransport>>(*client_tp, kServerLoopEp);

  uint32_t ms = 0;
  auto pump = [&] {
    srv->ingest();
    srv->tick(ms);
    c->tick(ms);
    ms += 16;
  };

  c->beginJoin(ms);
  ms += 16;
  for (int i = 0; i < 5; ++i) pump();

  EXPECT_EQ(c->state(), State::kJoined);
  EXPECT_EQ(c->playerId(), 1u);
  EXPECT_EQ(srv->world().playerCount(), 1u);

  const float spawn_x = -35.0f;
  for (int i = 0; i < 60; ++i) {
    ASSERT_TRUE(c->sendInput(ms, 1.0f, 0.0f, 0.0f, 0.0f, false));
    pump();
  }

  EXPECT_GE(c->snapshotsReceived(), 15u);
  ASSERT_GT(c->latestSnapshot().count, 0u);
  const sim::PlayerState* own = nullptr;
  for (uint32_t i = 0; i < c->latestSnapshot().count; ++i) {
    if (c->latestSnapshot().players[i].id == 1) own = &c->latestSnapshot().players[i];
  }
  ASSERT_NE(own, nullptr);
  EXPECT_GE(own->x, spawn_x + 50.0f * sim::kMoveSpeed * sim::kTickDt);

  c->leave(ms);
  pump();
  pump();
  EXPECT_EQ(srv->world().playerCount(), 0u);
}

TEST(ClientTest, ClientAndServerConvergeThroughSimulatedLatency) {
  constexpr net::Endpoint kClientEp{0x7F000001u, 0x3020u};
  constexpr net::Endpoint kServerLoopEp{0x7F000001u, 0x3021u};

  // Unwrapped baseline: how many pump iterations until kJoined.
  auto baselineIterationsToJoin = [&] {
    auto client_tp = std::make_unique<net::LoopbackTransport>(kClientEp);
    auto server_tp = std::make_unique<net::LoopbackTransport>(kServerLoopEp);
    client_tp->connect(*server_tp);
    auto srv = std::make_unique<server::Server<net::LoopbackTransport>>(*server_tp);
    auto c = std::make_unique<Client<net::LoopbackTransport>>(*client_tp, kServerLoopEp);

    uint32_t ms = 0;
    c->beginJoin(ms);
    ms += 16;
    for (int i = 0; i < 200; ++i) {
      srv->ingest();
      srv->tick(ms);
      c->tick(ms);
      ms += 16;
      if (c->state() == State::kJoined) return i;
    }
    return -1;
  }();
  ASSERT_GE(baselineIterationsToJoin, 0);

  auto client_tp = std::make_unique<net::LoopbackTransport>(kClientEp);
  auto server_tp = std::make_unique<net::LoopbackTransport>(kServerLoopEp);
  client_tp->connect(*server_tp);
  auto srv = std::make_unique<server::Server<net::LoopbackTransport>>(*server_tp);

  net::SimConfig cfg;
  cfg.latency_ms = 100;
  cfg.jitter_ms = 0;
  cfg.loss_permille = 0;
  cfg.seed = 1;
  auto wrapped = std::make_unique<net::SimulatedTransport<net::LoopbackTransport>>(*client_tp, cfg);
  auto c = std::make_unique<Client<net::SimulatedTransport<net::LoopbackTransport>>>(
      *wrapped, kServerLoopEp);

  uint32_t ms = 0;
  c->beginJoin(ms);
  ms += 16;
  int delayed_iterations_to_join = -1;
  for (int i = 0; i < 200; ++i) {
    srv->ingest();
    srv->tick(ms);
    c->tick(ms);
    wrapped->advanceTick();
    ms += 16;
    if (c->state() == State::kJoined) {
      delayed_iterations_to_join = i;
      break;
    }
  }

  ASSERT_GE(delayed_iterations_to_join, 0);
  EXPECT_GE(c->joinAttempts(), 1u);
  EXPECT_GT(delayed_iterations_to_join, baselineIterationsToJoin);
}

}  // namespace
}  // namespace client
