#include "server/server.h"

#include <array>
#include <memory>
#include <span>

#include <gtest/gtest.h>

#include "net/framing.h"
#include "net/protocol.h"
#include "net/transport.h"
#include "server/rewind.h"
#include "sim/sim.h"
#include "sim/world.h"
#include "support/recording_transport.h"

namespace server {
namespace {

using testsupport::RecordingTransport;

constexpr net::Endpoint kEpA{0x7F000001u, 0x3001u};
constexpr net::Endpoint kEpB{0x7F000001u, 0x3002u};

void injectJoin(RecordingTransport& tp, const net::Endpoint& from, uint16_t seq = 1) {
  net::PacketHeader h;
  h.type = net::MsgType::kJoinRequest;
  h.seq = seq;
  std::array<std::byte, net::kMaxPacket> buf{};
  const size_t written = net::framePacket(h, {}, buf);
  ASSERT_GT(written, 0u);
  tp.inject(from, std::span<const std::byte>(buf).subspan(0, written));
}

// Frames and injects one kInput packet, with both the header's ack_tick and
// the payload's view_tick set explicitly.
void injectShot(RecordingTransport& tp, const net::Endpoint& from, uint32_t player_id,
                 uint32_t input_tick, float move_x, float move_y, float aim_x, float aim_y,
                 bool fire, uint32_t ack_tick, uint32_t view_tick) {
  const sim::InputCommand in{.player_id = player_id,
                              .tick = input_tick,
                              .move_x = move_x,
                              .move_y = move_y,
                              .aim_x = aim_x,
                              .aim_y = aim_y,
                              .fire = fire,
                              .view_tick = view_tick};
  std::array<std::byte, net::kInputBytes> payload{};
  net::ByteWriter pw(payload);
  ASSERT_TRUE(net::encodeInput(in, pw));

  net::PacketHeader h;
  h.type = net::MsgType::kInput;
  h.ack_tick = ack_tick;
  std::array<std::byte, net::kMaxPacket> buf{};
  const size_t written = net::framePacket(h, payload, buf);
  ASSERT_GT(written, 0u);
  tp.inject(from, std::span<const std::byte>(buf).subspan(0, written));
}

bool posOf(const sim::WorldSnapshot& s, uint32_t id, float& x, float& y) {
  for (uint32_t i = 0; i < s.count; ++i) {
    if (s.players[i].id == id) {
      x = s.players[i].x;
      y = s.players[i].y;
      return true;
    }
  }
  return false;
}

// Shooter A (id 1, spawns (-35, -35)) and target B (id 2, spawns (-25, -35))
// join; then for input ticks 2 through 32, B moves move_y = 1 and A stays
// stationary, each tick acking the newest multiple of 3 the server could
// plausibly have already sent. Leaves the server positioned to fire A's shot
// at input tick 33. B9 is B's position at world tick 9 -- the same state
// history_ stored at that tick.
struct MovingTargetScenario {
  std::unique_ptr<RecordingTransport> tp = std::make_unique<RecordingTransport>();
  std::unique_ptr<Server<RecordingTransport>> srv =
      std::make_unique<Server<RecordingTransport>>(*tp);
  float b9x = 0.0f;
  float b9y = 0.0f;
  uint32_t now_ms = 0;

  MovingTargetScenario() {
    injectJoin(*tp, kEpA, 1);
    injectJoin(*tp, kEpB, 2);
    srv->ingest();
    srv->tick(now_ms);  // world tick -> 1

    for (uint32_t input_tick = 2; input_tick <= 32; ++input_tick) {
      const uint32_t ack = (srv->worldTick() / kSnapshotIntervalTicks) * kSnapshotIntervalTicks;
      injectShot(*tp, kEpB, 2, input_tick, 0.0f, 1.0f, 0.0f, 0.0f, false, ack, 0);
      injectShot(*tp, kEpA, 1, input_tick, 0.0f, 0.0f, 0.0f, 0.0f, false, ack, 0);
      srv->ingest();
      now_ms += 16;
      srv->tick(now_ms);

      if (srv->worldTick() == 9) {
        sim::WorldSnapshot snap{};
        srv->world().writeSnapshot(snap);
        EXPECT_TRUE(posOf(snap, 2, b9x, b9y));
      }
    }
  }

