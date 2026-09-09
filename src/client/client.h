#pragma once

#include <array>
#include <cstdint>

#include "client/clock_sync.h"
#include "client/prediction.h"
#include "net/bytes.h"
#include "net/framing.h"
#include "net/protocol.h"
#include "net/transport.h"
#include "sim/sim.h"
#include "sim/world.h"

namespace client {

inline constexpr uint32_t kJoinRetryTicks = 15;   // 250 ms at 60 Hz
inline constexpr uint32_t kJoinMaxAttempts = 40;  // ~10 s, then kFailed
inline constexpr uint16_t kJoinSeq = 1;
inline constexpr uint16_t kLeaveSeq = 2;

enum class State : uint8_t { kIdle, kJoining, kJoined, kRejected, kFailed };

template <net::Transport T>
class Client {
 public:
  Client(T& transport, net::Endpoint server) noexcept
      : transport_(transport), server_(server) {}

  // See server::Server's identical note: a reference member blocks copy/move
  // assignment but not the copy constructor, and no call site needs it.
  Client(const Client&) = delete;
  Client(Client&&) = delete;

  void beginJoin(uint32_t now_ms) noexcept {
    state_ = State::kJoining;
    join_attempts_ = 1;
    sendJoinRequest(now_ms);
    next_retry_tick_ = tick_ + kJoinRetryTicks;
  }

  void tick(uint32_t now_ms) noexcept {
    ++tick_;

    net::PacketSlot slot;
    while (transport_.tryReceive(slot)) {
      handlePacket(slot, now_ms);
    }

    tick_ = applyCorrection(tick_, clock_.takeCorrection());

    if (state_ == State::kJoining && tick_ >= next_retry_tick_) {
      if (join_attempts_ >= kJoinMaxAttempts) {
        state_ = State::kFailed;
      } else {
        ++join_attempts_;
        sendJoinRequest(now_ms);
        next_retry_tick_ = tick_ + kJoinRetryTicks;
      }
    }
  }

  bool sendInput(uint32_t now_ms, float move_x, float move_y, float aim_x, float aim_y,
                 bool fire) noexcept {
    if (state_ != State::kJoined) return false;
    const sim::InputCommand in{.player_id = player_id_,
                                .tick = tick_,
                                .move_x = move_x,
                                .move_y = move_y,
                                .aim_x = aim_x,
                                .aim_y = aim_y,
                                .fire = fire};
    std::array<std::byte, net::kInputBytes> payload{};
    net::ByteWriter pw(payload);
    if (!net::encodeInput(in, pw)) return false;

    net::PacketHeader h;
    h.type = net::MsgType::kInput;
    h.tick = tick_;
    h.send_time_ms = now_ms;
    if (!sendFramed(h, payload)) return false;

    pending_.record(in, now_ms);
    if (predicted_ready_ && prediction_enabled_) {
      predicted_.applyInput(in);
      predicted_.step();
    }
    return true;
  }

  void leave(uint32_t now_ms) noexcept {
    net::PacketHeader h;
    h.type = net::MsgType::kLeave;
    h.seq = kLeaveSeq;
    h.send_time_ms = now_ms;
    sendFramed(h, {});
    state_ = State::kIdle;
    player_id_ = sim::kInvalidPlayerId;
  }

  State state() const noexcept { return state_; }
  uint32_t playerId() const noexcept { return player_id_; }
  const sim::WorldSnapshot& latestSnapshot() const noexcept { return snapshot_; }
  uint32_t latestSnapshotTick() const noexcept { return latest_snapshot_tick_; }
  uint32_t serverTimeMs() const noexcept { return server_time_ms_; }
  uint32_t snapshotsReceived() const noexcept { return snapshots_received_; }
  uint32_t joinAttempts() const noexcept { return join_attempts_; }
  uint32_t clientTick() const noexcept { return tick_; }
  int32_t clockLead() const noexcept { return clock_.lead(); }
  uint32_t clockSnaps() const noexcept { return clock_.snaps(); }

  // A pure toggle: the prediction world is seeded once (on the first
  // snapshot that carries this player, in handleSnapshot below) and simply
  // stops or resumes receiving new inputs as this flips. It is not reset,
  // so re-enabling resumes from wherever it already sits rather than
  // waiting on another snapshot -- Task 7's reconciliation is what keeps it
  // synced to authority going forward.
  void setPredictionEnabled(bool on) noexcept { prediction_enabled_ = on; }
  bool predictionEnabled() const noexcept { return prediction_enabled_; }

  // Input-to-snapshot latency: the time from sending an input to the
  // snapshot that acknowledges it. This is NOT a pure network round trip --
  // it includes however long the server's InputBuffer held the input before
  // consuming it (up to kTargetLeadTicks worth) and the snapshot interval
  // (kSnapshotIntervalTicks), so it reads roughly 50-100 ms above the wire
  // RTT at 60 Hz. That is the number that matches what a player actually
  // feels, which is why it is the one shown -- but it must not be labelled
  // "ping".
  uint32_t rttMs() const noexcept { return rtt_ms_; }

