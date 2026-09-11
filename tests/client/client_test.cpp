#include "client/client.h"

#include <array>
#include <cstdint>
#include <memory>

#include <gtest/gtest.h>

#include "client/clock_sync.h"
#include "net/framing.h"
#include "net/loopback.h"
#include "net/protocol.h"
#include "net/simulated.h"
#include "net/snapshot_delta.h"
#include "net/snapshot_ring.h"
#include "server/server.h"
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
  auto tp = std::make_unique<RecordingTransport>();
  Client<RecordingTransport> c(*tp, kServerEp);

  EXPECT_EQ(c.state(), State::kIdle);
  EXPECT_EQ(c.playerId(), 0u);
  EXPECT_EQ(c.latestSnapshotTick(), 0u);
  EXPECT_EQ(c.snapshotsReceived(), 0u);
  EXPECT_EQ(c.joinAttempts(), 0u);

  EXPECT_FALSE(c.sendInput(0, 1.0f, 0.0f, 0.0f, 0.0f, false));
  EXPECT_EQ(tp->sentCount(), 0u);
}

TEST(ClientTest, JoinHandshakeRetransmitsUntilAnsweredThenStops) {
  auto tp = std::make_unique<RecordingTransport>();
  Client<RecordingTransport> c(*tp, kServerEp);

  c.beginJoin(1000);
  EXPECT_EQ(c.state(), State::kJoining);
  EXPECT_EQ(c.joinAttempts(), 1u);
  ASSERT_EQ(tp->sentCount(), 1u);
  {
    const RecordingTransport::Sent& s = tp->sentAt(0);
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
  EXPECT_EQ(tp->sentCount(), 1u);

  // The 15th sends a second identical join request.
  c.tick(1000 + 14 * 16);
  EXPECT_EQ(tp->sentCount(), 2u);
  EXPECT_EQ(c.joinAttempts(), 2u);
  {
    const RecordingTransport::Sent& s = tp->sentAt(1);
    net::ByteReader r(std::span<const std::byte>(s.data).subspan(0, s.len));
    net::PacketHeader h;
    ASSERT_TRUE(net::decodeHeader(r, h));
    EXPECT_EQ(h.type, net::MsgType::kJoinRequest);
    EXPECT_EQ(h.seq, kJoinSeq);
  }
}

TEST(ClientTest, AcceptanceStopsRetransmissionAndAssignsTheId) {
  auto tp = std::make_unique<RecordingTransport>();
  Client<RecordingTransport> c(*tp, kServerEp);
  c.beginJoin(0);

  injectJoinAccept(*tp, 4, kJoinSeq);
  c.tick(16);

  EXPECT_EQ(c.state(), State::kJoined);
  EXPECT_EQ(c.playerId(), 4u);

  const size_t sent_before = tp->sentCount();
  for (int i = 0; i < 60; ++i) c.tick(32 + static_cast<uint32_t>(i) * 16);
  EXPECT_EQ(tp->sentCount(), sent_before);
}

TEST(ClientTest, JoinAcceptSeedsTheClockAheadOfTheServer) {
  auto tp = std::make_unique<RecordingTransport>();
  Client<RecordingTransport> c(*tp, kServerEp);
  c.beginJoin(0);

  injectJoinAccept(*tp, 1, kJoinSeq, 500);
  c.tick(16);

  EXPECT_EQ(c.state(), State::kJoined);
  EXPECT_EQ(c.playerId(), 1u);
  EXPECT_EQ(c.clientTick(), 500u + static_cast<uint32_t>(kTargetLeadTicks));
}

TEST(ClientTest, MismatchedAckIsIgnored) {
  auto tp = std::make_unique<RecordingTransport>();
  Client<RecordingTransport> c(*tp, kServerEp);
  c.beginJoin(0);

  injectJoinAccept(*tp, 4, 9);
  c.tick(16);

  EXPECT_EQ(c.state(), State::kJoining);
  EXPECT_EQ(c.playerId(), 0u);
}

TEST(ClientTest, RejectionMovesToRejectedAndStopsSending) {
  auto tp = std::make_unique<RecordingTransport>();
  Client<RecordingTransport> c(*tp, kServerEp);
  c.beginJoin(0);

  injectLeaveAck(*tp, kJoinSeq);
  c.tick(16);

  EXPECT_EQ(c.state(), State::kRejected);
  const size_t sent_before = tp->sentCount();
  for (int i = 0; i < 60; ++i) c.tick(32 + static_cast<uint32_t>(i) * 16);
  EXPECT_EQ(tp->sentCount(), sent_before);
}

TEST(ClientTest, GivingUpAfterMaxAttemptsFails) {
  auto tp = std::make_unique<RecordingTransport>();
  Client<RecordingTransport> c(*tp, kServerEp);
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
  const size_t sent_before = tp->sentCount();
  c.tick(now_ms);
  EXPECT_EQ(tp->sentCount(), sent_before);
}

TEST(ClientTest, JunkPacketsAreIgnored) {
  auto tp = std::make_unique<RecordingTransport>();
  Client<RecordingTransport> c(*tp, kServerEp);
  c.beginJoin(0);
  const uint32_t attempts_before = c.joinAttempts();

  std::array<std::byte, 3> tiny{};
  tp->inject(kServerEp, tiny);

  std::array<std::byte, net::kHeaderBytes> bad_magic{};
  tp->inject(kServerEp, bad_magic);

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
    tp->inject(kServerEp, std::span<const std::byte>(buf).subspan(0, written));
  }

  c.tick(16);

  EXPECT_EQ(c.state(), State::kJoining);
  EXPECT_EQ(c.playerId(), 0u);
  EXPECT_EQ(c.joinAttempts(), attempts_before);
  EXPECT_EQ(c.snapshotsReceived(), 0u);
}

