#include "server/server.h"

#include <array>
#include <limits>
#include <memory>
#include <span>

#include <gtest/gtest.h>

#include "net/framing.h"
#include "net/loopback.h"
#include "net/protocol.h"
#include "net/transport.h"
#include "sim/sim.h"
#include "sim/world.h"
#include "support/recording_transport.h"

namespace server {
namespace {

using testsupport::RecordingTransport;

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

// Frames and injects one kInput packet from `from`, claiming `player_id`.
void injectInput(RecordingTransport& tp, const net::Endpoint& from, uint32_t player_id,
                  float move_x, float move_y, float aim_x, float aim_y, bool fire,
                  uint32_t tick) {
  const sim::InputCommand in{.player_id = player_id,
                              .tick = tick,
                              .move_x = move_x,
                              .move_y = move_y,
                              .aim_x = aim_x,
                              .aim_y = aim_y,
                              .fire = fire};
  std::array<std::byte, net::kInputBytes> payload{};
  net::ByteWriter pw(payload);
  ASSERT_TRUE(net::encodeInput(in, pw));

  net::PacketHeader h;
  h.type = net::MsgType::kInput;
  std::array<std::byte, net::kMaxPacket> buf{};
  const size_t written = net::framePacket(h, payload, buf);
  ASSERT_GT(written, 0u);
  tp.inject(from, std::span<const std::byte>(buf).subspan(0, written));
}

void injectJoin(RecordingTransport& tp, const net::Endpoint& from, uint16_t seq = 1) {
  net::PacketHeader h;
  h.type = net::MsgType::kJoinRequest;
  h.seq = seq;
  std::array<std::byte, net::kMaxPacket> buf{};
  const size_t written = net::framePacket(h, {}, buf);
  ASSERT_GT(written, 0u);
  tp.inject(from, std::span<const std::byte>(buf).subspan(0, written));
}

TEST(ServerTest, InputAppliesAtItsStampedTickNotOnArrival) {
  auto tp = std::make_unique<RecordingTransport>();
  auto srv = std::make_unique<Server<RecordingTransport>>(*tp);

  constexpr net::Endpoint kEpA{0x7F000001u, 0x2050u};
  injectJoin(*tp, kEpA, 1);
  srv->ingest();
  srv->tick(0);
  ASSERT_EQ(srv->playerFor(kEpA), 1u);

  sim::WorldSnapshot snap{};
  const sim::PlayerState* findP1 = nullptr;

  // Input stamped for tick 3 -- two ticks in the future.
  injectInput(*tp, kEpA, 1, 1.0f, 0.0f, 0.0f, 0.0f, false, 3);
  srv->ingest();

  srv->tick(16);  // simulates tick 2 -- the stamped input is not due yet
  snap = {};
  srv->world().writeSnapshot(snap);
  findP1 = nullptr;
  for (uint32_t i = 0; i < snap.count; ++i) {
    if (snap.players[i].id == 1) findP1 = &snap.players[i];
  }
  ASSERT_NE(findP1, nullptr);
  EXPECT_EQ(findP1->x, -35.0f);
  EXPECT_EQ(findP1->vx, 0.0f);
  // 2, not 1: the join tick's own tick(0) call already counted one
  // underrun for player 1 (it has no input the moment it joins), plus
  // this tick(16) call's own underrun since the stamped input (tick 3)
  // is not due yet.
  EXPECT_EQ(srv->inputUnderruns(), 2u);

  srv->tick(32);  // simulates tick 3 -- the stamped input applies now
  snap = {};
  srv->world().writeSnapshot(snap);
  findP1 = nullptr;
  for (uint32_t i = 0; i < snap.count; ++i) {
    if (snap.players[i].id == 1) findP1 = &snap.players[i];
  }
  ASSERT_NE(findP1, nullptr);
  EXPECT_EQ(findP1->vx, sim::kMoveSpeed);
  EXPECT_EQ(findP1->x, -35.0f + sim::kMoveSpeed * sim::kTickDt);
}

TEST(ServerTest, MissingInputRepeatsTheLastVelocity) {
  auto tp = std::make_unique<RecordingTransport>();
  auto srv = std::make_unique<Server<RecordingTransport>>(*tp);

  constexpr net::Endpoint kEpA{0x7F000001u, 0x2051u};
  injectJoin(*tp, kEpA, 1);
  srv->ingest();
  srv->tick(0);  // world at tick 1

  auto findP1 = [&] {
    sim::WorldSnapshot snap{};
    srv->world().writeSnapshot(snap);
    for (uint32_t i = 0; i < snap.count; ++i) {
      if (snap.players[i].id == 1) return snap.players[i];
    }
    return sim::PlayerState{};
  };

  // Accumulated the same way World::step() does (repeated += vx*dt, not a
  // closed-form multiplication) -- three separate additions round
  // differently than one multiplication by 3 under IEEE-754, even though
  // both print as the same value.
  float expected_x = -35.0f;

  injectInput(*tp, kEpA, 1, 1.0f, 0.0f, 0.0f, 0.0f, false, 2);
  srv->ingest();
  srv->tick(16);  // simulates tick 2 -- input applies
  expected_x = expected_x + sim::kMoveSpeed * sim::kTickDt;
  {
    sim::PlayerState p1 = findP1();
    EXPECT_EQ(p1.vx, sim::kMoveSpeed);
    EXPECT_EQ(p1.x, expected_x);
  }

  const uint64_t underruns_before = srv->inputUnderruns();
  srv->tick(32);  // simulates tick 3 -- no input; repeats last velocity
  expected_x = expected_x + sim::kMoveSpeed * sim::kTickDt;
  srv->tick(48);  // simulates tick 4 -- same
  expected_x = expected_x + sim::kMoveSpeed * sim::kTickDt;
  {
    sim::PlayerState p1 = findP1();
    EXPECT_EQ(p1.vx, sim::kMoveSpeed);
    EXPECT_EQ(p1.x, expected_x);
  }
  EXPECT_EQ(srv->inputUnderruns(), underruns_before + 2);

  injectInput(*tp, kEpA, 1, 0.0f, 0.0f, 0.0f, 0.0f, false, 5);
  srv->ingest();
  srv->tick(64);  // simulates tick 5 -- explicit stop is honored
  {
    sim::PlayerState p1 = findP1();
    EXPECT_EQ(p1.vx, 0.0f);
    EXPECT_EQ(p1.x, expected_x);  // unchanged: this tick's velocity is 0
  }
}

TEST(ServerTest, InputsMoveOnlyThePlayerTheSenderOwns) {
  auto tp = std::make_unique<RecordingTransport>();
  auto srv = std::make_unique<Server<RecordingTransport>>(*tp);

  constexpr net::Endpoint kEpA{0x7F000001u, 0x2000u};
  constexpr net::Endpoint kEpB{0x7F000001u, 0x2001u};
  constexpr net::Endpoint kEpC{0x7F000001u, 0x2002u};

  injectJoin(*tp, kEpA, 1);
  injectJoin(*tp, kEpB, 2);
  srv->ingest();
  srv->tick(0);
  ASSERT_EQ(srv->playerFor(kEpA), 1u);
  ASSERT_EQ(srv->playerFor(kEpB), 2u);

  // A legitimate input from epA moves player 1 only.
  injectInput(*tp, kEpA, 1, 1.0f, 0.0f, 0.0f, 0.0f, false, 2);
  srv->ingest();
  srv->tick(16);
  {
    sim::WorldSnapshot snap{};
    srv->world().writeSnapshot(snap);
    const sim::PlayerState* p1 = nullptr;
    const sim::PlayerState* p2 = nullptr;
    for (uint32_t i = 0; i < snap.count; ++i) {
      if (snap.players[i].id == 1) p1 = &snap.players[i];
      if (snap.players[i].id == 2) p2 = &snap.players[i];
    }
    ASSERT_NE(p1, nullptr);
    ASSERT_NE(p2, nullptr);
    EXPECT_EQ(p1->vx, sim::kMoveSpeed);
    EXPECT_EQ(p1->x, -35.0f + sim::kMoveSpeed * sim::kTickDt);
    EXPECT_EQ(p2->vx, 0.0f);
    EXPECT_EQ(p2->x, -25.0f);
  }

  // Spoof: epA claims to be player 2.
  const uint64_t dropped_before_spoof = srv->droppedPackets();
  injectInput(*tp, kEpA, 2, 1.0f, 0.0f, 0.0f, 0.0f, false, 3);
  srv->ingest();
  srv->tick(32);
  {
    sim::WorldSnapshot snap{};
    srv->world().writeSnapshot(snap);
    const sim::PlayerState* p1 = nullptr;
    const sim::PlayerState* p2 = nullptr;
    for (uint32_t i = 0; i < snap.count; ++i) {
      if (snap.players[i].id == 1) p1 = &snap.players[i];
      if (snap.players[i].id == 2) p2 = &snap.players[i];
    }
    ASSERT_NE(p1, nullptr);
    ASSERT_NE(p2, nullptr);
    EXPECT_EQ(p2->vx, 0.0f);
    EXPECT_EQ(p2->x, -25.0f);
    EXPECT_EQ(p1->vx, sim::kMoveSpeed);  // player 1 keeps its latched velocity
  }
  EXPECT_EQ(srv->droppedPackets(), dropped_before_spoof + 1);

  // An input from an endpoint with no session at all, claiming player 1.
  const uint64_t dropped_before_unknown = srv->droppedPackets();
  const float p1_x_before = [&] {
    sim::WorldSnapshot snap{};
    srv->world().writeSnapshot(snap);
    for (uint32_t i = 0; i < snap.count; ++i) {
      if (snap.players[i].id == 1) return snap.players[i].x;
    }
    return 0.0f;
  }();
  injectInput(*tp, kEpC, 1, -1.0f, 0.0f, 0.0f, 0.0f, false, 4);
  srv->ingest();
  srv->tick(48);
  {
    // The dropped packet must not touch player 1's velocity: it keeps
    // moving at its already-latched +x speed, not the rejected -1 input.
    sim::WorldSnapshot snap{};
    srv->world().writeSnapshot(snap);
    for (uint32_t i = 0; i < snap.count; ++i) {
      if (snap.players[i].id == 1) {
        EXPECT_EQ(snap.players[i].vx, sim::kMoveSpeed);
        EXPECT_EQ(snap.players[i].x, p1_x_before + sim::kMoveSpeed * sim::kTickDt);
      }
    }
  }
  EXPECT_EQ(srv->droppedPackets(), dropped_before_unknown + 1);

  // Malformed payload: 23 bytes (payload_len disagrees with the frame).
  const uint64_t dropped_before_malformed = srv->droppedPackets();
  {
    net::PacketHeader h;
    h.type = net::MsgType::kInput;
    std::array<std::byte, 23> short_payload{};
    std::array<std::byte, net::kMaxPacket> buf{};
    const size_t written = net::framePacket(h, short_payload, buf);
    ASSERT_GT(written, 0u);
    tp->inject(kEpA, std::span<const std::byte>(buf).subspan(0, written));
  }
  srv->ingest();
  srv->tick(64);
  EXPECT_EQ(srv->droppedPackets(), dropped_before_malformed + 1);

  // A well-framed 25-byte payload with a NaN move_x.
  const uint64_t dropped_before_nan = srv->droppedPackets();
  {
    const sim::InputCommand in{.player_id = 1,
                                .tick = 0,
                                .move_x = std::numeric_limits<float>::quiet_NaN(),
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
    tp->inject(kEpA, std::span<const std::byte>(buf).subspan(0, written));
  }
  srv->ingest();
  srv->tick(80);
  EXPECT_EQ(srv->droppedPackets(), dropped_before_nan + 1);

  // Wrong magic header.
  const uint64_t dropped_before_magic = srv->droppedPackets();
  {
    std::array<std::byte, net::kHeaderBytes> bad{};
    tp->inject(kEpA, bad);
  }
  srv->ingest();
  srv->tick(96);
  EXPECT_EQ(srv->droppedPackets(), dropped_before_magic + 1);

  // A 3-byte packet.
  const uint64_t dropped_before_short = srv->droppedPackets();
  {
    std::array<std::byte, 3> tiny{};
    tp->inject(kEpA, tiny);
  }
  srv->ingest();
  srv->tick(112);
  EXPECT_EQ(srv->droppedPackets(), dropped_before_short + 1);
}

TEST(ServerTest, FiringHitsTheNearestPlayerAlongTheAimAndIsRateLimited) {
  auto tp = std::make_unique<RecordingTransport>();
  auto srv = std::make_unique<Server<RecordingTransport>>(*tp);

  constexpr net::Endpoint kEpA{0x7F000001u, 0x2010u};
  constexpr net::Endpoint kEpB{0x7F000001u, 0x2011u};

  injectJoin(*tp, kEpA, 1);
  injectJoin(*tp, kEpB, 2);
  srv->ingest();
  srv->tick(0);
  ASSERT_EQ(srv->playerFor(kEpA), 1u);
  ASSERT_EQ(srv->playerFor(kEpB), 2u);

  // Player 1 spawns at (-35, -35); player 2 spawns at (-25, -35) -- directly
  // along +x from player 1, per the 8x4 spawn grid.
  EXPECT_EQ(srv->hits(1), 0u);

  injectInput(*tp, kEpA, 1, 0.0f, 0.0f, 1.0f, 0.0f, true, 2);
  srv->ingest();
  srv->tick(16);
  EXPECT_EQ(srv->hits(1), 1u);

  // A second shot one tick later is still on cooldown.
  injectInput(*tp, kEpA, 1, 0.0f, 0.0f, 1.0f, 0.0f, true, 3);
  srv->ingest();
  srv->tick(32);
  EXPECT_EQ(srv->hits(1), 1u);

  // Clear the cooldown, then fire a miss (aiming away): hits stays unchanged.
  for (int i = 0; i < 15; ++i) srv->tick(48 + i * 16);
  injectInput(*tp, kEpA, 1, 0.0f, 0.0f, -1.0f, 0.0f, true, 19);
  srv->ingest();
  srv->tick(300);
  EXPECT_EQ(srv->hits(1), 1u);
}

// World::step() -- the sole place a position integrates -- runs after both
// tick()'s apply pass and its fire-resolution pass. So a shot always
// evaluates against positions as of the START of the tick, never against
// any movement submitted for that same tick, from the shooter or the
// target. This test pins that: if step() were ever called before or
// between the two passes, this would start passing (a false hit) instead
// of failing to hit.
TEST(ServerTest, FireResolvesAgainstPositionsFromBeforeThisTicksMovement) {
  auto tp = std::make_unique<RecordingTransport>();
  auto srv = std::make_unique<Server<RecordingTransport>>(*tp);

  constexpr net::Endpoint kEpA{0x7F000001u, 0x2052u};  // shooter, player 1
  constexpr net::Endpoint kEpB{0x7F000001u, 0x2053u};  // target, player 2

  injectJoin(*tp, kEpA, 1);
  injectJoin(*tp, kEpB, 2);
  srv->ingest();
  srv->tick(0);  // world at tick 1; both spawn on the same row (y = -35)

  // Move player 2 off player 1's aim line, clear of the hitscan radius
  // (0.5): 4 ticks * kMoveSpeed * kTickDt =~ 0.533, just past it.
  uint32_t tick = 2;
  uint32_t now_ms = 16;
  for (int i = 0; i < 4; ++i) {
    injectInput(*tp, kEpB, 2, 0.0f, 1.0f, 0.0f, 0.0f, false, tick);
    srv->ingest();
    srv->tick(now_ms);
    ++tick;
    now_ms += 16;
  }
  EXPECT_EQ(srv->hits(1), 0u);

  // Same tick: player 1 fires along +x, and player 2 submits a downward
  // move that -- if it took effect before the shot resolved -- would bring
  // it back inside the radius. It must not: the shot evaluates against
  // player 2's pre-tick position and misses.
  injectInput(*tp, kEpA, 1, 0.0f, 0.0f, 1.0f, 0.0f, true, tick);
  injectInput(*tp, kEpB, 2, 0.0f, -1.0f, 0.0f, 0.0f, false, tick);
  srv->ingest();
  srv->tick(now_ms);

  EXPECT_EQ(srv->hits(1), 0u);
}

TEST(ServerTest, SnapshotsGoOutAt20HzToEveryLiveSession) {
  auto tp = std::make_unique<RecordingTransport>();
  auto srv = std::make_unique<Server<RecordingTransport>>(*tp);

  constexpr net::Endpoint kEpA{0x7F000001u, 0x2020u};
  constexpr net::Endpoint kEpB{0x7F000001u, 0x2021u};
  injectJoin(*tp, kEpA, 1);
  injectJoin(*tp, kEpB, 2);
  srv->ingest();
  srv->tick(0);
  tp->clearSent();

  // An accepted input with tick=5 from epA -- ahead of the tick this
  // tick(16) call actually simulates (2), so it is pushed but not yet
  // consumed; ack_tick (the highest tick *accepted*, not consumed) reaches 5
  // immediately.
  injectInput(*tp, kEpA, 1, 0.0f, 0.0f, 0.0f, 0.0f, false, 5);
  srv->ingest();
  srv->tick(16);
  // Reordered: an accepted input with an older tick (3 < 5, but still ahead
  // of the buffer's consumption floor) must not walk ack_tick backwards.
  injectInput(*tp, kEpA, 1, 0.0f, 0.0f, 0.0f, 0.0f, false, 3);
  srv->ingest();

  size_t snapshot_count = 0;
  for (int i = 0; i < 11; ++i) {
    const size_t before = tp->sentCount();
    srv->tick(32 + i * 16);
    const size_t sent_this_tick = tp->sentCount() - before;
    if (srv->worldTick() % kSnapshotIntervalTicks == 0) {
      EXPECT_EQ(sent_this_tick, 2u) << "worldTick " << srv->worldTick();
    } else {
      EXPECT_EQ(sent_this_tick, 0u) << "worldTick " << srv->worldTick();
    }
    snapshot_count += sent_this_tick;
  }
  // 12 total tick() calls after clearSent (the one above plus these 11);
  // worldTick runs 1 -> 13 across them, crossing a multiple of
  // kSnapshotIntervalTicks (3, 6, 9, 12) exactly 4 times: 2 * (12 / 3) == 8.
  EXPECT_EQ(snapshot_count, 8u);

  ASSERT_GT(tp->sentCount(), 0u);
  bool found_a = false, found_b = false;
  for (size_t i = 0; i < tp->sentCount(); ++i) {
    const RecordingTransport::Sent& s = tp->sentAt(i);
    net::ByteReader r(std::span<const std::byte>(s.data).subspan(0, s.len));
    net::PacketHeader h;
    ASSERT_TRUE(net::decodeHeader(r, h));
    EXPECT_EQ(h.type, net::MsgType::kSnapshot);
    EXPECT_EQ(h.version, net::kProtocolVersion);
    EXPECT_EQ(h.payload_len, 8u + 2u * net::kPlayerStateBytes);
    EXPECT_EQ(h.seq, 0u);

    sim::WorldSnapshot snap{};
    ASSERT_TRUE(net::decodeSnapshot(r, snap));
    EXPECT_EQ(snap.count, 2u);
    EXPECT_EQ(h.tick, snap.tick);

    if (s.to == kEpA) {
      EXPECT_EQ(h.ack_tick, 5u);
      found_a = true;
    } else if (s.to == kEpB) {
      EXPECT_EQ(h.ack_tick, 0u);
      found_b = true;
    }
  }
  EXPECT_TRUE(found_a);
  EXPECT_TRUE(found_b);
}

TEST(ServerTest, NoSessionsMeansNoSnapshotButTheWorldStillTicks) {
  auto tp = std::make_unique<RecordingTransport>();
  auto srv = std::make_unique<Server<RecordingTransport>>(*tp);

  srv->tick(0);
  srv->tick(16);
  srv->tick(32);
  EXPECT_EQ(tp->sentCount(), 0u);
  EXPECT_EQ(srv->worldTick(), 3u);
}

void injectLeave(RecordingTransport& tp, const net::Endpoint& from) {
  net::PacketHeader h;
  h.type = net::MsgType::kLeave;
  std::array<std::byte, net::kMaxPacket> buf{};
  const size_t written = net::framePacket(h, {}, buf);
  ASSERT_GT(written, 0u);
  tp.inject(from, std::span<const std::byte>(buf).subspan(0, written));
}

TEST(ServerTest, LeaveRemovesThePlayerAndSendsNoReply) {
  auto tp = std::make_unique<RecordingTransport>();
  auto srv = std::make_unique<Server<RecordingTransport>>(*tp);

  constexpr net::Endpoint kEpA{0x7F000001u, 0x2030u};
  constexpr net::Endpoint kEpB{0x7F000001u, 0x2031u};
  injectJoin(*tp, kEpA, 1);
  injectJoin(*tp, kEpB, 2);
  srv->ingest();
  srv->tick(0);
  ASSERT_EQ(srv->world().playerCount(), 2u);
  const uint32_t id_b = srv->playerFor(kEpB);
  tp->clearSent();

  injectLeave(*tp, kEpA);
  srv->ingest();
  srv->tick(16);

  EXPECT_EQ(srv->world().playerCount(), 1u);
  EXPECT_EQ(srv->playerFor(kEpA), 0u);
  EXPECT_TRUE(srv->world().hasPlayer(id_b));

  for (size_t i = 0; i < tp->sentCount(); ++i) {
    EXPECT_NE(tp->sentAt(i).to, kEpA) << "no reply should go to a departed endpoint";
  }
}

TEST(ServerTest, LeaveFromAnUnknownEndpointIsDropped) {
  auto tp = std::make_unique<RecordingTransport>();
  auto srv = std::make_unique<Server<RecordingTransport>>(*tp);
  constexpr net::Endpoint kEpA{0x7F000001u, 0x2032u};
  injectJoin(*tp, kEpA, 1);
  srv->ingest();
  srv->tick(0);
  ASSERT_EQ(srv->world().playerCount(), 1u);

  constexpr net::Endpoint kUnknown{0x7F000001u, 0x2033u};
  const uint64_t dropped_before = srv->droppedPackets();
  injectLeave(*tp, kUnknown);
  srv->ingest();
  srv->tick(16);

  EXPECT_EQ(srv->droppedPackets(), dropped_before + 1);
  EXPECT_EQ(srv->world().playerCount(), 1u);
}

TEST(ServerTest, SilentSessionsTimeOutAndTheFreedIdIsReusable) {
  auto tp = std::make_unique<RecordingTransport>();
  auto srv = std::make_unique<Server<RecordingTransport>>(*tp);

  constexpr net::Endpoint kEpA{0x7F000001u, 0x2040u};
  constexpr net::Endpoint kEpB{0x7F000001u, 0x2041u};
  injectJoin(*tp, kEpA, 1);
  injectJoin(*tp, kEpB, 2);
  srv->ingest();
  srv->tick(0);
  ASSERT_EQ(srv->world().playerCount(), 2u);
  const uint32_t id_a = srv->playerFor(kEpA);
  const uint32_t id_b = srv->playerFor(kEpB);

  uint32_t now_ms = 16;
  for (uint32_t i = 0; i < kSessionTimeoutTicks; ++i) {
    injectInput(*tp, kEpB, id_b, 0.0f, 0.0f, 0.0f, 0.0f, false, i);
    srv->ingest();
    srv->tick(now_ms);
    now_ms += 16;
  }

  EXPECT_EQ(srv->world().playerCount(), 1u);
  EXPECT_EQ(srv->playerFor(kEpA), 0u);
  EXPECT_FALSE(srv->world().hasPlayer(id_a));
  EXPECT_EQ(srv->playerFor(kEpB), id_b);
  EXPECT_TRUE(srv->world().hasPlayer(id_b));

  // A rejoin after timeout works; the id may differ.
  injectJoin(*tp, kEpA, 5);
  srv->ingest();
  srv->tick(now_ms);
  EXPECT_EQ(srv->world().playerCount(), 2u);
  EXPECT_NE(srv->playerFor(kEpA), 0u);

  // The freed id (epA's original) is reusable by the next joiner.
  constexpr net::Endpoint kEpC{0x7F000001u, 0x2042u};
  injectLeave(*tp, kEpA);
  srv->ingest();
  srv->tick(now_ms + 16);
  injectJoin(*tp, kEpC, 1);
  srv->ingest();
  srv->tick(now_ms + 32);
  EXPECT_EQ(srv->playerFor(kEpC), id_a);
}

}  // namespace
}  // namespace server
