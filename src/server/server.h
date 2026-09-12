#pragma once

#include <array>
#include <cstdint>
#include <span>

#include "net/bytes.h"
#include "net/framing.h"
#include "net/protocol.h"
#include "net/snapshot_delta.h"
#include "net/snapshot_ring.h"
#include "net/transport.h"
#include "server/input_buffer.h"
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

  // A reference member already blocks copy/move assignment; the copy
  // constructor is not implicitly deleted, and copying would deep-copy
  // World/SessionTable/PacketRing while binding the copy's transport_ to
  // the SAME transport as the original -- two independent simulation
  // states silently sharing one socket. No call site needs this.
  Server(const Server&) = delete;
  Server(Server&&) = delete;

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

  // Drains the ring, consumes one input per live session at the tick this
  // call is about to simulate, steps the world, broadcasts on schedule.
  void tick(uint32_t now_ms) noexcept {
    net::PacketSlot* slot;
    while ((slot = ring_.acquireRead()) != nullptr) {
      route(*slot, now_ms);
      ring_.commitRead();
    }

    const uint32_t next = world_.tick() + 1;

    // Pass 1: apply every live session's input for `next`, if it has one.
    // A miss (underrun) applies nothing -- World::step() then integrates
    // whatever velocity is already latched, which is the intended
    // repeat-last-input behavior, not a bug to paper over.
    std::array<sim::InputCommand, sim::kMaxPlayers> fire_candidates{};
    size_t fire_count = 0;
    for (size_t i = 0; i < sessions_.count(); ++i) {
      const uint32_t id = sessions_.playerAt(i);
      sim::InputCommand in{};
      if (inputs_[id - 1].takeFor(next, in)) {
        world_.applyInput(in);
        if (in.fire) fire_candidates[fire_count++] = in;
      } else {
        ++input_underruns_;
      }
    }

    // Pass 2: resolve every fire only after every session's input for this
    // tick has been applied -- so a hit never depends on session iteration
    // order (the shooter's fire resolving against the target's stale
    // pre-input position).
    for (size_t i = 0; i < fire_count; ++i) {
      const sim::InputCommand& in = fire_candidates[i];
      if (sessions_.tryFire(in.player_id, next) &&
          world_.resolveHitscan(in.player_id, in.aim_x, in.aim_y).has_value()) {
        ++hits_[in.player_id - 1];
      }
    }

    world_.step();

    if (world_.tick() % kSnapshotIntervalTicks == 0) broadcastSnapshot(now_ms);

    std::array<uint32_t, kMaxExpired> expired{};
    const size_t expired_count = sessions_.expire(world_.tick(), expired);
    for (size_t i = 0; i < expired_count; ++i) {
      world_.removePlayer(expired[i]);
      // Clear any future-ticked input this session already queued -- ids
      // are reused (SessionTable::joinOrGet hands out the lowest free id),
      // and InputBuffer::push accepts ticks up to kInputBufferSlots ahead
      // of the server's own progress. Without this, a departed player's
      // still-queued future input would execute under whichever new
      // player inherits the same id, up to ~1s later -- found by the P3
      // Task 8 security review.
      inputs_[expired[i] - 1].reset();
    }
  }

  size_t queuedPackets() const noexcept { return ring_.size(); }
  uint32_t worldTick() const noexcept { return world_.tick(); }
  const sim::World& world() const noexcept { return world_; }
  const SessionTable& sessions() const noexcept { return sessions_; }
  uint32_t playerFor(const net::Endpoint& ep) const noexcept { return sessions_.playerFor(ep); }
  uint64_t hits(uint32_t player_id) const noexcept {
    if (player_id == sim::kInvalidPlayerId || player_id > sim::kMaxPlayers) return 0;
    return hits_[player_id - 1];
  }
  uint64_t droppedPackets() const noexcept { return dropped_; }
  uint64_t ingestOverflows() const noexcept { return ingest_overflows_; }
  uint64_t inputUnderruns() const noexcept { return input_underruns_; }
  uint64_t lateInputs() const noexcept { return late_inputs_; }
  uint64_t deltasSent() const noexcept { return deltas_sent_; }
  uint64_t keyframesSent() const noexcept { return keyframes_sent_; }
  uint64_t snapshotBytesSent() const noexcept { return snapshot_bytes_sent_; }
  uint64_t snapshotBytesFullEquivalent() const noexcept {
    return snapshot_bytes_full_equivalent_;
  }

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
        handleInput(slot.peer, h, r);
        break;
      case net::MsgType::kLeave:
        handleLeave(slot.peer, h);
        break;
      default:
        ++dropped_;
        break;
    }
  }

  void handleLeave(const net::Endpoint& from, const net::PacketHeader& h) noexcept {
    if (h.payload_len != 0) {
      ++dropped_;
      return;
    }
    const uint32_t id = sessions_.playerFor(from);
    if (id == sim::kInvalidPlayerId) {
      ++dropped_;
      return;
    }
    sessions_.remove(from);
    world_.removePlayer(id);
    // See the identical note at the timeout-expiry call site: clears any
    // future-ticked input this session already queued, so a reused id
    // doesn't inherit and execute it under a new, unconsenting owner.
    inputs_[id - 1].reset();
  }

  // Buffers `in` against the tick it is stamped for; does not apply it.
  // Application happens in tick()'s pass 1, at the tick the input names --
  // never at arrival, which is what makes the client's replay reproduce the
  // server's steps exactly.
  void handleInput(const net::Endpoint& from, const net::PacketHeader& h,
                    net::ByteReader& r) noexcept {
    sim::InputCommand in{};
    if (!net::decodeInput(r, in)) {
      ++dropped_;
      return;
    }
    if (!sessions_.authorize(from, in.player_id)) {
      ++dropped_;
      return;
    }
    // Liveness updates unconditionally on a decoded, authorized input --
    // independent of whether the InputBuffer's acceptance window then
    // takes it. The snapshot acknowledgment is likewise recorded only after
    // authorize() succeeds -- an unauthorized sender must not be able to
    // move another session's delta baseline. ack_tick is otherwise
    // unvalidated attacker-controlled data; a legitimate client can only
    // have received a snapshot the server already sent, which is always
    // <= world_.tick(), so a larger value is rejected outright rather than
    // recorded -- without this, a single bogus ack_tick permanently pins
    // the session to full keyframes (every genuine, smaller ack_tick is
    // then monotonically rejected as "older" by noteSnapshotAck).
    sessions_.touch(from, world_.tick(), 0);
    if (h.ack_tick <= world_.tick()) sessions_.noteSnapshotAck(from, h.ack_tick);
    if (inputs_[in.player_id - 1].push(in)) {
      sessions_.touch(from, world_.tick(), in.tick);
    } else {
      ++late_inputs_;
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

  // Encodes and sends the newest snapshot once per session, because each
  // session may hold a different acknowledged baseline. A session whose
  // acknowledged tick still has a matching entry in `history_` gets a delta
  // against it; every other session (never acknowledged, or its baseline
  // has aged out of the ring) gets the full snapshot. Deliberately no
  // group-by-baseline cache: 32 sessions x 20 Hz is 640 encodes/second of a
  // sub-microsecond function, not worth optimizing ahead of P5's own
  // measurements.
  void broadcastSnapshot(uint32_t now_ms) noexcept {
    world_.writeSnapshot(snapshot_);
    history_.store(snapshot_);

    for (size_t i = 0; i < sessions_.count(); ++i) {
      const uint32_t pid = sessions_.playerAt(i);
      const sim::WorldSnapshot* baseline = history_.find(sessions_.ackedSnapshotTick(pid));

      net::ByteWriter pw(snapshot_payload_);
      net::MsgType type = net::MsgType::kSnapshotDelta;
      bool ok = false;
      if (baseline != nullptr && baseline->tick != snapshot_.tick) {
        ok = net::encodeSnapshotDelta(*baseline, snapshot_, pw);
      }
      if (!ok) {
        pw = net::ByteWriter(snapshot_payload_);
        type = net::MsgType::kSnapshot;
        ok = net::encodeSnapshot(snapshot_, pw);
      }
      if (!ok) {
        ++dropped_;
        continue;
      }

      if (type == net::MsgType::kSnapshotDelta) {
        ++deltas_sent_;
      } else {
        ++keyframes_sent_;
      }
      snapshot_bytes_sent_ += pw.size();
      snapshot_bytes_full_equivalent_ +=
          net::kSnapshotFixedBytes + snapshot_.count * net::kPlayerStateBytes;

      net::PacketHeader out_h;
      out_h.type = type;
      out_h.tick = world_.tick();
      out_h.send_time_ms = now_ms;
      out_h.ack_tick = sessions_.lastInputTick(pid);
      const std::span<const std::byte> payload(snapshot_payload_.data(), pw.size());
      sendFramed(sessions_.endpointAt(i), out_h, payload);
    }
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
  std::array<InputBuffer, sim::kMaxPlayers> inputs_{};
  std::array<std::byte, net::kMaxPacket> send_buf_{};
  sim::WorldSnapshot snapshot_{};
  std::array<std::byte, net::kMaxPacket> snapshot_payload_{};
  net::SnapshotRing history_;
  std::array<uint64_t, sim::kMaxPlayers> hits_{};
  uint64_t dropped_ = 0;
  uint64_t ingest_overflows_ = 0;
  uint64_t input_underruns_ = 0;
  uint64_t late_inputs_ = 0;
  uint64_t deltas_sent_ = 0;
  uint64_t keyframes_sent_ = 0;
  uint64_t snapshot_bytes_sent_ = 0;
  uint64_t snapshot_bytes_full_equivalent_ = 0;
};

}  // namespace server