// A UDP socket is unconnected: tryReceive() hands back a datagram from any
// source that reached this port, not just the joined server. Without a
// source check, a single forged Leave from an unrelated address would
// permanently block a still-joining client (no retry path out of
// kRejected) -- no address spoofing required. Found by the P3 Task 8
// security review.
TEST(ClientTest, PacketsFromAnUnrelatedSourceAreIgnored) {
  auto tp = std::make_unique<RecordingTransport>();
  Client<RecordingTransport> c(*tp, kServerEp);
  c.beginJoin(0);

  constexpr net::Endpoint kAttacker{0x7F000001u, 0x9999u};

  // A well-formed JoinAccept from the wrong source must not assign an id.
  {
    std::array<std::byte, net::kJoinAcceptBytes> payload{};
    net::ByteWriter pw(payload);
    ASSERT_TRUE(net::encodeJoinAccept(7, pw));
    net::PacketHeader h;
    h.type = net::MsgType::kJoinAccept;
    h.ack_seq = kJoinSeq;
    std::array<std::byte, net::kMaxPacket> buf{};
    const size_t written = net::framePacket(h, payload, buf);
    ASSERT_GT(written, 0u);
    tp->inject(kAttacker, std::span<const std::byte>(buf).subspan(0, written));
  }
  c.tick(16);
  EXPECT_EQ(c.state(), State::kJoining);
  EXPECT_EQ(c.playerId(), 0u);

  // A well-formed Leave (ack_seq == kJoinSeq) from the wrong source must
  // not move the client to kRejected -- the permanent join-denial this
  // check exists to close.
  {
    net::PacketHeader h;
    h.type = net::MsgType::kLeave;
    h.ack_seq = kJoinSeq;
    std::array<std::byte, net::kMaxPacket> buf{};
    const size_t written = net::framePacket(h, {}, buf);
    ASSERT_GT(written, 0u);
    tp->inject(kAttacker, std::span<const std::byte>(buf).subspan(0, written));
  }
  c.tick(32);
  EXPECT_EQ(c.state(), State::kJoining);

  // The real server's own accept still works afterward.
  injectJoinAccept(*tp, 4, kJoinSeq);
  c.tick(48);
  EXPECT_EQ(c.state(), State::kJoined);
  EXPECT_EQ(c.playerId(), 4u);
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

sim::WorldSnapshot twoPlayerSnapshot(uint32_t tick) {
  sim::WorldSnapshot s{};
  s.tick = tick;
  s.count = 2;
  s.players[0] = {.id = 1, .x = 1.0f, .y = 2.0f, .vx = 0.0f, .vy = 0.0f, .radius = 0.5f};
  s.players[1] = {.id = 2, .x = -3.0f, .y = 4.0f, .vx = 1.0f, .vy = -1.0f, .radius = 0.5f};
  return s;
}

TEST(ClientTest, SnapshotLeadErrorCorrectsTheClientClock) {
  auto tp = std::make_unique<RecordingTransport>();
  Client<RecordingTransport> c(*tp, kServerEp);
  c.beginJoin(0);
  injectJoinAccept(*tp, 1, kJoinSeq, 500);
  c.tick(16);
  ASSERT_EQ(c.clientTick(), 503u);

  // Lead 3, on target: a nominal +1 advance, no correction. Also seeds
  // ClockSync's EMA at this on-target value (its first observation).
  injectSnapshot(*tp, 500, 5000, twoPlayerSnapshot(500), 503);
  c.tick(32);
  EXPECT_EQ(c.clientTick(), 504u);
  EXPECT_EQ(c.clockLead(), 3);

  // A single, one-off short lead must not immediately flip the correction
  // -- that is exactly the raw-signal behavior ClockSync's EMA+deadband
  // exists to avoid (see clock_sync.h). Only a repeated, sustained
  // deviation should eventually cross the deadband and correct.
  injectSnapshot(*tp, 501, 5016, twoPlayerSnapshot(501), 502);
  c.tick(48);
  EXPECT_EQ(c.clientTick(), 505u);  // nominal +1 only, no correction yet

  uint32_t tick_before_loop = c.clientTick();
  uint32_t server_tick = 502;
  uint32_t send_ms = 5032;
  bool corrected = false;
  for (int i = 0; i < 20 && !corrected; ++i) {
    injectSnapshot(*tp, server_tick, send_ms, twoPlayerSnapshot(server_tick), server_tick - 2);
    const uint32_t before = c.clientTick();
    c.tick(send_ms);
    // Advanced by more than the nominal +1 means a correction fired.
    if (c.clientTick() != before + 1) corrected = true;
    ++server_tick;
    send_ms += 16;
  }
  EXPECT_TRUE(corrected);
  EXPECT_GT(c.clientTick(), tick_before_loop);  // corrected forward, matching the short-lead sign
}

TEST(ClientTest, ClockCorrectionNeverUnderflows) {
  auto tp = std::make_unique<RecordingTransport>();
  Client<RecordingTransport> c(*tp, kServerEp);
  c.beginJoin(0);
  injectJoinAccept(*tp, 1, kJoinSeq, 0);
  c.tick(16);
  ASSERT_EQ(c.clientTick(), 3u);

  // Lead 19, error +16: a snap demanding a correction of -16 against a
  // clock reading 4 after tick()'s nominal +1.
  injectSnapshot(*tp, 1, 5016, twoPlayerSnapshot(1), 20);
  c.tick(32);

  EXPECT_LT(c.clientTick(), 1000u);
  EXPECT_EQ(c.clientTick(), 0u);
}

sim::WorldSnapshot onePlayerSnapshot(uint32_t tick, float x, float y, float vx, float vy) {
  sim::WorldSnapshot s{};
  s.tick = tick;
  s.count = 1;
  s.players[0] = {.id = 1, .x = x, .y = y, .vx = vx, .vy = vy, .radius = 0.5f};
  return s;
}

TEST(ClientTest, SendInputMovesTheLocalPlayerWithoutWaitingForASnapshot) {
  auto tp = std::make_unique<RecordingTransport>();
  Client<RecordingTransport> c(*tp, kServerEp);
  c.beginJoin(0);
  injectJoinAccept(*tp, 1, kJoinSeq, 500);
  c.tick(16);
  ASSERT_EQ(c.clientTick(), 503u);
  ASSERT_TRUE(c.predictionEnabled());

  injectSnapshot(*tp, 500, 5000, onePlayerSnapshot(500, 10.0f, 20.0f, 0.0f, 0.0f), 503);
  c.tick(32);

  float x = 0.0f, y = 0.0f;
  ASSERT_TRUE(c.localPosition(x, y));
  EXPECT_EQ(x, 10.0f);
  EXPECT_EQ(y, 20.0f);

  EXPECT_TRUE(c.sendInput(48, 1.0f, 0.0f, 0.0f, 0.0f, false));

  ASSERT_TRUE(c.localPosition(x, y));
  EXPECT_EQ(x, 10.0f + sim::kMoveSpeed * sim::kTickDt);
  EXPECT_EQ(y, 20.0f);
}

TEST(ClientTest, PredictionOffRendersTheSnapshotPosition) {
  auto tp = std::make_unique<RecordingTransport>();
  Client<RecordingTransport> c(*tp, kServerEp);
  c.beginJoin(0);
  injectJoinAccept(*tp, 1, kJoinSeq, 500);
  c.tick(16);

  c.setPredictionEnabled(false);
  EXPECT_FALSE(c.predictionEnabled());

  injectSnapshot(*tp, 500, 5000, onePlayerSnapshot(500, 10.0f, 20.0f, 0.0f, 0.0f), 503);
  c.tick(32);

  float x = 0.0f, y = 0.0f;
  ASSERT_TRUE(c.localPosition(x, y));
  EXPECT_EQ(x, 10.0f);
  EXPECT_EQ(y, 20.0f);

  EXPECT_TRUE(c.sendInput(48, 1.0f, 0.0f, 0.0f, 0.0f, false));
  ASSERT_TRUE(c.localPosition(x, y));
  EXPECT_EQ(x, 10.0f);
  EXPECT_EQ(y, 20.0f);

  c.setPredictionEnabled(true);
  EXPECT_TRUE(c.sendInput(64, 1.0f, 0.0f, 0.0f, 0.0f, false));
  ASSERT_TRUE(c.localPosition(x, y));
  EXPECT_EQ(x, 10.0f + sim::kMoveSpeed * sim::kTickDt);
  EXPECT_EQ(y, 20.0f);
}

TEST(ClientTest, RttComesFromTheAcknowledgedInputsSendTime) {
  auto tp = std::make_unique<RecordingTransport>();
  Client<RecordingTransport> c(*tp, kServerEp);
  c.beginJoin(0);
  injectJoinAccept(*tp, 1, kJoinSeq, 500);
  c.tick(16);
  ASSERT_EQ(c.clientTick(), 503u);
  EXPECT_EQ(c.rttMs(), 0u);

  // Stamped tick 503, sent at ms=100.
  ASSERT_TRUE(c.sendInput(100, 1.0f, 0.0f, 0.0f, 0.0f, false));

  injectSnapshot(*tp, 500, 5016, onePlayerSnapshot(500, 0.0f, 0.0f, 0.0f, 0.0f), 503);
  c.tick(180);
  EXPECT_EQ(c.rttMs(), 80u);

  // An unmatched ack updates nothing.
  injectSnapshot(*tp, 501, 5032, onePlayerSnapshot(501, 0.0f, 0.0f, 0.0f, 0.0f), 999);
  c.tick(196);
  EXPECT_EQ(c.rttMs(), 80u);
}

// ack_tick is chosen as h.tick + kTargetLeadTicks throughout, keeping the
// clock's lead exactly on target so ClockSync never issues a correction --
// which keeps tick_'s progression purely nominal (+1 per tick() call) and
// the tick arithmetic below fully predictable.
TEST(ClientTest, ReconciliationReplaysUnacknowledgedInputs) {
  auto tp = std::make_unique<RecordingTransport>();
  Client<RecordingTransport> c(*tp, kServerEp);
  c.beginJoin(0);
  injectJoinAccept(*tp, 1, kJoinSeq, 500);
  c.tick(16);
  ASSERT_EQ(c.clientTick(), 503u);

  injectSnapshot(*tp, 500, 5000, onePlayerSnapshot(500, 0.0f, 0.0f, 0.0f, 0.0f), 503);
  c.tick(32);  // tick_ 503 -> 504, on-target lead, no correction
  ASSERT_EQ(c.clientTick(), 504u);

  ASSERT_TRUE(c.sendInput(48, 1.0f, 0.0f, 0.0f, 0.0f, false));  // stamps 504
  c.tick(64);                                                   // -> 505
  ASSERT_TRUE(c.sendInput(80, 1.0f, 0.0f, 0.0f, 0.0f, false));  // stamps 505
  c.tick(96);                                                   // -> 506
  ASSERT_TRUE(c.sendInput(112, 1.0f, 0.0f, 0.0f, 0.0f, false));  // stamps 506
  ASSERT_EQ(c.clientTick(), 506u);

  float x = 0.0f, y = 0.0f;
  ASSERT_TRUE(c.localPosition(x, y));
  EXPECT_EQ(x, 3.0f * sim::kMoveSpeed * sim::kTickDt);

  // Authoritative snapshot at tick 504 (the server has consumed only that
  // one), lead kept on target -> ack_tick = 504 + kTargetLeadTicks = 507.
  // Player 1 is placed far from the predicted position on purpose (but
  // inside the arena -- World::step() clamps to +/-49.5, which 100.0f
  // would immediately hit on the first replayed step, masking the real
  // assertion), so a wrong reconciliation (e.g. not adopting authority, or
  // replaying the wrong range) is unmistakable.
  injectSnapshot(*tp, 504, 5048, onePlayerSnapshot(504, 20.0f, 0.0f, sim::kMoveSpeed, 0.0f),
                 504 + static_cast<uint32_t>(kTargetLeadTicks));
  c.tick(128);  // tick_ 506 -> 507; replay range is (504, 506] = {505, 506}
  ASSERT_EQ(c.clientTick(), 507u);

  ASSERT_TRUE(c.localPosition(x, y));
  EXPECT_EQ(x, 20.0f + 2.0f * sim::kMoveSpeed * sim::kTickDt);
  EXPECT_EQ(y, 0.0f);

  // Gap: advance past tick 507 with no input sent for it, then send one
  // for 508.
  c.tick(144);  // tick_ 507 -> 508, no sendInput for 507
  ASSERT_EQ(c.clientTick(), 508u);
  ASSERT_TRUE(c.sendInput(160, 1.0f, 0.0f, 0.0f, 0.0f, false));  // stamps 508

  // Authoritative snapshot at tick 506, ack_tick = 506 + 3 = 509 (on
  // target again). Player 1 reset to the origin with kMoveSpeed already
  // latched in x, so the replay range (506, 508] = {507, 508} exercises
  // both a miss (507: velocity carries from the authoritative state) and
  // a hit (508: the input just sent).
  injectSnapshot(*tp, 506, 5064, onePlayerSnapshot(506, 0.0f, 0.0f, sim::kMoveSpeed, 0.0f),
                 506 + static_cast<uint32_t>(kTargetLeadTicks));
  c.tick(176);  // tick_ 508 -> 509
  ASSERT_EQ(c.clientTick(), 509u);

  ASSERT_TRUE(c.localPosition(x, y));
  EXPECT_EQ(x, 2.0f * sim::kMoveSpeed * sim::kTickDt);
}

TEST(ClientTest, ReconciliationRecordsThePreCorrectionError) {
  auto tp = std::make_unique<RecordingTransport>();
  Client<RecordingTransport> c(*tp, kServerEp);
  c.beginJoin(0);
  injectJoinAccept(*tp, 1, kJoinSeq, 500);
  c.tick(16);

  injectSnapshot(*tp, 500, 5000, onePlayerSnapshot(500, 0.0f, 0.0f, 0.0f, 0.0f), 503);
  c.tick(32);
  EXPECT_EQ(c.predictionError().samples(), 0u);

  // Predicted stayed at (0, 0) (no sendInput was ever called); authority
  // places it at (3, 4) -- a 3-4-5 triangle, correction magnitude 5.0f.
  injectSnapshot(*tp, 501, 5016, onePlayerSnapshot(501, 3.0f, 4.0f, 0.0f, 0.0f), 503);
  c.tick(48);
  EXPECT_EQ(c.predictionError().samples(), 1u);
  EXPECT_EQ(c.predictionError().worst(), 5.0f);
}

TEST(ClientTest, InputsGoOutAndSnapshotsLandNewestWins) {
  auto tp = std::make_unique<RecordingTransport>();
  Client<RecordingTransport> c(*tp, kServerEp);
  c.beginJoin(0);
  injectJoinAccept(*tp, 4, kJoinSeq);
  c.tick(16);
  ASSERT_EQ(c.playerId(), 4u);
  const size_t sent_before_input = tp->sentCount();

  EXPECT_TRUE(c.sendInput(2000, 1.0f, 0.0f, 0.0f, 1.0f, true));
  ASSERT_EQ(tp->sentCount(), sent_before_input + 1);
  {
    const RecordingTransport::Sent& s = tp->sentAt(sent_before_input);
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

  injectSnapshot(*tp, 30, 5000, twoPlayerSnapshot(30));
  c.tick(2016);
  EXPECT_EQ(c.snapshotsReceived(), 1u);
  EXPECT_EQ(c.latestSnapshotTick(), 30u);
  EXPECT_EQ(c.serverTimeMs(), 5000u);
  ASSERT_EQ(c.latestSnapshot().count, 2u);
  EXPECT_EQ(c.latestSnapshot().players[0].id, 1u);
  EXPECT_EQ(c.latestSnapshot().players[0].x, 1.0f);
  EXPECT_EQ(c.latestSnapshot().players[1].id, 2u);

  // Stale (tick 27 after tick 30) is dropped, but still counted as received.
  injectSnapshot(*tp, 27, 5016, twoPlayerSnapshot(27));
  c.tick(2032);
  EXPECT_EQ(c.snapshotsReceived(), 2u);
  EXPECT_EQ(c.latestSnapshotTick(), 30u);

  // A newer one (tick 33) is adopted.
  injectSnapshot(*tp, 33, 5032, twoPlayerSnapshot(33));
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
    tp->inject(kServerEp, std::span<const std::byte>(buf).subspan(0, written));
  }
  c.tick(2064);
  EXPECT_EQ(c.latestSnapshotTick(), 33u);

  // leave() sends one kLeave and returns to kIdle.
  const size_t sent_before_leave = tp->sentCount();
  c.leave(3000);
  ASSERT_EQ(tp->sentCount(), sent_before_leave + 1);
  {
    const RecordingTransport::Sent& s = tp->sentAt(sent_before_leave);
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

  // Total updates received, not just full snapshots: once this client's
  // inputs start acknowledging a held baseline, the server switches most
  // broadcasts to kSnapshotDelta, so snapshotsReceived() alone undercounts.
  EXPECT_GE(c->snapshotsReceived() + c->deltasApplied(), 15u);
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

TEST(ClientTest, InputAcknowledgesTheNewestSnapshotHeld) {
  auto tp = std::make_unique<RecordingTransport>();
  Client<RecordingTransport> c(*tp, kServerEp);
  c.beginJoin(0);
  injectJoinAccept(*tp, 1, kJoinSeq, 500);
  c.tick(16);

  ASSERT_TRUE(c.sendInput(16, 0.0f, 0.0f, 0.0f, 0.0f, false));
  {
    const RecordingTransport::Sent& s = tp->sentAt(tp->sentCount() - 1);
    net::ByteReader r(std::span<const std::byte>(s.data).subspan(0, s.len));
    net::PacketHeader h;
    ASSERT_TRUE(net::decodeHeader(r, h));
    EXPECT_EQ(h.ack_tick, 0u);
  }

  injectSnapshot(*tp, 77, 5000, onePlayerSnapshot(77, 0.0f, 0.0f, 0.0f, 0.0f), 0);
  c.tick(32);

  ASSERT_TRUE(c.sendInput(48, 0.0f, 0.0f, 0.0f, 0.0f, false));
  {
    const RecordingTransport::Sent& s = tp->sentAt(tp->sentCount() - 1);
    net::ByteReader r(std::span<const std::byte>(s.data).subspan(0, s.len));
    net::PacketHeader h;
    ASSERT_TRUE(net::decodeHeader(r, h));
    EXPECT_EQ(h.ack_tick, 77u);
  }
}

void injectSnapshotDelta(RecordingTransport& tp, const net::PacketHeader& h,
                          std::span<const std::byte> payload) {
  std::array<std::byte, net::kMaxPacket> buf{};
  const size_t written = net::framePacket(h, payload, buf);
  ASSERT_GT(written, 0u);
  tp.inject(kServerEp, std::span<const std::byte>(buf).subspan(0, written));
}

TEST(ClientTest, AppliesADeltaAgainstAHeldBaseline) {
  auto tp = std::make_unique<RecordingTransport>();
  Client<RecordingTransport> c(*tp, kServerEp);
  c.beginJoin(0);
  injectJoinAccept(*tp, 1, kJoinSeq, 500);
  c.tick(16);

  sim::WorldSnapshot baseline{};
  baseline.tick = 100;
  baseline.count = 2;
  baseline.players[0] = {.id = 1, .x = 0.0f, .y = 0.0f, .vx = 0.0f, .vy = 0.0f, .radius = 0.5f};
  baseline.players[1] = {.id = 2, .x = 1.0f, .y = 1.0f, .vx = 0.0f, .vy = 0.0f, .radius = 0.5f};
  injectSnapshot(*tp, 100, 5000, baseline, 0);
  c.tick(32);
  ASSERT_EQ(c.latestSnapshotTick(), 100u);

  sim::WorldSnapshot current = baseline;
  current.tick = 103;
  current.players[1] = {
      .id = 2, .x = 4.0f, .y = 1.0f, .vx = sim::kMoveSpeed, .vy = 0.0f, .radius = 0.5f};

  std::array<std::byte, net::kMaxPacket> delta_payload{};
  net::ByteWriter dw(delta_payload);
  ASSERT_TRUE(net::encodeSnapshotDelta(baseline, current, dw));

  net::PacketHeader dh;
  dh.type = net::MsgType::kSnapshotDelta;
  dh.tick = 103;
  injectSnapshotDelta(*tp, dh, std::span<const std::byte>(delta_payload).subspan(0, dw.size()));
  c.tick(48);

  EXPECT_EQ(c.latestSnapshotTick(), 103u);
  bool found = false;
  for (uint32_t i = 0; i < c.latestSnapshot().count; ++i) {
    if (c.latestSnapshot().players[i].id != 2) continue;
    found = true;
    EXPECT_FLOAT_EQ(c.latestSnapshot().players[i].x, 4.0f);
    EXPECT_FLOAT_EQ(c.latestSnapshot().players[i].vx, sim::kMoveSpeed);
  }
  EXPECT_TRUE(found);
  EXPECT_EQ(c.deltasApplied(), 1u);
  EXPECT_EQ(c.deltasDropped(), 0u);
  EXPECT_NE(c.snapshots().find(103), nullptr);
}

}  // namespace
}  // namespace client