  // The local player's position: predicted when prediction is on and the
  // prediction world has been seeded, otherwise straight from the newest
  // snapshot. False when no snapshot has yet carried this player.
  bool localPosition(float& x, float& y) const noexcept {
    if (prediction_enabled_ && predicted_ready_) {
      predicted_.writeSnapshot(predicted_scratch_);
      for (uint32_t i = 0; i < predicted_scratch_.count; ++i) {
        if (predicted_scratch_.players[i].id == player_id_) {
          x = predicted_scratch_.players[i].x;
          y = predicted_scratch_.players[i].y;
          return true;
        }
      }
      return false;
    }
    for (uint32_t i = 0; i < snapshot_.count; ++i) {
      if (snapshot_.players[i].id == player_id_) {
        x = snapshot_.players[i].x;
        y = snapshot_.players[i].y;
        return true;
      }
    }
    return false;
  }

 private:
  void handlePacket(const net::PacketSlot& slot, uint32_t now_ms) noexcept {
    net::ByteReader r(std::span<const std::byte>(slot.data).subspan(0, slot.len));
    net::PacketHeader h;
    if (!net::decodeHeader(r, h)) return;

    switch (h.type) {
      case net::MsgType::kJoinAccept:
        handleJoinAccept(r, h);
        break;
      case net::MsgType::kLeave:
        if (state_ == State::kJoining && h.ack_seq == kJoinSeq) {
          state_ = State::kRejected;
        }
        break;
      case net::MsgType::kSnapshot:
        handleSnapshot(r, h, now_ms);
        break;
      default:
        break;
    }
  }

  void handleJoinAccept(net::ByteReader& r, const net::PacketHeader& h) noexcept {
    if (state_ != State::kJoining || h.ack_seq != kJoinSeq) return;
    uint32_t id = sim::kInvalidPlayerId;
    if (!net::decodeJoinAccept(r, id)) return;
    player_id_ = id;
    state_ = State::kJoined;
    // Place the clock ahead of the server's reported tick, so the client's
    // first input is stamped for a tick the server has not yet simulated.
    // tick() increments tick_ before draining the transport (see tick()
    // below), so this seed is not immediately clobbered by that increment.
    tick_ = h.tick + static_cast<uint32_t>(kTargetLeadTicks);
  }

  void handleSnapshot(net::ByteReader& r, const net::PacketHeader& h, uint32_t now_ms) noexcept {
    sim::WorldSnapshot snap{};
    if (!net::decodeSnapshot(r, snap)) return;
    ++snapshots_received_;
    // Newest wins: adopt only when strictly newer than what is already
    // stored. A decode failure adopts nothing (handled by the early return
    // above, before snapshots_received_ is incremented).
    if (h.tick <= latest_snapshot_tick_) return;
    snapshot_ = snap;
    latest_snapshot_tick_ = h.tick;
    server_time_ms_ = h.send_time_ms;
    clock_.observe(h.tick, h.ack_tick);

    // ack_tick names an input this client sent and (if still pending) still
    // holds the send time for -- RTT is a subtraction, no wire round trip
    // needed. now_ms >= send_time_ms is required: send_time_ms is derived
    // from an attacker-influenced ack_tick, and an unguarded unsigned
    // subtraction would report a ~4-billion-ms round trip.
    if (const PendingInput* p = pending_.find(h.ack_tick); p != nullptr && now_ms >= p->send_time_ms) {
      rtt_ms_ = now_ms - p->send_time_ms;
    }

    if (!predicted_ready_) {
      for (uint32_t i = 0; i < snap.count; ++i) {
        if (snap.players[i].id != player_id_) continue;
        predicted_.addPlayer(player_id_, snap.players[i].x, snap.players[i].y);
        predicted_.setPlayerState(snap.players[i]);
        predicted_ready_ = true;
        break;
      }
    }
  }

  void sendJoinRequest(uint32_t now_ms) noexcept {
    net::PacketHeader h;
    h.type = net::MsgType::kJoinRequest;
    h.seq = kJoinSeq;
    h.send_time_ms = now_ms;
    sendFramed(h, {});
  }

  // Saturates at 0 rather than wrapping -- c is bounded by
  // ClockSync::kMaxCorrectionTicks, but t - (-c) can still underflow a
  // uint32_t early after a join.
  static uint32_t applyCorrection(uint32_t t, int32_t c) noexcept {
    if (c >= 0) return t + static_cast<uint32_t>(c);
    const uint32_t magnitude = static_cast<uint32_t>(-static_cast<int64_t>(c));
    return t < magnitude ? 0u : t - magnitude;
  }

  bool sendFramed(net::PacketHeader h, std::span<const std::byte> payload) noexcept {
    std::array<std::byte, net::kMaxPacket> buf{};
    const size_t written = net::framePacket(h, payload, buf);
    if (written == 0) return false;
    return transport_.send(server_, std::span<const std::byte>(buf).subspan(0, written));
  }

  T& transport_;
  net::Endpoint server_;
  State state_ = State::kIdle;
  uint32_t player_id_ = sim::kInvalidPlayerId;
  uint32_t tick_ = 0;
  uint32_t next_retry_tick_ = 0;
  uint32_t join_attempts_ = 0;
  uint32_t snapshots_received_ = 0;
  uint32_t latest_snapshot_tick_ = 0;
  uint32_t server_time_ms_ = 0;
  sim::WorldSnapshot snapshot_{};
  ClockSync clock_;
  sim::World predicted_;
  PendingInputs pending_;
  bool predicted_ready_ = false;
  bool prediction_enabled_ = true;
  uint32_t rtt_ms_ = 0;
  mutable sim::WorldSnapshot predicted_scratch_{};
};

}  // namespace client
