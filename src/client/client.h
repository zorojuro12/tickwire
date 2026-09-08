#pragma once

#include <array>
#include <cstdint>

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
      handlePacket(slot);
    }

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
    return sendFramed(h, payload);
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

 private:
  void handlePacket(const net::PacketSlot& slot) noexcept {
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
        handleSnapshot(r, h);
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
  }

  void handleSnapshot(net::ByteReader& r, const net::PacketHeader& h) noexcept {
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
  }

  void sendJoinRequest(uint32_t now_ms) noexcept {
    net::PacketHeader h;
    h.type = net::MsgType::kJoinRequest;
    h.seq = kJoinSeq;
    h.send_time_ms = now_ms;
    sendFramed(h, {});
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
};

}  // namespace client
