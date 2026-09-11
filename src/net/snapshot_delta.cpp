#include "net/snapshot_delta.h"

namespace net {
namespace {

const sim::PlayerState* findById(const sim::WorldSnapshot& s, uint32_t id) {
  for (uint32_t i = 0; i < s.count; ++i) {
    if (s.players[i].id == id) return &s.players[i];
  }
  return nullptr;
}

bool sameFields(const sim::PlayerState& a, const sim::PlayerState& b) {
  return a.x == b.x && a.y == b.y && a.vx == b.vx && a.vy == b.vy && a.radius == b.radius;
}

bool validIdsAndCount(const sim::WorldSnapshot& s) {
  if (s.count > sim::kMaxPlayers) return false;
  for (uint32_t i = 0; i < s.count; ++i) {
    const uint32_t id = s.players[i].id;
    if (id < 1 || id > sim::kMaxPlayers) return false;
  }
  return true;
}

}  // namespace

bool encodeSnapshotDelta(const sim::WorldSnapshot& baseline, const sim::WorldSnapshot& current,
                          ByteWriter& w) {
  if (!validIdsAndCount(baseline) || !validIdsAndCount(current)) return false;

  uint32_t present_mask = 0;
  uint32_t changed_mask = 0;
  for (uint32_t i = 0; i < current.count; ++i) {
    const sim::PlayerState& p = current.players[i];
    present_mask |= (1u << (p.id - 1));
    const sim::PlayerState* base = findById(baseline, p.id);
    if (base == nullptr || !sameFields(*base, p)) {
      changed_mask |= (1u << (p.id - 1));
    }
  }

  w.u32(current.tick);
  w.u32(baseline.tick);
  w.u32(present_mask);
  w.u32(changed_mask);

  for (uint32_t id = 1; id <= sim::kMaxPlayers; ++id) {
    if ((changed_mask & (1u << (id - 1))) == 0) continue;
    const sim::PlayerState* p = findById(current, id);
    w.f32(p->x);
    w.f32(p->y);
    w.f32(p->vx);
    w.f32(p->vy);
    w.f32(p->radius);
  }

  return w.ok();
}

bool decodeSnapshotDelta(ByteReader& r, SnapshotDelta& out) {
  SnapshotDelta d{};
  d.tick = r.u32();
  d.baseline_tick = r.u32();
  d.present_mask = r.u32();
  d.changed_mask = r.u32();
  d.record_count = static_cast<uint32_t>(__builtin_popcount(d.changed_mask));

  if (d.changed_mask & ~d.present_mask) return false;

  uint32_t idx = 0;
  for (uint32_t id = 1; id <= sim::kMaxPlayers; ++id) {
    if ((d.changed_mask & (1u << (id - 1))) == 0) continue;
    sim::PlayerState& p = d.records[idx++];
    p.id = id;
    p.x = r.f32();
    p.y = r.f32();
    p.vx = r.f32();
    p.vy = r.f32();
    p.radius = r.f32();
  }

  if (!r.ok()) return false;
  if (r.remaining() != 0) return false;

  out = d;
  return true;
}

bool applySnapshotDelta(const sim::WorldSnapshot& baseline, const SnapshotDelta& d,
                         sim::WorldSnapshot& out) {
  if (baseline.tick != d.baseline_tick) return false;

  sim::WorldSnapshot tmp{};
  tmp.tick = d.tick;
  uint32_t record_idx = 0;
  for (uint32_t id = 1; id <= sim::kMaxPlayers; ++id) {
    if ((d.present_mask & (1u << (id - 1))) == 0) continue;

    sim::PlayerState p{};
    if (d.changed_mask & (1u << (id - 1))) {
      p = d.records[record_idx++];
    } else {
      const sim::PlayerState* base = findById(baseline, id);
      if (base == nullptr) return false;
      p = *base;
    }
    tmp.players[tmp.count++] = p;
  }

  out = tmp;
  return true;
}

}  // namespace net