  // A's shot at input tick 33, aimed at (target_x, target_y) relative to A's
  // stationary spawn origin, with the given ack_tick/view_tick.
  void fire(float target_x, float target_y, uint32_t ack_tick, uint32_t view_tick) {
    injectShot(*tp, kEpA, 1, 33, 0.0f, 0.0f, target_x - (-35.0f), target_y - (-35.0f), true,
               ack_tick, view_tick);
    srv->ingest();
    now_ms += 16;
    srv->tick(now_ms);
  }
};

TEST(ServerLagCompTest, CompensatedShotHitsWhereTheTargetWasDrawn) {
  {
    MovingTargetScenario s;
    s.fire(s.b9x, s.b9y, /*ack_tick=*/30, /*view_tick=*/9);
    EXPECT_EQ(s.srv->hits(1), 1u);
    EXPECT_EQ(s.srv->shots(1), 1u);
    EXPECT_EQ(s.srv->rewoundShots(), 1u);
    EXPECT_EQ(s.srv->rewindsRejected(), 0u);
  }
  {
    MovingTargetScenario s;
    s.fire(s.b9x, s.b9y, /*ack_tick=*/30, /*view_tick=*/0);
    EXPECT_EQ(s.srv->hits(1), 0u);
    EXPECT_EQ(s.srv->shots(1), 1u);
    EXPECT_EQ(s.srv->rewoundShots(), 0u);
    EXPECT_EQ(s.srv->rewindsRejected(), 0u);
  }
  {
    MovingTargetScenario s;
    float live_x = 0.0f, live_y = 0.0f;
    sim::WorldSnapshot snap{};
    s.srv->world().writeSnapshot(snap);
    ASSERT_TRUE(posOf(snap, 2, live_x, live_y));
    // view_tick = 31 is beyond the shooter's acknowledged snapshot (30).
    s.fire(live_x, live_y, /*ack_tick=*/30, /*view_tick=*/31);
    EXPECT_EQ(s.srv->hits(1), 1u);
    EXPECT_EQ(s.srv->shots(1), 1u);
    EXPECT_EQ(s.srv->rewoundShots(), 0u);
    EXPECT_EQ(s.srv->rewindsRejected(), 1u);
  }
}

TEST(ServerLagCompTest, HitIsConfirmedToTheShooterOnly) {
  {
    MovingTargetScenario s;
    s.tp->clearSent();
    s.fire(s.b9x, s.b9y, /*ack_tick=*/30, /*view_tick=*/9);

    size_t hit_confirms = 0;
    for (size_t i = 0; i < s.tp->sentCount(); ++i) {
      const RecordingTransport::Sent& sent = s.tp->sentAt(i);
      net::ByteReader r(std::span<const std::byte>(sent.data).subspan(0, sent.len));
      net::PacketHeader h;
      ASSERT_TRUE(net::decodeHeader(r, h));
      if (h.type != net::MsgType::kHitConfirm) continue;
      ++hit_confirms;
      EXPECT_EQ(sent.to, kEpA);
      EXPECT_EQ(h.tick, 33u);
      net::HitConfirm hc{};
      ASSERT_TRUE(net::decodeHitConfirm(r, hc));
      EXPECT_EQ(hc.target_id, 2u);
      EXPECT_EQ(hc.fire_tick, 33u);
    }
    EXPECT_EQ(hit_confirms, 1u);
  }
  {
    MovingTargetScenario s;
    s.tp->clearSent();
    s.fire(s.b9x, s.b9y, /*ack_tick=*/30, /*view_tick=*/0);

    size_t hit_confirms = 0;
    for (size_t i = 0; i < s.tp->sentCount(); ++i) {
      const RecordingTransport::Sent& sent = s.tp->sentAt(i);
      net::ByteReader r(std::span<const std::byte>(sent.data).subspan(0, sent.len));
      net::PacketHeader h;
      ASSERT_TRUE(net::decodeHeader(r, h));
      if (h.type == net::MsgType::kHitConfirm) ++hit_confirms;
    }
    EXPECT_EQ(hit_confirms, 0u);
  }
}

}  // namespace
}  // namespace server
