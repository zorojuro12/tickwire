#include "client/client.h"

#include <array>
#include <cstdint>

#include <gtest/gtest.h>

#include "net/framing.h"
#include "net/protocol.h"
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

}  // namespace
}  // namespace client
