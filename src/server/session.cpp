#include "server/session.h"

namespace server {

int SessionTable::findByEndpoint(const net::Endpoint& from) const noexcept {
  for (uint32_t i = 0; i < count_; ++i) {
    if (entries_[i].peer == from) return static_cast<int>(i);
  }
  return -1;
}

int SessionTable::findByPlayer(uint32_t player_id) const noexcept {
  if (player_id == sim::kInvalidPlayerId) return -1;
  for (uint32_t i = 0; i < count_; ++i) {
    if (entries_[i].player_id == player_id) return static_cast<int>(i);
  }
  return -1;
}

uint32_t SessionTable::joinOrGet(const net::Endpoint& from, uint32_t now_tick) {
  int existing = findByEndpoint(from);
  if (existing >= 0) return entries_[static_cast<uint32_t>(existing)].player_id;
  if (count_ == sim::kMaxPlayers) return sim::kInvalidPlayerId;

  // Lowest unused id in [1, kMaxPlayers].
  uint32_t new_id = sim::kInvalidPlayerId;
  for (uint32_t candidate = 1; candidate <= sim::kMaxPlayers; ++candidate) {
    if (findByPlayer(candidate) < 0) {
      new_id = candidate;
      break;
    }
  }

  Entry& e = entries_[count_];
  e.peer = from;
  e.player_id = new_id;
  e.last_seen_tick = now_tick;
  e.last_input_tick = 0;
  e.last_fire_tick = 0;
  e.live = true;
  ++count_;
  return new_id;
}

uint32_t SessionTable::playerFor(const net::Endpoint& from) const noexcept {
  int slot = findByEndpoint(from);
  if (slot < 0) return sim::kInvalidPlayerId;
  return entries_[static_cast<uint32_t>(slot)].player_id;
}

bool SessionTable::endpointFor(uint32_t player_id, net::Endpoint& out) const noexcept {
  int slot = findByPlayer(player_id);
  if (slot < 0) return false;
  out = entries_[static_cast<uint32_t>(slot)].peer;
  return true;
}

bool SessionTable::authorize(const net::Endpoint& from, uint32_t player_id) const noexcept {
  return player_id != sim::kInvalidPlayerId && playerFor(from) == player_id;
}

void SessionTable::touch(const net::Endpoint& from, uint32_t now_tick,
                          uint32_t input_tick) noexcept {
  int slot = findByEndpoint(from);
  if (slot < 0) return;
  Entry& e = entries_[static_cast<uint32_t>(slot)];
  e.last_seen_tick = now_tick;
  if (input_tick > e.last_input_tick) e.last_input_tick = input_tick;
}

uint32_t SessionTable::lastInputTick(uint32_t player_id) const noexcept {
  int slot = findByPlayer(player_id);
  if (slot < 0) return 0;
  return entries_[static_cast<uint32_t>(slot)].last_input_tick;
}

void SessionTable::noteSnapshotAck(const net::Endpoint& from, uint32_t snapshot_tick) noexcept {
  int slot = findByEndpoint(from);
  if (slot < 0) return;
  Entry& e = entries_[static_cast<uint32_t>(slot)];
  if (snapshot_tick > e.acked_snapshot_tick) e.acked_snapshot_tick = snapshot_tick;
}

uint32_t SessionTable::ackedSnapshotTick(uint32_t player_id) const noexcept {
  int slot = findByPlayer(player_id);
  if (slot < 0) return 0;
  return entries_[static_cast<uint32_t>(slot)].acked_snapshot_tick;
}

bool SessionTable::tryFire(uint32_t player_id, uint32_t now_tick) noexcept {
  int slot = findByPlayer(player_id);
  if (slot < 0) return false;
  Entry& e = entries_[static_cast<uint32_t>(slot)];
  if (e.ever_fired && now_tick - e.last_fire_tick < kFireCooldownTicks) return false;
  e.last_fire_tick = now_tick;
  e.ever_fired = true;
  return true;
}

bool SessionTable::remove(const net::Endpoint& from) noexcept {
  int slot = findByEndpoint(from);
  if (slot < 0) return false;
  removeAt(static_cast<size_t>(slot));
  return true;
}

size_t SessionTable::expire(uint32_t now_tick, std::span<uint32_t> out) noexcept {
  size_t written = 0;
  for (uint32_t i = 0; i < count_;) {
    uint32_t elapsed = now_tick >= entries_[i].last_seen_tick
                            ? now_tick - entries_[i].last_seen_tick
                            : 0;
    if (elapsed >= kSessionTimeoutTicks) {
      if (written < out.size()) out[written] = entries_[i].player_id;
      ++written;
      removeAt(i);
      // removeAt swapped the last live entry into slot i; re-check it.
    } else {
      ++i;
    }
  }
  return written;
}

void SessionTable::removeAt(size_t slot) noexcept {
  uint32_t last = count_ - 1;
  entries_[slot] = entries_[last];
  entries_[last] = Entry{};
  --count_;
}

uint32_t SessionTable::count() const noexcept { return count_; }

net::Endpoint SessionTable::endpointAt(size_t i) const noexcept { return entries_[i].peer; }

uint32_t SessionTable::playerAt(size_t i) const noexcept { return entries_[i].player_id; }

}  // namespace server
