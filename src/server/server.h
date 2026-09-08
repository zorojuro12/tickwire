#pragma once

#include <array>
#include <cstdint>
#include <span>

#include "net/bytes.h"
#include "net/framing.h"
#include "net/protocol.h"
#include "net/transport.h"
#include "server/packet_ring.h"
#include "server/session.h"
#include "sim/sim.h"
#include "sim/world.h"

namespace server {

inline constexpr uint32_t kSnapshotIntervalTicks = 3;  // 60 Hz sim -> 20 Hz snapshots
inline constexpr size_t kIngestCapacity = 256;         // power of two, per PacketRing

template <net::Transport T>
class Server {
 public:
  explicit Server(T& transport) noexcept : transport_(transport) {}

  // Drains the transport into the ring; returns packets accepted.
  size_t ingest() noexcept {
    size_t accepted = 0;
    for (;;) {
      net::PacketSlot* slot = ring_.acquireWrite();
      if (slot == nullptr) {
        ++ingest_overflows_;
        break;
      }
      if (!transport_.tryReceive(*slot)) break;
      ring_.commitWrite();
      ++accepted;
    }
    return accepted;
  }

  // Drains the ring, steps the world, broadcasts on schedule.
  void tick(uint32_t now_ms) noexcept {
    net::PacketSlot* slot;
    while ((slot = ring_.acquireRead()) != nullptr) {
      route(*slot, now_ms);
      ring_.commitRead();
    }
    world_.step();
  }

  size_t queuedPackets() const noexcept { return ring_.size(); }
  uint32_t worldTick() const noexcept { return world_.tick(); }
  const sim::World& world() const noexcept { return world_; }
  uint32_t playerFor(const net::Endpoint& ep) const noexcept { return sessions_.playerFor(ep); }
  uint64_t hits(uint32_t player_id) const noexcept {
    if (player_id == sim::kInvalidPlayerId || player_id > sim::kMaxPlayers) return 0;
    return hits_[player_id - 1];
  }
  uint64_t droppedPackets() const noexcept { return dropped_; }
  uint64_t ingestOverflows() const noexcept { return ingest_overflows_; }

 private:
  static void spawnPosition(uint32_t player_id, float& x, float& y) noexcept {
    // 8x4 grid inside the arena, fully deterministic.
    const uint32_t i = player_id - 1;
    x = -35.0f + 10.0f * static_cast<float>(i % 8);
    y = -35.0f + 10.0f * static_cast<float>(i / 8);
  }

  void route(const net::PacketSlot& slot, uint32_t now_ms) noexcept {
    net::ByteReader r(std::span<const std::byte>(slot.data).subspan(0, slot.len));
    net::PacketHeader h;
    if (!net::decodeHeader(r, h)) {
      ++dropped_;
      return;
    }
    switch (h.type) {
      case net::MsgType::kJoinRequest:
        handleJoin(slot.peer, h, now_ms);
        break;
      case net::MsgType::kInput:
        handleInput(slot.peer, r);
        break;
      default:
        ++dropped_;
        break;
    }
  }

  void handleInput(const net::Endpoint& from, net::ByteReader& r) noexcept {
    sim::InputCommand in{};
    if (!net::decodeInput(r, in)) {
      ++dropped_;
      return;
    }
    if (!sessions_.authorize(from, in.player_id)) {
      ++dropped_;
      return;
    }
    sessions_.touch(from, world_.tick(), in.tick);
    world_.applyInput(in);
    if (in.fire && sessions_.tryFire(in.player_id, world_.tick())) {
      if (world_.resolveHitscan(in.player_id, in.aim_x, in.aim_y).has_value()) {
        ++hits_[in.player_id - 1];
      }
    }
  }

  void handleJoin(const net::Endpoint& from, const net::PacketHeader& h,
                   uint32_t now_ms) noexcept {
    if (h.payload_len != 0) {
      ++dropped_;
      return;
    }
    const uint32_t id = sessions_.joinOrGet(from, world_.tick());
    if (id == sim::kInvalidPlayerId) {
      sendLeave(from, h.seq, now_ms);
      return;
    }
    float x = 0.0f, y = 0.0f;
    spawnPosition(id, x, y);
    world_.addPlayer(id, x, y);  // no-op if the id is already present
    sendJoinAccept(from, id, h.seq, now_ms);
  }

  void sendJoinAccept(const net::Endpoint& to, uint32_t id, uint16_t ack_seq,
                       uint32_t now_ms) noexcept {
    std::array<std::byte, net::kJoinAcceptBytes> payload{};
    net::ByteWriter pw(payload);
    if (!net::encodeJoinAccept(id, pw)) {
      ++dropped_;
      return;
    }
    net::PacketHeader out_h;
    out_h.type = net::MsgType::kJoinAccept;
    // Replies are framed during the pre-step routing phase of tick(), but
    // report the tick that will be true once this call's step() runs --
    // the tick the reply is actually observed at, not the one before it.
    out_h.tick = world_.tick() + 1;
    out_h.send_time_ms = now_ms;
    out_h.ack_seq = ack_seq;
    sendFramed(to, out_h, payload);
  }

  void sendLeave(const net::Endpoint& to, uint16_t ack_seq, uint32_t now_ms) noexcept {
    net::PacketHeader out_h;
    out_h.type = net::MsgType::kLeave;
    out_h.tick = world_.tick() + 1;
    out_h.send_time_ms = now_ms;
    out_h.ack_seq = ack_seq;
    sendFramed(to, out_h, {});
  }

  void sendFramed(const net::Endpoint& to, net::PacketHeader h,
                   std::span<const std::byte> payload) noexcept {
    const size_t written = net::framePacket(h, payload, send_buf_);
    if (written == 0) {
      ++dropped_;
      return;
    }
    // A send() returning false is not an error -- UDP may drop, and the
    // loopback inbox may be full; count it and continue.
    if (!transport_.send(to, std::span<const std::byte>(send_buf_).subspan(0, written))) {
      ++dropped_;
    }
  }

  T& transport_;
  sim::World world_;
  SessionTable sessions_;
  PacketRing<net::PacketSlot, kIngestCapacity> ring_;
  std::array<std::byte, net::kMaxPacket> send_buf_{};
  std::array<uint64_t, sim::kMaxPlayers> hits_{};
  uint64_t dropped_ = 0;
  uint64_t ingest_overflows_ = 0;
};

}  // namespace server
