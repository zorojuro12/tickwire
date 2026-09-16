# Phase 4 — Entity Interpolation + Snapshot Delta Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use the `executing-plans` skill to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Smooth remote players between the server's 20 Hz snapshots (entity
interpolation) and stop resending unchanged player records (snapshot delta),
so the demo looks right and the wire cost of a snapshot drops enough to prove
headroom past today's 32-player ceiling.

**Architecture:** Two independent halves that share one new data structure.
`net::SnapshotRing` holds the last 16 snapshots; the server keeps one so it can
encode a delta against whatever baseline each client last acknowledged, and the
client keeps one that serves double duty as the delta baseline store *and* the
interpolation buffer. Delta travels as a new additive message type
(`MsgType::kSnapshotDelta = 6`) carrying two 32-bit player-id bitmasks
(present / changed) plus a record for changed players only; the baseline a
client holds is acknowledged through the `ack_tick` header field, which is
already on every packet and is currently always zero in the client→server
direction. Interpolation is a separate client-side module (`client::Interpolator`)
that runs its own render timeline, deliberately *behind* the newest snapshot, and
linearly interpolates remote players between the two snapshots bracketing it.

**Tech Stack:** C++20, GCC 10, CMake 3.28.4 (in the pinned container),
GoogleTest, raylib (GUI config only). No new third-party dependencies.

**Spec:**
- [`docs/specs/2026-09-04-tickwire-design.md`](../specs/2026-09-04-tickwire-design.md) — build-order table, P4 = "Entity interpolation + snapshot delta", Risk: Medium
- [`docs/specs/2026-09-04-architecture-resolution.md`](../specs/2026-09-04-architecture-resolution.md) — §Q2 `kMaxPlayers = 32` is derived; delta is what buys headroom past it
- [`docs/wire-format.md`](../wire-format.md) — the frozen format this phase extends
- [`docs/project-history.md`](../project-history.md) — P2's 60/20 Hz cadence decision, P3's local-player-only prediction decision

---

## Global Constraints

Copied from `CLAUDE.md` and the specs. Every task's requirements implicitly
include this section.

- **Never invoke the host `g++`, `cmake`, or `ctest`.** The host has GCC 9.4 and
  CMake 3.16 (below the 3.21 floor; CMake 3.18 runs *zero tests and exits 0* — a
  silent green). All build/test commands go through `scripts/tw <command...>`,
  which runs them inside `tickwire-dev:gcc10-cmake3.28.4-x11` with the repo
  bind-mounted at `/work`.
- **`ctest` does not build.** `scripts/ci.sh` runs `cmake --build` before
  `ctest`, and so must every checkpoint in this plan — otherwise a new test file
  is never compiled and `ctest -R` either runs a stale binary or matches nothing
  and exits 0. Every command below is therefore
  `scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain -R <regex> --output-on-failure"`.
  (This is a correction to the P3 plan's template, which ran `ctest` alone.)
- **TSan runs additionally need `setarch -R`**, wrapping **`ctest`, not the
  build** (as `scripts/ci.sh:18` does):
  `scripts/tw bash -c "cmake --build build/tsan -j8 && setarch -R ctest --test-dir build/tsan --output-on-failure"`.
  `scripts/tw`'s capability flags and `setarch -R` are both required, neither
  alone is sufficient.
- **`-Wall -Wextra -Werror`** from the first commit. Non-negotiable.
- **No `std::format`** (GCC 10 lacks it), **no `std::bit_cast`** (GCC 11+ only —
  pun `float`↔`uint32_t` with `std::memcpy`).
- **`libsim` takes no changes in this phase.** No I/O, no wall-clock reads, no
  allocation, only trivially-copyable POD across its boundary. P4 is entirely
  `libnet` / `libserver` / `libclient` / `apps`.
- **No wall-clock reads in testable code.** `Client::tick(uint32_t now_ms)` and
  `Server::tick(uint32_t now_ms)` take the current monotonic millisecond as a
  parameter. Nothing this phase adds may call `server::monotonicMs()` outside
  `apps/`. The interpolation timeline is counted in **ticks**, not milliseconds,
  precisely so it needs no clock.
- **Protocol fields are explicitly little-endian**, byte-by-byte through
  `net::ByteWriter`/`net::ByteReader`. Never `memcpy` a struct onto the wire,
  never `reinterpret_cast` a buffer to a struct. `_be` suffixes are reserved for
  values the kernel requires in network order.
- **Decoders reject rather than normalize**, and never partially populate the
  caller's output struct — assign only once every check has passed.
- **File size:** 200–400 lines typical, 800 hard maximum.
- **Commits:** `type: description` (feat, fix, refactor, docs, test, chore, perf,
  ci). One commit per checkpoint, chained behind its test with `&&` — never `;`,
  never a separate line. `git add` names exact paths; **never** `git add -A` or
  `git add .`. The executing session appends its own `Claude-Session:` trailer
  per its system instructions; the commands below omit it deliberately, since
  the session id is not knowable when a plan is written.
- **Test scoping:** inside a checkpoint use `-R <regex>`; the full suite only at
  a task boundary. Never run the full suite inside a checkpoint.
- **Coverage:** 80% project-wide; `libsim` is held to 100% (unchanged this phase).
- **Any phase touching the network surface requires the `security-review` skill
  / `security-reviewer` agent — mandatory, not optional.** This phase adds a
  message type and a new attacker-influenced field path; Task 9 is that review.

---

## Decisions this plan makes

These are the open questions P4 raises. Each is settled here, with the
reasoning, so the executing session does not re-litigate them.

**1. A new message type, not a format version bump.** `kSnapshotDelta = 6`,
`kMaxMsgType` 5 → 6. `docs/wire-format.md` already designates this the
additive-extension path: *"Values `6..255` are unused; P6's hit-feedback message
can be added additively without touching the header or bumping the version
again."* `kProtocolVersion` stays **2**. Full `kSnapshot` packets remain, and
remain the keyframe every session starts on — delta is an optimization layered
over the existing message, not a replacement for it. The wire-format doc still
needs a new payload section and a Version-history entry (Task 10); "no version
bump" is not "no documentation change".

**2. The baseline is acknowledged through `ack_tick`, which costs zero new
bytes.** `PacketHeader::ack_tick` is populated today *only* by the server on
`Snapshot` packets; on client→server `Input` packets it has always been the
default `0` (verified: `Client::sendInput` sets `type`, `tick` and
`send_time_ms`, and nothing else; `Server::handleInput` does not even receive
the header). The client now sets `ack_tick = latest_snapshot_tick_` on every
input, and the server records it per session. This is the same story P1's
reservation told at P3 — a field that was already there turning out to be
exactly the channel a later phase needed.

**3. Unchanged players cost zero bytes; `radius` stays in the records that are
sent.** The architecture-resolution doc names `radius` as *"the first field to
drop from a delta"* (it is `sim::kPlayerRadius` for every player, always). This
plan deliberately does something strictly better and drops the **entire 24-byte
record** for every unchanged player, while keeping `radius` inside the 20-byte
records that *are* sent. Rationale: dropping `radius` from a sent record saves 4
bytes but forces a baseline lookup (or a hardcoded `sim::kPlayerRadius` in the
wire decoder) for players that have no baseline entry, i.e. a player who joined
since the baseline — a variable-size record or a special case in the decoder,
for 4 bytes. Keeping it buys the property that **a delta applied to its baseline
reconstructs a `WorldSnapshot` field-for-field identical to the full snapshot of
the same tick**, which is Task 3's strongest test and worth far more than the
bytes. This is a deliberate, argued deviation from the resolution doc, recorded
in `docs/project-history.md` at Task 10.

**4. The interpolation timeline is its own controller, not `clockLead()`.**
The obvious shortcut — render at `tick_ - clockLead() - delay` — is rejected:
`clockLead()` is documented in `client.h` as a stale, round-trip-old value, and
P3's own convergence work found it reports a materially different number from
the true gap. Feeding a noisy estimate into the render path would reintroduce
the jitter this phase exists to remove. Instead `Interpolator` holds a
`render_tick_` advanced one tick per client tick and corrected toward
`newest_snapshot_tick - kInterpDelayTicks` on each snapshot. Unlike `ClockSync`
this needs **no EMA and no cooldown**: `ClockSync` closes a loop over
substantial dead time (it observes the delayed effect of its own past
corrections), whereas `Interpolator` observes the snapshot tick *directly* and
its correction has no influence on that signal at all. Snap-or-nudge is
sufficient, and the simpler controller is the correct one here.

**5. No extrapolation on starvation.** When the render tick runs past the newest
snapshot (a lost or late packet), remote players **freeze at the newest known
position**. Extrapolation guesses a velocity-projected position it must then
visibly retract; it is the classic way to turn a 50 ms gap into a 200 ms
rubber-band. Freezing for the ~50 ms until the next snapshot is the better
artifact, and it is what `kInterpDelayTicks = 6` (two full snapshot intervals)
exists to make rare.

**6. `kMaxPlayers` stays at 32.** Delta's justification is headroom, and this
phase *measures* the headroom rather than spending it. Raising the ceiling
touches `SessionTable`, spawn layout, and the load client's shape — that is P5's
concern, where it can be load-tested rather than asserted. The number this phase
produces for the headline table:

| Encoding | Payload | On the wire (+24 B header) | Max players under the 1200 B MTU |
|---|---|---|---|
| Full snapshot | `8 + 24N` | `32 + 24N` | **48** |
| Delta, worst case (every player changed) | `16 + 20N` | `40 + 20N` | **58** |
| Delta, typical (few movers) | `16 + 20C` | `40 + 20C` | far higher |

At today's `N = 32`: full is 800 B on the wire, an all-changed delta is 680 B,
and a delta with two movers is 80 B — a 90% reduction in the common case.

---

## File Structure

**New files**

| Path | Responsibility |
|---|---|
| `src/net/snapshot_ring.h` / `.cpp` | `net::SnapshotRing` — the last 16 snapshots, found by tick. Serves the delta codec's baseline lookup on both ends, and the client's interpolation buffer. Lives in `libnet` because it exists for the codec and both `libserver` and `libclient` already link it. |
| `src/net/snapshot_delta.h` / `.cpp` | The `kSnapshotDelta` payload codec: `encodeSnapshotDelta`, `decodeSnapshotDelta`, `applySnapshotDelta`. Kept out of `protocol.cpp`, which is already the frozen-format file. |
| `src/client/interpolation.h` / `.cpp` | `client::Interpolator` — the render timeline and the bracketing lerp. Pure: takes a `const net::SnapshotRing&`, touches no transport. |
| `tests/net/snapshot_ring_test.cpp` | Task 1 |
| `tests/net/snapshot_delta_test.cpp` | Tasks 2–3 |
| `tests/client/interpolation_test.cpp` | Task 7 |

**Modified files**

| Path | Change |
|---|---|
| `src/net/protocol.h` | `kSnapshotDelta = 6`, `kMaxMsgType` 5 → 6, delta size constants |
| `src/server/session.h` / `.cpp` | Per-session acknowledged snapshot tick |
| `src/server/server.h` | Snapshot history, per-session delta/keyframe broadcast, byte counters, `handleInput` takes the header |
| `src/client/client.h` | Ack out, delta in, ring storage, interpolation wiring and toggle |
| `apps/tw_client.cpp` | Interpolated remote rendering, `I` toggle, HUD |
| `apps/tw_loadclient.cpp` | Delta byte-savings stats |
| `CMakeLists.txt` | Three new sources into `libnet`/`libclient`, three new test targets |
| `docs/wire-format.md`, `docs/project-history.md`, `CLAUDE.md`, `README.md` | Task 10 |

`src/client/client.h` is 344 lines today and CLAUDE.md's typical band is
200–400. The heavy logic deliberately lands in the two new modules so
`client.h` grows by roughly 60 lines of wiring, not 300 of algorithm. If it
crosses 400 during Task 8, that is acceptable (the hard limit is 800) and not a
reason to restructure mid-phase.

---

## Task 1: `net::SnapshotRing`

The store both halves of the phase read from. Nothing else in the phase can be
written until snapshots can be kept and found by tick.

**Files:**
- Create: `src/net/snapshot_ring.h`, `src/net/snapshot_ring.cpp`
- Create: `tests/net/snapshot_ring_test.cpp`
- Modify: `CMakeLists.txt` — add `src/net/snapshot_ring.cpp` to the `libnet`
  sources (line 28-ish, the `add_library(libnet STATIC ...)` call) and
  `tw_add_test(snapshot_ring_test tests/net/snapshot_ring_test.cpp)` beside the
  other `tests/net/` targets.

**Interfaces:**
- Consumes: `sim::WorldSnapshot` (`src/sim/sim.h`), unchanged.
- Produces:

```cpp
namespace net {

inline constexpr size_t kSnapshotRingSlots = 16;  // 16 snapshots at 20 Hz = 800 ms

// The most recent kSnapshotRingSlots snapshots, found by tick. Fixed storage,
// no allocation; a 16-entry linear scan, matching SessionTable's own approach.
// Callers must not store the same tick twice -- both call sites are already
// guarded by a strictly-newer check.
class SnapshotRing {
 public:
  void store(const sim::WorldSnapshot& s) noexcept;
  const sim::WorldSnapshot* find(uint32_t tick) const noexcept;          // nullptr when absent
  const sim::WorldSnapshot* newest() const noexcept;                     // nullptr when empty
  const sim::WorldSnapshot* newestAtOrBefore(uint32_t tick) const noexcept;
  const sim::WorldSnapshot* oldestAfter(uint32_t tick) const noexcept;
  uint32_t count() const noexcept;  // live entries, saturating at kSnapshotRingSlots

 private:
  std::array<sim::WorldSnapshot, kSnapshotRingSlots> slots_{};
  std::array<bool, kSnapshotRingSlots> filled_{};
  size_t next_ = 0;
};

}  // namespace net
```

**Checkpoint 1: a stored snapshot is found by its tick; an absent one is not**

- [ ] **Step 1: Write the failing test, then run it**

Spec, in `tests/net/snapshot_ring_test.cpp` as
`TEST(SnapshotRingTest, StoresAndFindsByTick)`:
`net::SnapshotRing ring;` — assert `ring.count() == 0`, `ring.find(100) == nullptr`
and `ring.newest() == nullptr`. Build `sim::WorldSnapshot s{}` with `s.tick = 100`,
`s.count = 1`, `s.players[0] = sim::PlayerState{1, 5.0f, -5.0f, 1.0f, 2.0f, sim::kPlayerRadius}`,
and `ring.store(s)`. Then: `ring.count() == 1`; `ring.find(100)` is non-null and
reports `tick == 100`, `count == 1`, `players[0].x == 5.0f`, `players[0].vy == 2.0f`;
`ring.find(99) == nullptr`; `ring.newest()` is non-null with `tick == 100`.
Store a second snapshot with `s.tick = 103`, `count = 0`: `ring.find(100)` is still
non-null, `ring.find(103)` is non-null, `ring.newest()->tick == 103`, `ring.count() == 2`.

Run: `scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain -R snapshot_ring_test --output-on-failure"`
Expected: FAIL — compile error, `net/snapshot_ring.h: No such file or directory`.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: `store` writes into `slots_[next_]`, sets `filled_[next_] = true`, and
advances `next_ = (next_ + 1) % kSnapshotRingSlots`. `find` linearly scans filled
slots for `slots_[i].tick == tick` and returns its address, else `nullptr`.
`newest` returns the filled slot with the greatest `tick`, else `nullptr`.
`count` counts filled slots. Add `#include <array>`, `<cstdint>`, `"sim/sim.h"`.
Leave `newestAtOrBefore` / `oldestAfter` for Checkpoint 3.

```bash
scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain -R snapshot_ring_test --output-on-failure" && \
  git add src/net/snapshot_ring.h src/net/snapshot_ring.cpp tests/net/snapshot_ring_test.cpp CMakeLists.txt && \
  git commit -m "feat: keep the last 16 snapshots addressable by tick"
```

Expected: PASS, then one commit.

**Checkpoint 2: the oldest entry is evicted once the ring wraps**

- [ ] **Step 1: Write the failing test, then run it**

Spec, as `TEST(SnapshotRingTest, EvictsOldestOnceFull)`: store 17 snapshots with
`tick = 1..17` (`count = 0` on each). Assert `ring.count() == net::kSnapshotRingSlots`
(16, not 17); `ring.find(1) == nullptr` (evicted by the 17th store);
`ring.find(2)` is non-null; `ring.find(17)` is non-null; `ring.newest()->tick == 17`.

Run: `scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain -R snapshot_ring_test --output-on-failure"`
Expected: FAIL on `ring.count()` — `count()` returns 17 is impossible (the array
holds 16), so the concrete first failure is `ring.find(1)` returning non-null if
Checkpoint 1's `store` were written without the modulo, **or** a PASS if it was
written with it. If this checkpoint passes on first write, do **not** alter the
test: record it as a regression pin in the commit message and move to
Checkpoint 3. The modulo in Checkpoint 1's contract makes that the likely outcome.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: no change if Step 1 passed — the wrap is already in `store`. If it
failed, the fix is the `% kSnapshotRingSlots` in `store`'s `next_` advance and
nothing else.

```bash
scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain -R snapshot_ring_test --output-on-failure" && \
  git add src/net/snapshot_ring.cpp tests/net/snapshot_ring_test.cpp && \
  git commit -m "test: pin SnapshotRing eviction once the ring wraps"
```

Expected: PASS, then one commit.

**Checkpoint 3: the bracketing lookups find the snapshots either side of a tick**

- [ ] **Step 1: Write the failing test, then run it**

Spec, as `TEST(SnapshotRingTest, BracketsATickBetweenTwoSnapshots)`: store
snapshots at ticks `100`, `103`, `106` (`count = 0`). Then:
- `newestAtOrBefore(104)->tick == 103` and `oldestAfter(104)->tick == 106`
- `newestAtOrBefore(103)->tick == 103` (at-or-before is inclusive) and
  `oldestAfter(103)->tick == 106` (after is exclusive)
- `newestAtOrBefore(99) == nullptr` (nothing that old) and `oldestAfter(99)->tick == 100`
- `newestAtOrBefore(200)->tick == 106` and `oldestAfter(200) == nullptr`
  (the starvation case Decision 5 freezes on)

Run: `scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain -R snapshot_ring_test --output-on-failure"`
Expected: FAIL — compile error, `'class net::SnapshotRing' has no member named 'newestAtOrBefore'`.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: `newestAtOrBefore(t)` scans filled slots and returns the one with the
greatest `tick <= t`, else `nullptr`. `oldestAfter(t)` returns the one with the
smallest `tick > t`, else `nullptr`. Plain `<=` / `>` on `uint32_t`; this ring
never spans a tick-counter wrap (16 snapshots is 48 ticks, against a counter
that wraps after ~2.3 years at 60 Hz — the same bound `InputBuffer` already
accepts, recorded in P3's security review).

```bash
scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain -R snapshot_ring_test --output-on-failure" && \
  git add src/net/snapshot_ring.h src/net/snapshot_ring.cpp tests/net/snapshot_ring_test.cpp && \
  git commit -m "feat: find the snapshots bracketing a render tick"
```

Expected: PASS, then one commit.

**Task boundary:** full suite, plain configuration.

```bash
scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain --output-on-failure"
```

---

## Task 2: the snapshot delta codec

The payload format and its three operations. Encoding and decoding stay
independent of the baseline (a decoder must be able to reject a malformed
packet without holding any state); reconstruction is the separate
`applySnapshotDelta` step.

**Files:**
- Modify: `src/net/protocol.h` — `MsgType::kSnapshotDelta = 6`, `kMaxMsgType = 6`, size constants
- Create: `src/net/snapshot_delta.h`, `src/net/snapshot_delta.cpp`
- Create: `tests/net/snapshot_delta_test.cpp`
- Modify: `CMakeLists.txt` — `src/net/snapshot_delta.cpp` into `libnet`,
  `tw_add_test(snapshot_delta_test tests/net/snapshot_delta_test.cpp)`

**Interfaces:**
- Consumes: `net::ByteWriter` / `net::ByteReader` (`src/net/bytes.h`),
  `sim::WorldSnapshot`, `sim::kMaxPlayers`.
- Produces:

```cpp
namespace net {

inline constexpr size_t kSnapshotDeltaFixedBytes = 16;   // tick + baseline_tick + 2 masks
inline constexpr size_t kDeltaRecordBytes = 20;          // x, y, vx, vy, radius (id is in the mask)

// One decoded kSnapshotDelta payload. Records are ascending by player id and
// correspond, in order, to the set bits of changed_mask.
struct SnapshotDelta {
  uint32_t tick;
  uint32_t baseline_tick;
  uint32_t present_mask;   // bit (id - 1) set: player id is in this snapshot
  uint32_t changed_mask;   // bit (id - 1) set: a record for id follows; else copy the baseline
  uint32_t record_count;   // == popcount(changed_mask)
  std::array<sim::PlayerState, sim::kMaxPlayers> records;  // .id filled in from the mask
};

// Encodes `current` as a delta against `baseline`. False if the writer runs out
// of room or either snapshot holds an out-of-range id or count.
bool encodeSnapshotDelta(const sim::WorldSnapshot& baseline,
                         const sim::WorldSnapshot& current, ByteWriter& w);
bool decodeSnapshotDelta(ByteReader& r, SnapshotDelta& out);

// Reconstructs the full snapshot. False when the delta names a player that is
// present-but-unchanged and absent from the baseline (unreconstructable), or
// when baseline.tick != d.baseline_tick.
bool applySnapshotDelta(const sim::WorldSnapshot& baseline, const SnapshotDelta& d,
                        sim::WorldSnapshot& out);

}  // namespace net
```

Payload layout (little-endian throughout, byte-by-byte):

| Offset | Size | Field |
|---:|---:|---|
| 0 | 4 | `tick` |
| 4 | 4 | `baseline_tick` |
| 8 | 4 | `present_mask` |
| 12 | 4 | `changed_mask` |
| 16 | 20 × `popcount(changed_mask)` | records, ascending by id: `x`, `y`, `vx`, `vy`, `radius` (IEEE-754 binary32, in that order) |

A player is *unchanged* when all five of `x`, `y`, `vx`, `vy`, `radius` compare
equal (`==`) to the baseline's record for the same id. `==` rather than a bitwise
comparison is deliberate: the only values where they differ are `-0.0f` vs
`+0.0f`, and reconstructing one where the other stood is arithmetically
indistinguishable forever after. `NaN` cannot reach here — every decoder already
rejects non-finite floats, and `World` never produces one.

**Checkpoint 1: the header accepts message type 6**

- [ ] **Step 1: Write the failing test, then run it**

Spec, in `tests/net/protocol_test.cpp` as
`TEST(ProtocolTest, HeaderAcceptsSnapshotDeltaType)`: build a `net::PacketHeader h`
with `h.type = net::MsgType::kSnapshotDelta`, `h.payload_len = 0`, encode it with
`net::encodeHeader` into a 24-byte buffer, decode it back with `net::decodeHeader`,
and assert the call returns `true` and `out.type == net::MsgType::kSnapshotDelta`.
Also assert `static_cast<uint8_t>(net::MsgType::kSnapshotDelta) == 6` and
`net::kMaxMsgType == 6`, and that a header whose raw type byte is `7` is still
**rejected** (hand-write the 24 bytes, or encode then patch offset 5 to `7`).

Run: `scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain -R protocol_test --output-on-failure"`
Expected: FAIL — compile error, `'kSnapshotDelta' is not a member of 'net::MsgType'`.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: in `src/net/protocol.h`, add `kSnapshotDelta = 6` to `MsgType` and
change `kMaxMsgType` from `5` to `6`. `kProtocolVersion` stays `2` (Decision 1).
No other file changes — `decodeHeader` already validates against `kMaxMsgType`.
Check whether an existing test in `protocol_test.cpp` or `robustness_test.cpp`
pins "type 6 is rejected"; if one does, update it to pin type 7 instead, and
include that file in the commit.

```bash
scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain -R 'protocol_test|robustness_test|framing_test' --output-on-failure" && \
  git add src/net/protocol.h tests/net/protocol_test.cpp && \
  git commit -m "feat: admit a snapshot-delta message type to the header"
```

Expected: PASS, then one commit.

**Checkpoint 2: an unchanged player costs zero bytes and round-trips**

- [ ] **Step 1: Write the failing test, then run it**

Spec, in `tests/net/snapshot_delta_test.cpp` as
`TEST(SnapshotDeltaTest, UnchangedPlayerIsOmittedAndRestored)`. Build a baseline:
`tick = 100`, `count = 2`, players `{1, 1.0f, 2.0f, 0.0f, 0.0f, kPlayerRadius}` and
`{2, -3.0f, 4.0f, 0.0f, 0.0f, kPlayerRadius}`. Build `current`: `tick = 103`,
same two players, **player 1 unchanged**, player 2 moved to
`{2, -3.0f, 5.0f, 0.0f, 1.0f, kPlayerRadius}`. Encode into a
`std::array<std::byte, net::kMaxPacket>` via `net::ByteWriter`:
- `encodeSnapshotDelta(baseline, current, w)` returns `true`
- `w.size() == net::kSnapshotDeltaFixedBytes + net::kDeltaRecordBytes` (16 + 20 = **36**),
  proving player 1 contributed nothing

Decode with `net::ByteReader` into a `net::SnapshotDelta d`: returns `true`,
`d.tick == 103`, `d.baseline_tick == 100`, `d.present_mask == 0b11`,
`d.changed_mask == 0b10`, `d.record_count == 1`.

Then `sim::WorldSnapshot out{}; applySnapshotDelta(baseline, d, out)` returns
`true`, and `out` equals `current` field for field: `out.tick == 103`,
`out.count == 2`, and for each of ids 1 and 2 the `x`, `y`, `vx`, `vy`, `radius`
match `current`'s exactly (`==`, not `EXPECT_NEAR` — reconstruction is exact).
Players in `out` are ordered ascending by id.

Run: `scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain -R snapshot_delta_test --output-on-failure"`
Expected: FAIL — compile error, `net/snapshot_delta.h: No such file or directory`.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract:
- `encodeSnapshotDelta`: return `false` if either snapshot's `count > sim::kMaxPlayers`
  or holds an id outside `[1, sim::kMaxPlayers]`. Build `present_mask` from
  `current`'s ids and `changed_mask` from the five-field comparison against the
  baseline's same-id record (a player absent from the baseline is **changed**).
  Write `tick`, `baseline_tick` (= `baseline.tick`), `present_mask`,
  `changed_mask`, then one 20-byte record per set bit of `changed_mask` in
  ascending id order. Return `false` if any write fails (the `ByteWriter` is
  bounds-checked; propagate, do not assume room).
- `decodeSnapshotDelta`: read the four fixed fields, set
  `record_count = popcount(changed_mask)` via `__builtin_popcount`, then read
  exactly that many records, filling each `records[i].id` from the *i*-th set
  bit of `changed_mask` (ascending). Propagate a `false` from the bounds-checked
  reader. Assign to `out` only after every read has succeeded.
  **Deliberately not yet implemented here:** the mask-subset guard, the
  trailing-bytes check, and the non-finite rejection. Task 3 drives each of
  those from its own failing test — do not write them early, or Task 3's
  checkpoints have nothing to turn from red to green.
- `applySnapshotDelta`: return `false` if `baseline.tick != d.baseline_tick`.
  Walk ids `1..sim::kMaxPlayers` ascending; for each bit set in `present_mask`,
  take the record from `d` if its bit is set in `changed_mask`, else find that id
  in `baseline` and copy it — returning `false` if it is not there. Append to
  `out.players` in ascending id order, set `out.count` and `out.tick = d.tick`.
  Populate `out` only once the walk has succeeded.

Add both sources to `libnet` in `CMakeLists.txt` and register the test target.

```bash
scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain -R snapshot_delta_test --output-on-failure" && \
  git add src/net/snapshot_delta.h src/net/snapshot_delta.cpp tests/net/snapshot_delta_test.cpp CMakeLists.txt && \
  git commit -m "feat: encode a snapshot as a delta against a baseline"
```

Expected: PASS, then one commit.

**Checkpoint 3: a player who left the game disappears from the reconstruction**

- [ ] **Step 1: Write the failing test, then run it**

Spec, as `TEST(SnapshotDeltaTest, DepartedPlayerIsDroppedFromPresentMask)`:
baseline `tick = 100`, `count = 3`, ids 1, 2, 3 at distinct positions. Current
`tick = 103`, `count = 2`, **ids 1 and 3 only** (player 2 left), both unchanged
from the baseline. Encode: `w.size() == 16` (no records at all — two unchanged
players and one departure cost nothing beyond the masks). Decode:
`d.present_mask == 0b101`, `d.changed_mask == 0`, `d.record_count == 0`.
Apply: returns `true`, `out.count == 2`, `out.players[0].id == 1`,
`out.players[1].id == 3`, and no slot in `[0, out.count)` has `id == 2`.

Run: `scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain -R snapshot_delta_test --output-on-failure"`
Expected: PASS is possible here — Checkpoint 2's `present_mask` walk already
handles departures. If it passes on first write, do not alter the test; commit it
as the regression pin it is, with the `test:` commit message below. If it fails,
the cause is `applySnapshotDelta` iterating the baseline's players rather than
the `present_mask`, and the fix is to drive the walk from the mask.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: `applySnapshotDelta` is driven by `present_mask`, never by the
baseline's own `count` — a baseline player whose bit is clear is simply not
copied.

```bash
scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain -R snapshot_delta_test --output-on-failure" && \
  git add src/net/snapshot_delta.cpp tests/net/snapshot_delta_test.cpp && \
  git commit -m "test: pin that a departed player leaves the delta's present mask"
```

Expected: PASS, then one commit.

**Checkpoint 4: a player who joined since the baseline arrives as a full record**

- [ ] **Step 1: Write the failing test, then run it**

Spec, as `TEST(SnapshotDeltaTest, PlayerAbsentFromBaselineIsSentInFull)`:
baseline `tick = 100`, `count = 1`, player `{1, 0.0f, 0.0f, 0.0f, 0.0f, kPlayerRadius}`.
Current `tick = 103`, `count = 2`: player 1 unchanged, plus a new player
`{5, -35.0f, 25.0f, 0.0f, 0.0f, kPlayerRadius}` — note the **non-contiguous id**,
which is what a reused session slot produces. Encode: `w.size() == 36`
(one record, for player 5 only). Decode: `d.present_mask == 0b10001` (bits 0 and 4),
`d.changed_mask == 0b10000` (bit 4 only), `d.record_count == 1`, and
`d.records[0].id == 5` — proving the id is recovered from the bit position, not
from the wire. Apply: `out.count == 2`, player 5 present at exactly
`x == -35.0f`, `y == 25.0f`.

Run: `scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain -R snapshot_delta_test --output-on-failure"`
Expected: FAIL if Checkpoint 2's encoder treated "absent from baseline" as
unchanged (the record would be omitted, `w.size() == 16`, and `applySnapshotDelta`
would then return `false`). Checkpoint 2's contract states the rule explicitly, so
a PASS here is acceptable — commit as a pin.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: in `encodeSnapshotDelta`, a current player with no same-id record in
the baseline sets its `changed_mask` bit unconditionally.

```bash
scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain -R snapshot_delta_test --output-on-failure" && \
  git add src/net/snapshot_delta.cpp tests/net/snapshot_delta_test.cpp && \
  git commit -m "test: pin that a newly joined player is delta-encoded in full"
```

Expected: PASS, then one commit.

**Checkpoint 5: a delta applied to its baseline reconstructs the full snapshot exactly**

- [ ] **Step 1: Write the failing test, then run it**

Spec, as `TEST(SnapshotDeltaTest, DeltaThenApplyEqualsTheFullSnapshot)` — the
property that justifies keeping `radius` on the wire (Decision 3). Build a
baseline with all 32 players at `x = i * 1.5f`, `y = -i * 0.25f`, `vx = 0`,
`vy = 0`, `radius = kPlayerRadius`, ids `1..32`, `tick = 100`. Build `current`
at `tick = 103` from a copy, then mutate a spread of them: ids 1, 7, 8, 31 and 32
get `x += 0.125f` and `vx = sim::kMoveSpeed`. Encode the delta, decode it, apply
it, and assert the result is field-for-field identical to `current`: same `tick`,
same `count`, and for every index `i` in `[0, count)` the same `id`, `x`, `y`,
`vx`, `vy`, `radius` under `==`. Separately, encode `current` with the existing
`net::encodeSnapshot` into another buffer and assert the delta buffer is strictly
smaller (`16 + 5*20 == 116` against `8 + 32*24 == 776`).

Run: `scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain -R snapshot_delta_test --output-on-failure"`
Expected: PASS is the likely outcome, since Checkpoints 2–4 built every path this
exercises. It is kept as its own checkpoint because it is the phase's central
correctness claim and deserves its own commit; if it fails, the failure localizes
the bug to whichever field or id ordering diverges.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: no new code expected. If the ordering assertion fails, the cause is
`applySnapshotDelta` appending in mask order while `encodeSnapshot` preserves
`World`'s slot order — fix by making the reconstruction ascending-by-id (as
Checkpoint 2's contract specifies) and, if needed, sorting the expectation rather
than the implementation.

```bash
scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain -R snapshot_delta_test --output-on-failure" && \
  git add src/net/snapshot_delta.cpp tests/net/snapshot_delta_test.cpp && \
  git commit -m "test: pin delta-plus-baseline against the full snapshot encoding"
```

Expected: PASS, then one commit.

**Task boundary:** full suite, plain configuration.

```bash
scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain --output-on-failure"
```

---

## Task 3: delta decoder strictness and the golden byte vector

The project's rule is that decoders reject rather than normalize, and that
golden byte vectors — not prose — define the format. Task 2 built the happy
paths; this task makes the decoder hostile and pins the layout.

**Files:**
- Modify: `src/net/snapshot_delta.cpp`
- Modify: `tests/net/snapshot_delta_test.cpp`
- Modify: `tests/net/robustness_test.cpp` — extend the existing fuzz/truncation
  sweep to cover the new payload

**Interfaces:** no signature changes. `decodeSnapshotDelta` gains guards only.

**Checkpoint 1: a changed bit outside the present mask is rejected**

- [ ] **Step 1: Write the failing test, then run it**

Spec, as `TEST(SnapshotDeltaTest, RejectsChangedMaskNotSubsetOfPresentMask)`:
hand-build a payload with `net::ByteWriter` — `tick = 103`, `baseline_tick = 100`,
`present_mask = 0b0001`, `changed_mask = 0b0011` (bit 1 set but not present),
followed by two 20-byte records of finite values. Assert
`decodeSnapshotDelta` returns `false`. Also assert the output struct is
untouched: pass a `net::SnapshotDelta d{}` pre-set to `d.tick = 0xDEADBEEF` and
confirm it still reads `0xDEADBEEF` after the rejected call.

Run: `scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain -R snapshot_delta_test --output-on-failure"`
Expected: FAIL — the decoder currently accepts it, returns `true`, and
overwrites `d.tick` with `103`.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: after reading the two masks and before reading any record, return
`false` when `(changed_mask & ~present_mask) != 0`. A record for a player the
sender simultaneously claims is absent is incoherent, and accepting it would let
a sender drive `record_count` above the number of reconstructable players.

```bash
scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain -R snapshot_delta_test --output-on-failure" && \
  git add src/net/snapshot_delta.cpp tests/net/snapshot_delta_test.cpp && \
  git commit -m "fix: reject a delta whose changed mask escapes its present mask"
```

Expected: PASS, then one commit.

**Checkpoint 2: a payload that disagrees with its own record count is rejected**

- [ ] **Step 1: Write the failing test, then run it**

Spec, as `TEST(SnapshotDeltaTest, RejectsPayloadLengthDisagreeingWithChangedMask)`,
two cases against `present_mask = changed_mask = 0b11` (two records promised, 56
bytes total expected):
- **too few**: write the 16 fixed bytes plus *one* 20-byte record (36 bytes) and
  decode over exactly those 36 bytes → `false`
- **too many**: write the 16 fixed bytes plus *three* records (76 bytes) and
  decode over all 76 → `false` (trailing bytes are a malformed packet, not
  padding to ignore)

Run: `scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain -R snapshot_delta_test --output-on-failure"`
Expected: FAIL on the **too many** case only. The too-few case already fails
correctly because `ByteReader` is bounds-checked and Task 2's contract
propagates its `false`; the trailing-bytes check does not exist yet, so the
too-many case returns `true`.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: after the final record is read, return `false` unless the reader is
exactly exhausted. Use the same "declared framing must match the bytes present"
rule `decodeSnapshot` already applies to `count × 24`.

```bash
scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain -R snapshot_delta_test --output-on-failure" && \
  git add src/net/snapshot_delta.cpp tests/net/snapshot_delta_test.cpp && \
  git commit -m "fix: reject a snapshot delta with bytes beyond its last record"
```

Expected: PASS, then one commit.

**Checkpoint 3: a non-finite float in any record is rejected**

- [ ] **Step 1: Write the failing test, then run it**

Spec, as `TEST(SnapshotDeltaTest, RejectsNonFiniteRecordFields)`: for each of the
five fields in turn (`x`, `y`, `vx`, `vy`, `radius`), hand-build a single-record
payload (`present_mask = changed_mask = 0b1`) whose record carries
`std::nanf("")` in that field and finite values elsewhere, and assert
`decodeSnapshotDelta` returns `false`. Repeat once with
`std::numeric_limits<float>::infinity()` and once with its negation. Use
`std::nanf("")` and `std::numeric_limits<float>::infinity()` rather than the
`NAN`/`INFINITY` macros, matching the existing suite.

Run: `scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain -R snapshot_delta_test --output-on-failure"`
Expected: FAIL — every case returns `true`; the finiteness guard does not exist
on this path yet.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: reject the payload if `std::isfinite` fails on any of the five fields
of any record. This is the same guard `decodeSnapshot` already applies to a full
`PlayerState`, and it exists for the same reason recorded in P2's security
review: a `NaN` that reaches `libsim` is sticky under IEEE-754 and corrupts the
entity's state every tick thereafter. The delta path reaches exactly the same
`World::setPlayerState` through the client's reconciliation, so it needs exactly
the same guard.

```bash
scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain -R snapshot_delta_test --output-on-failure" && \
  git add src/net/snapshot_delta.cpp tests/net/snapshot_delta_test.cpp && \
  git commit -m "fix: reject non-finite floats in a snapshot delta record"
```

Expected: PASS, then one commit.

**Checkpoint 4: the golden byte vector pins the layout**

- [ ] **Step 1: Write the failing test, then run it**

Spec, as `TEST(SnapshotDeltaTest, GoldenByteVector)`. Baseline `tick = 0x64` (100),
`count = 2`, players `{1, 0.0f, 0.0f, 0.0f, 0.0f, 0.5f}` and
`{2, 1.0f, 0.0f, 0.0f, 0.0f, 0.5f}`. Current `tick = 0x67` (103), player 1
unchanged, player 2 moved to `{2, 2.0f, 0.0f, 0.0f, 0.0f, 0.5f}`. Encode and
assert the produced bytes are exactly, in order:

```
67 00 00 00   tick          = 103
64 00 00 00   baseline_tick = 100
03 00 00 00   present_mask  = 0b11
02 00 00 00   changed_mask  = 0b10
00 00 00 40   x      = 2.0f
00 00 00 00   y      = 0.0f
00 00 00 00   vx     = 0.0f
00 00 00 00   vy     = 0.0f
00 00 00 3F   radius = 0.5f
```

36 bytes total. Compare with a byte-for-byte loop over a
`std::array<uint8_t, 36>` literal so a mismatch names the offset. Assert also
that decoding these exact bytes yields `tick == 103`, `changed_mask == 0b10`,
`records[0].id == 2`, `records[0].x == 2.0f`.

Run: `scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain -R snapshot_delta_test --output-on-failure"`
Expected: PASS is the likely outcome — Task 2 built the encoder to this layout.
This checkpoint exists because `docs/wire-format.md` states that the golden
vectors, not the prose, *define* the format, so the format is not actually
frozen until one exists. If it fails, the byte dump names the offset that
diverged; fix the encoder to match this table, never the table to match the
encoder.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: no new code expected. If a float's bytes disagree, confirm the
encoder writes floats through the same `ByteWriter` float path
`encodeSnapshot` uses (`std::memcpy` into a `uint32_t`, then four LE bytes) and
not through any host-layout shortcut.

```bash
scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain -R snapshot_delta_test --output-on-failure" && \
  git add tests/net/snapshot_delta_test.cpp && \
  git commit -m "test: pin the snapshot delta layout with a golden byte vector"
```

Expected: PASS, then one commit.

**Checkpoint 5: the malformed-input sweep covers the new payload**

- [ ] **Step 1: Write the failing test, then run it**

Spec, in `tests/net/robustness_test.cpp`, matching whatever sweep style that
file already uses for `decodeSnapshot`:
- a **truncation sweep**: take the 36-byte golden payload and decode every
  prefix of length `0..35`; every one returns `false` and none reads out of
  bounds (ASan is the check that matters here, so this test must also be run in
  the ASan configuration at the task boundary)
- a **random-byte sweep**: 20,000 trials of random bytes of random length in
  `[0, 128]` through `decodeSnapshotDelta`; no crash, no hang, and whenever it
  returns `true` the result satisfies `(changed_mask & ~present_mask) == 0` and
  `record_count == __builtin_popcount(changed_mask)`. Seed the RNG with a fixed
  constant so a failure reproduces.

Run: `scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain -R robustness_test --output-on-failure"`
Expected: PASS is expected for the truncation sweep given Checkpoint 2, and for
the invariants given Checkpoints 1–3. The sweep's job is to find the case the
four hand-written checkpoints missed; a genuine failure here is a real bug in
`decodeSnapshotDelta`, not a bad test.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: no new code expected. Any failure is fixed in
`src/net/snapshot_delta.cpp`, never by loosening the sweep's invariants.

```bash
scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain -R robustness_test --output-on-failure" && \
  git add tests/net/robustness_test.cpp src/net/snapshot_delta.cpp && \
  git commit -m "test: sweep malformed input through the snapshot delta decoder"
```

Expected: PASS, then one commit.

**Task boundary:** full suite in **plain and ASan** — this task's truncation
sweep is only meaningful under a sanitizer.

```bash
scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain --output-on-failure" && \
scripts/tw bash -c "cmake --build build/asan -j8 && ctest --test-dir build/asan --output-on-failure"
```

---

## Task 4: the server records which snapshot each session holds

The delta baseline is per session, so the server needs somewhere to keep it and
a way to learn it. Both halves land here; the broadcast that uses them is Task 5.

**Files:**
- Modify: `src/server/session.h`, `src/server/session.cpp`
- Modify: `src/server/server.h` — `handleInput` gains the header parameter
- Modify: `tests/server/session_test.cpp`, `tests/server/server_test.cpp`

**Interfaces:**
- Produces, on `server::SessionTable`:

```cpp
// Records that this endpoint's session has acknowledged holding the snapshot
// at `snapshot_tick`. Monotonic: an older tick is ignored. No-op for an
// endpoint with no live session.
void noteSnapshotAck(const net::Endpoint& from, uint32_t snapshot_tick) noexcept;

// The newest snapshot tick this player has acknowledged; 0 when the player is
// unknown or has acknowledged nothing.
uint32_t ackedSnapshotTick(uint32_t player_id) const noexcept;
```

- Changed: `void Server::handleInput(const net::Endpoint& from, const net::PacketHeader& h, net::ByteReader& r)`
  — the dispatch site in `Server::tick` changes from `handleInput(slot.peer, r)`
  to `handleInput(slot.peer, h, r)`.

**Checkpoint 1: an acknowledgment is recorded and read back per player**

- [ ] **Step 1: Write the failing test, then run it**

Spec, in `tests/server/session_test.cpp` as
`TEST(SessionTest, RecordsAndReportsAnAcknowledgedSnapshotTick)`: build a
`SessionTable t`, bind an endpoint via the existing assign-on-first-sight call
to get `player_id`. Assert `t.ackedSnapshotTick(player_id) == 0` before any ack
and `t.ackedSnapshotTick(999) == 0` for an unknown id. Call
`t.noteSnapshotAck(ep, 120)`; assert `t.ackedSnapshotTick(player_id) == 120`.
Call `t.noteSnapshotAck(unbound_ep, 500)` for an endpoint with no session and
assert nothing changes (`ackedSnapshotTick(player_id)` is still `120`).

Run: `scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain -R session_test --output-on-failure"`
Expected: FAIL — compile error, `'class server::SessionTable' has no member named 'noteSnapshotAck'`.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: add `uint32_t acked_snapshot_tick = 0;` to `SessionTable::Entry`.
`noteSnapshotAck` locates the entry by endpoint (the same lookup `touch` uses)
and assigns; a miss is a silent no-op. `ackedSnapshotTick` uses `findByPlayer`
and returns `0` on a miss.

```bash
scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain -R session_test --output-on-failure" && \
  git add src/server/session.h src/server/session.cpp tests/server/session_test.cpp && \
  git commit -m "feat: track the snapshot each session has acknowledged"
```

Expected: PASS, then one commit.

**Checkpoint 2: an older acknowledgment never moves the baseline backwards**

- [ ] **Step 1: Write the failing test, then run it**

Spec, as `TEST(SessionTest, IgnoresAnAcknowledgmentOlderThanTheStoredOne)`:
bind an endpoint, `noteSnapshotAck(ep, 120)`, then `noteSnapshotAck(ep, 90)` and
assert `ackedSnapshotTick(player_id)` is still `120`. Then
`noteSnapshotAck(ep, 121)` and assert it becomes `121`. Finally
`noteSnapshotAck(ep, 0)` and assert it is still `121`.

Run: `scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain -R session_test --output-on-failure"`
Expected: FAIL — Checkpoint 1's unconditional assignment lets `90` overwrite
`120`, so the first assertion reads `90`.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: `noteSnapshotAck` assigns only when `snapshot_tick > acked_snapshot_tick`.
This matters beyond tidiness: `ack_tick` on an inbound input is
attacker-controlled, and UDP reorders freely. A backwards-walking baseline would
let a sender pin the server to an ancient snapshot — or, once Task 5 lands, to
one that has aged out of the history ring, forcing a full keyframe on every
broadcast. Monotonicity makes both harmless.

```bash
scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain -R session_test --output-on-failure" && \
  git add src/server/session.cpp tests/server/session_test.cpp && \
  git commit -m "fix: keep the acknowledged snapshot tick monotonic"
```

Expected: PASS, then one commit.

**Checkpoint 3: a reused player id does not inherit the departed session's baseline**

- [ ] **Step 1: Write the failing test, then run it**

Spec, as `TEST(SessionTest, ClearsTheAcknowledgedTickWhenASessionIsRemoved)`:
bind endpoint A, `noteSnapshotAck(epA, 120)`, remove A's session through
whatever public removal path `session_test.cpp` already exercises, then bind a
**different** endpoint B and assert B is issued the same `player_id` A had (the
reuse the table already does) **and** that `ackedSnapshotTick(player_id) == 0`,
not `120`.

Run: `scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain -R session_test --output-on-failure"`
Expected: FAIL — `removeAt` clears the player binding but not the new field, so
the assertion reads `120`.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: reset `acked_snapshot_tick = 0` in `SessionTable::removeAt`, alongside
whatever that function already clears. This is the same class of bug P3's
security review found and fixed for `InputBuffer` (a departed player's queued
input surviving into a reused id) and explicitly left open for `hits_`. Here it
is a correctness bug, not just a cosmetic one: an inherited baseline makes the
server send Task 5's deltas against a snapshot the new occupant never received,
and the new client would discard every one of them.

```bash
scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain -R session_test --output-on-failure" && \
  git add src/server/session.cpp tests/server/session_test.cpp && \
  git commit -m "fix: clear a session's acknowledged snapshot tick on removal"
```

Expected: PASS, then one commit.

**Checkpoint 4: an inbound input's `ack_tick` reaches the session table**

- [ ] **Step 1: Write the failing test, then run it**

Spec, in `tests/server/server_test.cpp` as
`TEST(ServerTest, InputAcknowledgmentUpdatesTheSessionsBaseline)`: stand up the
`RecordingTransport` harness the file already uses, join a client, then send an
`Input` packet whose **header** carries `ack_tick = 42` and whose payload is a
valid `InputCommand` for that player. Drive `srv->tick(...)` once. Assert
`srv->sessions().ackedSnapshotTick(player_id) == 42` — add a
`const SessionTable& sessions() const noexcept` accessor to `Server` if one does
not already exist, since the server's session table is otherwise private and
this checkpoint has no other observable signal.

Run: `scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain -R server_test --output-on-failure"`
Expected: FAIL — `handleInput` never sees the header, so the stored tick is `0`.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: change `handleInput` to take `const net::PacketHeader& h` as its
second parameter and update the single dispatch site in `Server::tick`. After
the existing `authorize` gate succeeds — never before it, an unauthorized sender
must not be able to move another session's baseline — call
`sessions_.noteSnapshotAck(from, h.ack_tick)`. Add the `sessions()` accessor if
needed.

```bash
scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain -R server_test --output-on-failure" && \
  git add src/server/server.h tests/server/server_test.cpp && \
  git commit -m "feat: read the client's snapshot acknowledgment off its inputs"
```

Expected: PASS, then one commit.

**Task boundary:** full suite, plain configuration.

```bash
scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain --output-on-failure"
```

---

## Task 5: the server broadcasts deltas against each session's baseline

The first task where bytes actually come off the wire. `broadcastSnapshot`
stops encoding one payload for everybody and starts encoding per session,
because each session may hold a different baseline.

**Files:**
- Modify: `src/server/server.h`
- Modify: `tests/server/server_test.cpp`

**Interfaces:**
- Consumes: `net::SnapshotRing` (Task 1), `net::encodeSnapshotDelta` (Task 2),
  `SessionTable::ackedSnapshotTick` (Task 4).
- Produces, on `server::Server<T>`:

```cpp
uint64_t deltasSent() const noexcept;            // kSnapshotDelta packets sent
uint64_t keyframesSent() const noexcept;         // kSnapshot packets sent
uint64_t snapshotBytesSent() const noexcept;     // payload bytes actually sent, summed
uint64_t snapshotBytesFullEquivalent() const noexcept;  // what full snapshots would have cost
```

**Checkpoint 1: a session holding a known baseline is sent a delta**

- [ ] **Step 1: Write the failing test, then run it**

Spec, in `tests/server/server_test.cpp` as
`TEST(ServerTest, SendsADeltaToASessionHoldingAKnownBaseline)`: join one client
over the `RecordingTransport` harness. Drive `srv->tick()` far enough to emit at
least two snapshots (`kSnapshotIntervalTicks == 3`, so six ticks emits two).
Read the first recorded `kSnapshot` packet's header `tick` — call it `T0`. Now
send an `Input` whose header carries `ack_tick = T0`, drive ticks until the next
broadcast, and assert the newest recorded packet's header `type` is
`net::MsgType::kSnapshotDelta`, with a `payload_len` strictly smaller than the
first snapshot's.

Also assert, as a regression pin in the same test, that a **second** client that
joins and acknowledges nothing still receives `net::MsgType::kSnapshot`. This
assertion passes both before and after this checkpoint — it is a guard on the
keyframe fallback, not this checkpoint's RED signal.

Run: `scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain -R server_test --output-on-failure"`
Expected: FAIL — the server sends `kSnapshot` to everyone, so the type assertion
reads `kSnapshot` where `kSnapshotDelta` is expected.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: add a `net::SnapshotRing history_;` member to `Server`. In
`broadcastSnapshot`, after `world_.writeSnapshot(snapshot_)`, call
`history_.store(snapshot_)`. Then, per session, resolve
`const sim::WorldSnapshot* baseline = history_.find(sessions_.ackedSnapshotTick(pid));`
and:
- `baseline != nullptr && baseline->tick != snapshot_.tick` → encode with
  `net::encodeSnapshotDelta(*baseline, snapshot_, ...)` into a per-iteration
  `net::ByteWriter` over `snapshot_payload_`, header `type = kSnapshotDelta`
- otherwise → the existing full `net::encodeSnapshot`, header `type = kSnapshot`

An `encodeSnapshotDelta` returning `false` falls back to the full encoding for
that session rather than dropping the packet; count it in `dropped_` only if the
full encoding also fails, exactly as today. Everything else about the header
(`tick`, `send_time_ms`, per-session `ack_tick`) is unchanged.

Note the deliberate cost: the payload is now encoded once per session instead of
once per broadcast. At 32 sessions × 20 Hz that is 640 encodes/second of a
sub-microsecond function, which is not worth optimizing ahead of P5's
measurements. Do **not** add a group-by-baseline cache.

```bash
scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain -R server_test --output-on-failure" && \
  git add src/server/server.h tests/server/server_test.cpp && \
  git commit -m "feat: send each session a delta against the snapshot it holds"
```

Expected: PASS, then one commit.

**Checkpoint 2: the delta the server sends reconstructs the server's own world**

- [ ] **Step 1: Write the failing test, then run it**

Spec, as `TEST(ServerTest, ASentDeltaReconstructsTheAuthoritativeSnapshot)`:
join two clients so the world holds two players, and drive input for one of them
so they are actually moving and the delta is non-trivial. Capture the full
`kSnapshot` at tick `T0` and decode it with `net::decodeSnapshot` into
`baseline`. Acknowledge `T0`, drive to the next broadcast, and take the
`kSnapshotDelta` packet. Decode it with `net::decodeSnapshotDelta`, then
`net::applySnapshotDelta(baseline, d, out)` — assert it returns `true`. Finally
assert `out` matches the server's own world: `sim::WorldSnapshot expected{};
srv->world().writeSnapshot(expected);` — the same accessor `server_test.cpp`
already uses at lines 80 and 103 — and compare `count` and every player's
`id`/`x`/`y`/`vx`/`vy`/`radius` under `==`. Compare `out.tick` against the
delta packet's header `tick`, not against `expected.tick`: the world has
advanced by the time the test reads it, and the snapshot was cut earlier.

Run: `scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain -R server_test --output-on-failure"`
Expected: PASS is the likely outcome given Task 2's equivalence property and
Checkpoint 1's wiring. It is a separate checkpoint because it is the first test
that proves the *server's* baseline bookkeeping — not just the codec — is
correct: a wrong `baseline_tick`, a stale ring entry, or storing the snapshot
after sending rather than before would all pass Checkpoint 1 and fail here.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: no new code expected. If `applySnapshotDelta` returns `false`, the
cause is almost certainly `history_.store` running *after* the send loop rather
than before it, or the loop resolving the baseline against `snapshot_.tick`
instead of the session's acknowledged tick.

```bash
scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain -R server_test --output-on-failure" && \
  git add src/server/server.h tests/server/server_test.cpp && \
  git commit -m "test: pin that a sent delta rebuilds the authoritative snapshot"
```

Expected: PASS, then one commit.

**Checkpoint 3: a baseline that has aged out of the ring falls back to a keyframe**

- [ ] **Step 1: Write the failing test, then run it**

Spec, as `TEST(ServerTest, FallsBackToAKeyframeWhenTheBaselineHasAgedOut)`: join
a client, acknowledge an early snapshot tick `T0`, then drive the server through
more than `net::kSnapshotRingSlots` further broadcasts (16 snapshots ×
3 ticks = 48 ticks; drive 60 to be safe) **without** acknowledging anything
newer. Assert the newest recorded packet's type is `net::MsgType::kSnapshot`,
not `kSnapshotDelta` — `T0` is no longer in `history_`, so there is nothing to
delta against.

Run: `scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain -R server_test --output-on-failure"`
Expected: PASS — Checkpoint 1's contract already routes a `nullptr` baseline to
the full encoding. Committed as the regression pin it is: the fallback is what
keeps a client that has fallen behind (or is being starved by an attacker
withholding acknowledgments) recoverable rather than permanently desynced.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: no new code expected.

```bash
scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain -R server_test --output-on-failure" && \
  git add tests/server/server_test.cpp && \
  git commit -m "test: pin the keyframe fallback for an aged-out baseline"
```

Expected: PASS, then one commit.

**Checkpoint 4: the server reports what delta encoding saved**

- [ ] **Step 1: Write the failing test, then run it**

Spec, as `TEST(ServerTest, ReportsSnapshotByteSavings)`: join a client, drive
several broadcasts with the client acknowledging each snapshot as it arrives, and
assert:
- `srv->keyframesSent() >= 1` and `srv->deltasSent() >= 1`
- `srv->snapshotBytesSent() > 0`
- `srv->snapshotBytesFullEquivalent() > srv->snapshotBytesSent()` — the whole
  point of the phase, asserted as an inequality rather than an exact figure so
  the test does not re-pin the layout the golden vector already owns
- `srv->keyframesSent() + srv->deltasSent()` equals the number of snapshot-type
  packets the `RecordingTransport` actually captured

Run: `scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain -R server_test --output-on-failure"`
Expected: FAIL — compile error, `'class server::Server<...>' has no member named 'deltasSent'`.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: add four `uint64_t` members and their accessors. In the per-session
branch of `broadcastSnapshot`, add the payload size actually written to
`snapshot_bytes_sent_` and increment `deltas_sent_` or `keyframes_sent_`; add
`net::kSnapshotFixedBytes + snapshot_.count * net::kPlayerStateBytes` to
`snapshot_bytes_full_equivalent_` on every session iteration regardless of
branch, so the two totals are comparable. Count payload bytes only, not the
24-byte header, since the header is identical either way.

```bash
scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain -R server_test --output-on-failure" && \
  git add src/server/server.h tests/server/server_test.cpp && \
  git commit -m "feat: count the bytes snapshot delta encoding saves"
```

Expected: PASS, then one commit.

**Task boundary:** full suite, plain configuration.

```bash
scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain --output-on-failure"
```

---

## Task 6: the client acknowledges and applies deltas

The other end of the delta path. After this task the wire half of P4 is
complete and measurable end to end; interpolation is still untouched.

**Files:**
- Modify: `src/client/client.h`
- Modify: `tests/client/client_test.cpp`

**Interfaces:**
- Consumes: `net::SnapshotRing`, `net::decodeSnapshotDelta`, `net::applySnapshotDelta`.
- Produces, on `client::Client<T>`:

```cpp
const net::SnapshotRing& snapshots() const noexcept;  // the interpolation buffer, Task 8 reads it
uint32_t deltasApplied() const noexcept;
uint32_t deltasDropped() const noexcept;  // baseline not held -- the self-healing path
```

**Checkpoint 1: every outgoing input acknowledges the newest snapshot held**

- [ ] **Step 1: Write the failing test, then run it**

Spec, in `tests/client/client_test.cpp` as
`TEST(ClientTest, InputAcknowledgesTheNewestSnapshotHeld)`: stand up the
`RecordingTransport` harness the file already uses, join the client, and deliver
a `kSnapshot` at `tick = 77` containing the client's own player. Call
`sendInput(...)`, then decode the header of the last packet the transport
recorded and assert `h.ack_tick == 77`. Before any snapshot has arrived, assert
the same field is `0` (send an input immediately after the join accept and
decode it).

Run: `scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain -R client_test --output-on-failure"`
Expected: FAIL on the `77` assertion — `sendInput` never sets `ack_tick`, so it
reads `0`. The pre-snapshot `0` assertion passes already and is a pin.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: in `Client::sendInput`, set `h.ack_tick = latest_snapshot_tick_`
alongside the existing `h.tick` and `h.send_time_ms`. `latest_snapshot_tick_`
is only ever assigned from a snapshot the client has accepted *and* stored, so
it can never name a baseline the client does not hold — which is what makes the
server's delta self-healing under packet loss: a lost delta leaves this value at
the older tick, and the server's next delta is cut against that same older
baseline.

```bash
scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain -R client_test --output-on-failure" && \
  git add src/client/client.h tests/client/client_test.cpp && \
  git commit -m "feat: acknowledge the held snapshot on every outgoing input"
```

Expected: PASS, then one commit.

**Checkpoint 2: a delta against a held baseline updates the client's world**

- [ ] **Step 1: Write the failing test, then run it**

Spec, as `TEST(ClientTest, AppliesADeltaAgainstAHeldBaseline)`: join the client,
deliver a full `kSnapshot` at `tick = 100` with two players — the client's own
and a remote at `{2, 1.0f, 1.0f, 0.0f, 0.0f, kPlayerRadius}`. Assert
`latestSnapshotTick() == 100`. Now build a `kSnapshotDelta` payload with
`net::encodeSnapshotDelta` against that same snapshot, moving player 2 to
`{2, 4.0f, 1.0f, sim::kMoveSpeed, 0.0f, kPlayerRadius}` at `tick = 103`, frame it
with header `type = kSnapshotDelta`, `tick = 103`, and deliver it. Then:
`latestSnapshotTick() == 103`; `latestSnapshot()` reports player 2 at
`x == 4.0f` and `vx == sim::kMoveSpeed`; `deltasApplied() == 1`;
`deltasDropped() == 0`; and `snapshots().find(103)` is non-null — proving the
reconstructed snapshot was stored, which is what lets the *next* delta be cut
against it.

Run: `scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain -R client_test --output-on-failure"`
Expected: FAIL — compile error, `'class client::Client<...>' has no member named 'deltasApplied'`.
The `kSnapshotDelta` packet is also silently ignored today, since
`handlePacket`'s switch has no case for it.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract:
- Add a `net::SnapshotRing snapshots_;` member and the three accessors.
- In `handleSnapshot`, after the existing strictly-newer check and the
  assignment to `snapshot_`, call `snapshots_.store(snapshot_)` — full snapshots
  enter the ring too, and are the baselines every first delta is cut against.
- Add `case net::MsgType::kSnapshotDelta:` to `handlePacket`'s switch, calling a
  new `handleSnapshotDelta(r, h, now_ms)`.
- `handleSnapshotDelta`: `decodeSnapshotDelta`; return on failure. Look up
  `snapshots_.find(d.baseline_tick)`; if absent, `++deltas_dropped_` and return.
  `applySnapshotDelta` into a local `sim::WorldSnapshot`; return on failure.
  Then run **exactly the same acceptance path a full snapshot takes** — the
  strictly-newer `h.tick` check, `snapshot_ = reconstructed`,
  `latest_snapshot_tick_ = h.tick`, `server_time_ms_`, `clock_.observe`, the
  RTT estimate, `snapshots_.store`, and the prediction seed/reconcile loop.
  Factor that shared tail into one private helper called by both handlers rather
  than duplicating it; duplicating it is how the two paths drift and how a
  delta-fed client silently stops reconciling.

```bash
scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain -R client_test --output-on-failure" && \
  git add src/client/client.h tests/client/client_test.cpp && \
  git commit -m "feat: reconstruct the world from a snapshot delta"
```

Expected: PASS, then one commit.

**Checkpoint 3: a delta naming a baseline the client does not hold is dropped**

- [ ] **Step 1: Write the failing test, then run it**

Spec, as `TEST(ClientTest, DropsADeltaAgainstAnUnheldBaseline)`: join the client
and deliver a full snapshot at `tick = 100`. Deliver a `kSnapshotDelta` with
`baseline_tick = 55` (never received) at `h.tick = 103`. Assert:
`deltasDropped() == 1`; `deltasApplied() == 0`; `latestSnapshotTick()` is still
`100`; and `latestSnapshot()` still reports the tick-100 positions — a dropped
delta must leave the world exactly as it was, never half-applied.

Run: `scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain -R client_test --output-on-failure"`
Expected: PASS — Checkpoint 2's contract already returns early on a missing
baseline. Committed as a pin, because this is the path that makes packet loss
self-healing: the client keeps acknowledging `100`, so the server keeps cutting
deltas against `100` until one lands.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: no new code expected.

```bash
scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain -R client_test --output-on-failure" && \
  git add tests/client/client_test.cpp && \
  git commit -m "test: pin that an unresolvable delta leaves the world untouched"
```

Expected: PASS, then one commit.

**Checkpoint 4: client and server converge over a delta-only stream**

- [ ] **Step 1: Write the failing test, then run it**

Spec, in `tests/client/convergence_test.cpp` as
`TEST(ConvergenceTest, ClientTracksServerOverADeltaStream)`, following the
in-memory zero-latency convergence test P3 already built in that file. Run a real
`Server` and a real `Client` over the loopback pair for ~300 ticks with the
client sending continuous movement input. Assert at the end:
- `srv->deltasSent() > srv->keyframesSent()` — the steady state is deltas, with
  keyframes only at the start
- `client->deltasApplied() > 0` and `client->deltasDropped() == 0` over a lossless
  transport
- the client's `latestSnapshot()` position for its own player matches the
  server's authoritative position within the tolerance the existing convergence
  test already uses

Run: `scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain -R convergence_test --output-on-failure"`
Expected: PASS is the likely outcome. It is a separate checkpoint because it is
the first time the two ends run against each other for a sustained period: a
baseline that drifts, a ring that evicts too early, or an acknowledgment that
never advances would each show up here as `deltasDropped() > 0` or a keyframe
count that keeps climbing, and nowhere earlier.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: no new code expected. A climbing keyframe count means acknowledgments
are not reaching the server (check Task 4 Checkpoint 4's wiring); a climbing
drop count means the client is acknowledging a tick it did not store (check that
`snapshots_.store` runs on *both* the full and delta paths).

```bash
scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain -R convergence_test --output-on-failure" && \
  git add tests/client/convergence_test.cpp && \
  git commit -m "test: converge client and server over a delta-only snapshot stream"
```

Expected: PASS, then one commit.

**Task boundary:** full suite in **all three configurations** — this task
completed a change to the packet path that both ends now depend on.

```bash
scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain --output-on-failure" && \
scripts/tw bash -c "cmake --build build/asan -j8 && ctest --test-dir build/asan --output-on-failure" && \
scripts/tw bash -c "cmake --build build/tsan -j8 && setarch -R ctest --test-dir build/tsan --output-on-failure"
```

---

## Task 7: `client::Interpolator`

The second half of the phase, and a pure module: a render timeline plus a
bracketing lerp. It touches no transport, no clock and no `Client`, so every
behavior below is unit-testable with hand-built snapshots.

**Files:**
- Create: `src/client/interpolation.h`, `src/client/interpolation.cpp`
- Create: `tests/client/interpolation_test.cpp`
- Modify: `CMakeLists.txt` — `src/client/interpolation.cpp` into `libclient`
  (line 32), `tw_add_test(interpolation_test tests/client/interpolation_test.cpp)`

**Interfaces:**
- Consumes: `net::SnapshotRing` (Task 1), `sim::PlayerState`.
- Produces:

```cpp
namespace client {

// Two full snapshot intervals at 60 Hz: kSnapshotIntervalTicks (3) * 2 = 6
// ticks = 100 ms. One interval would leave zero margin -- a single late or
// lost snapshot would starve the timeline on every occurrence. Two means a
// lost snapshot costs nothing visible at all.
inline constexpr uint32_t kInterpDelayTicks = 6;

// |error| at or beyond this: jump rather than crawl. 18 ticks is 300 ms,
// far past anything the 1-tick-per-snapshot nudge should be asked to close.
inline constexpr int32_t kInterpSnapErrorTicks = 18;

// The render timeline for remote entities, deliberately behind the newest
// snapshot, and the interpolation that reads it.
//
// Unlike ClockSync this needs no EMA and no cooldown. ClockSync closes a loop
// over real dead time -- it observes the delayed consequence of its own past
// corrections, which is why reacting to every observation there produced a
// GROWING oscillation. Here the observed signal is the snapshot's own tick,
// which this class's corrections do not influence at all, so a plain
// snap-or-nudge is both sufficient and correct.
class Interpolator {
 public:
  void advance() noexcept;                               // once per client tick
  void observe(uint32_t newest_snapshot_tick) noexcept;  // once per accepted snapshot
  uint32_t renderTick() const noexcept;
  bool haveTimeline() const noexcept;
  uint32_t snaps() const noexcept;

  // The interpolated position of player_id at renderTick(). False when the
  // ring holds nothing usable for that player. Never extrapolates.
  bool sample(const net::SnapshotRing& ring, uint32_t player_id,
              float& x, float& y) const noexcept;

 private:
  uint32_t render_tick_ = 0;
  bool have_ = false;
  uint32_t snaps_ = 0;
};

}  // namespace client
```

**Checkpoint 1: the first snapshot seeds the timeline one delay behind it**

- [ ] **Step 1: Write the failing test, then run it**

Spec, in `tests/client/interpolation_test.cpp` as
`TEST(InterpolatorTest, FirstObservationSeedsTheTimelineBehindTheSnapshot)`:
a fresh `client::Interpolator interp;` reports `haveTimeline() == false` and
`renderTick() == 0`. After `interp.observe(100)`: `haveTimeline() == true`,
`renderTick() == 100 - client::kInterpDelayTicks` (94), and `snaps() == 0` — the
seed is not a snap. Then three `interp.advance()` calls leave `renderTick() == 97`.
Separately, on another fresh instance, `observe(3)` must **saturate at 0**, not
wrap: `renderTick() == 0` (3 − 6 underflows a `uint32_t`).

Run: `scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain -R interpolation_test --output-on-failure"`
Expected: FAIL — compile error, `client/interpolation.h: No such file or directory`.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: `advance()` increments `render_tick_` only when `have_`. `observe(t)`
computes `target = t >= kInterpDelayTicks ? t - kInterpDelayTicks : 0`; when
`!have_` it assigns `render_tick_ = target`, sets `have_ = true` and returns
without touching `snaps_`. The correction branch is Checkpoint 2.

```bash
scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain -R interpolation_test --output-on-failure" && \
  git add src/client/interpolation.h src/client/interpolation.cpp tests/client/interpolation_test.cpp CMakeLists.txt && \
  git commit -m "feat: run a render timeline one interpolation delay behind the server"
```

Expected: PASS, then one commit.

**Checkpoint 2: the timeline nudges toward small errors and snaps past large ones**

- [ ] **Step 1: Write the failing test, then run it**

Spec, as `TEST(InterpolatorTest, NudgesSmallDriftAndSnapsLargeDrift)`, one
instance carried through the whole sequence:
- `observe(100)` → `renderTick() == 94` (seed)
- three `advance()` → `97`; `observe(103)` → target is `97`, error `0`, so
  `renderTick()` stays `97` and `snaps() == 0`
- three `advance()` → `100`; `observe(103)` → target `97`, error `-3`, within
  the snap threshold, so exactly **one** tick of correction: `renderTick() == 99`
- `observe(103)` again → target `97`, error `-2`, one more: `renderTick() == 98`
- `observe(200)` → target `194`, error `+96` ≥ `kInterpSnapErrorTicks`, so
  `renderTick() == 194` and `snaps() == 1`

Run: `scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain -R interpolation_test --output-on-failure"`
Expected: FAIL on the first `observe(103)` after the seed — Checkpoint 1's
`observe` returns early for every observation once `have_` is set, so
`renderTick()` reads `100` where `99` is expected, and `snaps()` never leaves `0`.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: when `have_`, compute
`const int64_t error = static_cast<int64_t>(target) - static_cast<int64_t>(render_tick_);`
(64-bit, so the subtraction of two `uint32_t`s cannot wrap). If
`|error| >= kInterpSnapErrorTicks`, assign `render_tick_ = target` and
`++snaps_`. Otherwise apply at most one tick: `+1` when `error > 0`, `-1` when
`error < 0` (saturating at `0`), nothing when `error == 0`. One tick per
observation is 20 ticks/second of authority at the 20 Hz snapshot rate, which
closes any sub-threshold drift in well under a second.

```bash
scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain -R interpolation_test --output-on-failure" && \
  git add src/client/interpolation.cpp tests/client/interpolation_test.cpp && \
  git commit -m "feat: correct the render timeline toward the snapshot stream"
```

Expected: PASS, then one commit.

**Checkpoint 3: a player is interpolated between the two bracketing snapshots**

- [ ] **Step 1: Write the failing test, then run it**

Spec, as `TEST(InterpolatorTest, InterpolatesBetweenBracketingSnapshots)`. Build
a `net::SnapshotRing ring` holding two snapshots, each with one player id `9`:
- `tick = 100`, player 9 at `{9, 0.0f, 0.0f, 0.0f, 0.0f, kPlayerRadius}`
- `tick = 104`, player 9 at `{9, 10.0f, -4.0f, 0.0f, 0.0f, kPlayerRadius}`

`observe(108)` puts `renderTick()` at `102` — exactly halfway. Then
`sample(ring, 9, x, y)` returns `true` with `x == 5.0f` and `y == -2.0f`
exactly (the midpoint of these values is representable, so `==` is correct
here, not `EXPECT_NEAR`).

Second case in the same test, on a fresh instance and ring, with the realistic
3-tick spacing: snapshots at `100` (player 9 at `x = 0.0f`) and `103`
(`x = 3.0f`), `observe(107)` → `renderTick() == 101` → alpha `1/3` →
`EXPECT_NEAR(x, 1.0f, 1e-5f)`.

Run: `scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain -R interpolation_test --output-on-failure"`
Expected: FAIL — compile error, `'class client::Interpolator' has no member named 'sample'`.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: `sample` resolves `a = ring.newestAtOrBefore(render_tick_)` and
`b = ring.oldestAfter(render_tick_)`. With both present and `player_id` in both,
compute
`alpha = static_cast<float>(render_tick_ - a->tick) / static_cast<float>(b->tick - a->tick)`
and return `a.x + (b.x - a.x) * alpha`, likewise for `y`. `b->tick > a->tick`
always holds by the ring's own contract, so the divisor cannot be zero. Return
`false` when `!have_`. The remaining combinations are Checkpoints 4 and 5 —
returning `false` for them for now is acceptable and is what those checkpoints
turn red.

```bash
scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain -R interpolation_test --output-on-failure" && \
  git add src/client/interpolation.h src/client/interpolation.cpp tests/client/interpolation_test.cpp && \
  git commit -m "feat: interpolate a remote player between two snapshots"
```

Expected: PASS, then one commit.

**Checkpoint 4: a starved timeline freezes instead of extrapolating**

- [ ] **Step 1: Write the failing test, then run it**

Spec, as `TEST(InterpolatorTest, FreezesAtTheNewestSnapshotRatherThanExtrapolating)`.
Ring holds ticks `100` (player 9 at `x = 0.0f`) and `103` (`x = 3.0f`).
- **past the newest**: `observe(116)` → `renderTick() == 110`, beyond everything
  held. `sample(ring, 9, x, y)` returns `true` with `x == 3.0f` **exactly** —
  the tick-103 value, not a velocity-projected one. Assert `x == 3.0f` rather
  than merely `x <= 3.0f`, so an extrapolating implementation fails loudly.
- **before the oldest**: on a fresh instance, `observe(101)` → `renderTick() == 95`,
  older than anything held. `sample` returns `true` with `x == 0.0f` (the
  tick-100 value).
- **empty ring**: a fresh instance and an empty `net::SnapshotRing` →
  `sample` returns `false`.

Run: `scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain -R interpolation_test --output-on-failure"`
Expected: FAIL on the first two cases — Checkpoint 3 only handles the
both-present case and returns `false` when either bracket is missing.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: in `sample`, when `b == nullptr` and `a != nullptr`, return `a`'s
position for `player_id` (`false` if the player is not in `a`). When
`a == nullptr` and `b != nullptr`, return `b`'s position (`false` if absent).
When both are `nullptr`, return `false`. Never project by velocity — Decision 5
records why: a wrong guess must be visibly retracted, and 100 ms of delay exists
precisely so this path is rare.

```bash
scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain -R interpolation_test --output-on-failure" && \
  git add src/client/interpolation.cpp tests/client/interpolation_test.cpp && \
  git commit -m "feat: freeze a starved render timeline at the newest snapshot"
```

Expected: PASS, then one commit.

**Checkpoint 5: players who join or leave mid-window are handled**

- [ ] **Step 1: Write the failing test, then run it**

Spec, as `TEST(InterpolatorTest, HandlesPlayersAppearingAndDisappearingMidWindow)`.
Ring holds tick `100` with player `8` only (at `x = 1.0f`) and tick `104` with
player `7` only (at `x = 9.0f`). `observe(108)` → `renderTick() == 102`, so both
brackets exist.
- **joined** (in `b`, not `a`): `sample(ring, 7, x, y)` returns `true` with
  `x == 9.0f` — held at its first known position rather than lerped from a
  position it never had.
- **departed** (in `a`, not `b`): `sample(ring, 8, x, y)` returns `false` — a
  player the newer snapshot does not list is gone, and must stop being drawn
  immediately rather than lingering for the length of the window.
- **never present**: `sample(ring, 30, x, y)` returns `false`.

Run: `scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain -R interpolation_test --output-on-failure"`
Expected: FAIL on the **joined** case — Checkpoint 3's both-present lookup finds
player 7 missing from `a` and returns `false` where `true` and `9.0f` are
expected. The departed and never-present cases already return `false` and are
pins.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: in the both-brackets-present path, look the player up in each. Both →
lerp (Checkpoint 3). Only `b` → return `b`'s position. Only `a` → return
`false`. Neither → `false`.

```bash
scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain -R interpolation_test --output-on-failure" && \
  git add src/client/interpolation.cpp tests/client/interpolation_test.cpp && \
  git commit -m "feat: hold a joining player and drop a departing one at the window edge"
```

Expected: PASS, then one commit.

**Task boundary:** full suite, plain configuration.

```bash
scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain --output-on-failure"
```

---

## Task 8: the client renders remote players from the interpolator

Wiring only — the algorithm landed in Task 7. This is also where P3's
prediction/interpolation split becomes an enforced invariant rather than a
convention: the local player is predicted and **never** interpolated.

**Files:**
- Modify: `src/client/client.h`
- Modify: `tests/client/client_test.cpp`

**Interfaces:**
- Consumes: `client::Interpolator`, `net::SnapshotRing` (already a member from
  Task 6).
- Produces, on `client::Client<T>`:

```cpp
void setInterpolationEnabled(bool on) noexcept;
bool interpolationEnabled() const noexcept;
uint32_t renderTick() const noexcept;   // 0 before the first snapshot

// The position to draw a REMOTE player at: interpolated when interpolation is
// on, the raw newest snapshot when it is off. False for the local player id
// (that one is predicted -- see localPosition) and for an unknown player.
bool remotePosition(uint32_t player_id, float& x, float& y) const noexcept;
```

**Checkpoint 1: a remote player is drawn between its two snapshot positions**

- [ ] **Step 1: Write the failing test, then run it**

Spec, in `tests/client/client_test.cpp` as
`TEST(ClientTest, RemotePlayerIsInterpolatedBetweenSnapshots)`: join the client,
then deliver two full snapshots containing the local player plus a remote
player `2` that moves a known distance — `tick = 100` with player 2 at
`{2, 0.0f, 0.0f, ...}` and `tick = 104` with `{2, 10.0f, 0.0f, ...}`. Then drive the render timeline to exactly `102` with a
**bounded loop** rather than a computed call count — the timeline seeds off
whichever snapshot arrives first (`100 - 6 = 94`) and is then both advanced by
`tick()` and nudged by `observe(104)`, so the exact number of calls is an
implementation detail this test must not encode:

```cpp
for (int i = 0; i < 64 && client->renderTick() != 102; ++i) client->tick(now_ms);
ASSERT_EQ(client->renderTick(), 102u);
```

Then `remotePosition(2, x, y)` returns `true` with `x == 5.0f`.

Assert also that `remotePosition(client->playerId(), x, y)` returns **`false`** —
the local player is predicted, never interpolated, and this accessor refuses it
rather than quietly returning a second, conflicting position for the same
entity.

Run: `scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain -R client_test --output-on-failure"`
Expected: FAIL — compile error, `'class client::Client<...>' has no member named 'remotePosition'`.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: add a `Interpolator interp_;` member and a
`bool interpolation_enabled_ = true;` flag. Call `interp_.advance()` in
`Client::tick`, next to the existing `++tick_` (before the receive loop, so a
snapshot arriving this tick corrects a timeline that has already moved). Call
`interp_.observe(h.tick)` from the shared snapshot-accept tail Task 6 factored
out, so both the full and delta paths feed it. `renderTick()` forwards to
`interp_.renderTick()`. `remotePosition` returns `false` immediately when
`player_id == player_id_` or `player_id == sim::kInvalidPlayerId`; otherwise it
forwards to `interp_.sample(snapshots_, player_id, x, y)` when interpolation is
enabled, and otherwise scans `snapshot_` for the id (the pre-P4 behavior).

```bash
scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain -R client_test --output-on-failure" && \
  git add src/client/client.h tests/client/client_test.cpp && \
  git commit -m "feat: draw remote players from the interpolated render timeline"
```

Expected: PASS, then one commit.

**Checkpoint 2: the toggle falls back to the raw newest snapshot**

- [ ] **Step 1: Write the failing test, then run it**

Spec, as `TEST(ClientTest, InterpolationToggleFallsBackToTheNewestSnapshot)`:
the same setup as Checkpoint 1, with `renderTick()` at `102` and
`remotePosition(2, x, y)` reporting `5.0f`. Then
`client->setInterpolationEnabled(false)`; assert `interpolationEnabled() == false`
and that `remotePosition(2, x, y)` now reports `x == 10.0f` — the raw tick-104
value, which is exactly the stepped, pre-P4 behavior the demo toggles back to.
Re-enable and assert it returns to `5.0f`, proving the toggle is pure and the
timeline was not reset while it was off.

Run: `scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain -R client_test --output-on-failure"`
Expected: FAIL — compile error, `'class client::Client<...>' has no member named 'setInterpolationEnabled'`.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: `setInterpolationEnabled` assigns the flag and nothing else — like
P3's prediction toggle it is pure, and `interp_` keeps advancing and observing
while it is off so re-enabling resumes immediately rather than re-seeding.

```bash
scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain -R client_test --output-on-failure" && \
  git add src/client/client.h tests/client/client_test.cpp && \
  git commit -m "feat: make interpolation runtime-toggleable for the demo"
```

Expected: PASS, then one commit.

**Task boundary:** full suite, plain configuration.

```bash
scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain --output-on-failure"
```

---

## Task 9: the apps report and demonstrate the phase

The numbers table is the project's headline artifact, so P4's savings have to
come out of a real binary, not a unit test. The GUI gains the A/B toggle that
makes interpolation visible the way `P` made prediction visible at P3.

**Files:**
- Modify: `apps/tw_server.cpp` — snapshot byte summary
- Modify: `apps/tw_loadclient.cpp` — per-client delta stats
- Modify: `scripts/e2e-udp.sh` — assert both
- Modify: `apps/tw_client.cpp` — interpolated remote rendering, `I` toggle, HUD

**Interfaces:** consumes the accessors from Tasks 5, 6 and 8. Adds none.

**Checkpoint 1: the server reports what delta encoding saved over a real run**

- [ ] **Step 1: Write the failing test, then run it**

Spec: extend `scripts/e2e-udp.sh` with a check mirroring its existing
`lead=` guard — after the run, `grep -q "snapshot_bytes=" "$server_out"` must
succeed, and the script must `exit 1` with a `FAIL:` message on `stderr` if it
does not. The e2e test target already runs this script, so the observable
signal is that CTest target failing.

Run: `scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain -R e2e --output-on-failure"`
Expected: FAIL — `FAIL: tw_server printed no snapshot_bytes= summary`, because
`tw_server` prints only `port=` and `ticks=`.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: after the existing `std::printf("ticks=%u\n", ticks_run);` in
`apps/tw_server.cpp`, print one line:
`snapshot_bytes=%llu full_equiv_bytes=%llu deltas=%llu keyframes=%llu`, sourced
from `srv->snapshotBytesSent()`, `srv->snapshotBytesFullEquivalent()`,
`srv->deltasSent()` and `srv->keyframesSent()`. Cast each to
`unsigned long long` for the format specifier — `PRIu64` would need
`<cinttypes>` and the rest of `apps/` does not use it.

```bash
scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain -R e2e --output-on-failure" && \
  git add apps/tw_server.cpp scripts/e2e-udp.sh && \
  git commit -m "feat: report snapshot byte savings at the end of a server run"
```

Expected: PASS, then one commit.

**Checkpoint 2: the load client reports its delta reception**

- [ ] **Step 1: Write the failing test, then run it**

Spec: extend `scripts/e2e-udp.sh` with a second guard requiring
`grep -q "deltas_applied=" "$loadclient_out"`, failing with its own `FAIL:`
message otherwise.

Run: `scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain -R e2e --output-on-failure"`
Expected: FAIL — `FAIL: tw_loadclient printed no deltas_applied= stats line`.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: extend the existing per-client stats line in `apps/tw_loadclient.cpp`
(the one already carrying `player=`, `rtt_ms=`, `lead=`, `pred_p50=` …) with
`deltas_applied=%u deltas_dropped=%u`, from `c.deltasApplied()` and
`c.deltasDropped()`. Keep it one line per client; do not restructure the
existing fields, since `e2e-udp.sh` greps `lead=` off this same line.

```bash
scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain -R e2e --output-on-failure" && \
  git add apps/tw_loadclient.cpp scripts/e2e-udp.sh && \
  git commit -m "feat: report delta reception from the load client"
```

Expected: PASS, then one commit.

**Checkpoint 3: the GUI draws interpolated remotes and toggles with `I`**

- [ ] **Step 1: Write the failing test, then run it**

There is no automated assertion available for raylib rendering — the same
limitation P2 and P3 both recorded. The gate is therefore the GUI build plus
`client_selftest`, and the behavioral verification is the manual smoke run in
Step 2.

Run: `scripts/tw bash -c "cmake -S . -B build/gui -DTW_BUILD_GUI=ON && cmake --build build/gui -j8 && ctest --test-dir build/gui -R client_selftest --output-on-failure"`
Expected: PASS before the change (the GUI already builds) — this run establishes
the baseline. Record that it passed; the RED signal for this checkpoint is
Step 2's compile, which fails until `remotePosition` is actually called.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract, in `apps/tw_client.cpp`:
- Replace the remote-player draw loop's snapshot read with
  `client->remotePosition(snap.players[i].id, rx, ry)`, skipping any player the
  call returns `false` for. The existing `if (snap.players[i].id == client->playerId()) continue;`
  guard becomes redundant (`remotePosition` refuses the local id) but leave it —
  it documents the split at the call site.
- `if (IsKeyPressed(KEY_I)) client->setInterpolationEnabled(!client->interpolationEnabled());`
  next to the existing `KEY_P` handler.
- Draw remote players in `RED` while interpolating and `ORANGE` while not, so
  the toggle is visible without reading the HUD — the same device the green/
  yellow local player already uses for prediction.
- Extend the HUD `TextFormat` with ` interp=%s render=%u`, from
  `client->interpolationEnabled() ? "on" : "off"` and `client->renderTick()`.

Then run the demo manually and confirm it behaves: two clients, one moving,
watched from the other. Interpolation on should make the remote player glide;
`I` off should make it step visibly at 20 Hz. Launch **both processes inside a
single container** — two `scripts/tw` invocations are two network namespaces and
the client will never reach the server (a real mistake made while verifying P3):

```bash
scripts/tw bash -c '
./build/gui/tw_server --port 0 > /tmp/tw_server.out 2>&1 &
for i in $(seq 1 50); do grep -q "^port=" /tmp/tw_server.out 2>/dev/null && break; sleep 0.1; done
port=$(grep "^port=" /tmp/tw_server.out | head -1 | cut -d= -f2)
./build/gui/tw_client --host 127.0.0.1 --port "$port" --latency-ms 150 &
./build/gui/tw_client --host 127.0.0.1 --port "$port" --latency-ms 150
'
```

The interactive judgment ("does it actually look smooth?") cannot be made by the
executing session — it renders through WSLg onto the host desktop, which no
session tool can capture. Ask the user to run the command above and confirm,
exactly as P3's prediction toggle was confirmed. Record the outcome in Task 11's
history entry.

```bash
scripts/tw bash -c "cmake --build build/gui -j8 && ctest --test-dir build/gui -R client_selftest --output-on-failure" && \
  git add apps/tw_client.cpp && \
  git commit -m "feat: render interpolated remotes and toggle interpolation with I"
```

Expected: PASS, then one commit.

**Task boundary:** the three headless configurations plus the GUI build.

```bash
scripts/tw bash scripts/ci.sh && \
scripts/tw bash -c "cmake --build build/gui -j8 && ctest --test-dir build/gui -R client_selftest --output-on-failure"
```

---

## Task 10: mandatory security review

`CLAUDE.md` makes this non-optional for any phase touching the network surface,
and this phase added a message type, a new decoder, and a new attacker-
influenced field path (`ack_tick` inbound). P1, P2 and P3 each ran one; their
findings are recorded in `docs/project-history.md` and are the precedent for how
to treat what turns up.

**Files:** whatever the findings require, plus `docs/project-history.md`.

**Interfaces:** none.

**Checkpoint 1: review, then fix or record every finding**

- [ ] **Step 1: Run the review**

Run the `security-review` skill (or the `security-reviewer` agent) over
`src/net/snapshot_delta.cpp`, `src/net/snapshot_ring.cpp`,
`src/client/interpolation.cpp`, and the P4 diffs to `src/server/server.h`,
`src/server/session.cpp` and `src/client/client.h`. Run the `cpp-reviewer`
agent over the same set, as P3 did — the two find different classes of problem.

Threat model, unchanged from P1: an unauthenticated attacker controls every byte
of every datagram, can send them at any rate, and can forge any source address
the network lets through. Specific questions this phase raises, each of which
must be answered explicitly in the write-up rather than left implied:
- Can a crafted `changed_mask` / `present_mask` pair drive a read or write
  outside `records` or `out.players`? (`popcount` of a `uint32_t` is at most 32,
  which is `sim::kMaxPlayers` exactly — confirm that equality is enforced and
  not merely true by luck.)
- Can an attacker's `ack_tick` on a forged input move another session's
  baseline? (Task 4 Checkpoint 4 places the call after `authorize`; confirm.)
- Can a session be pinned to permanent keyframes, or to a baseline that makes
  the server do unbounded work per broadcast?
- Does `applySnapshotDelta` reject every input it cannot fully reconstruct,
  leaving `out` untouched rather than half-populated?
- Do the two new rings (`net::SnapshotRing` on each end) have any unbounded
  growth or tick-wrap path?
- Re-verify, do not assume, that P1/P2/P3's still-deferred findings are
  unaffected by this phase's changes — P3's review established that re-checking
  after a rewrite is the standard, not re-reading the old write-up.

- [ ] **Step 2: Act on the findings, then commit**

For each finding: fix it with a regression test proving RED then GREEN, or
record it as deferred with the reasoning, matching how P1–P3 handled theirs.
CRITICAL and HIGH findings are fixed in this task, not deferred, unless the fix
would require a wire-format change — in which case stop and raise it with the
user rather than deciding unilaterally. Append the full write-up to
`docs/project-history.md`'s P4 section under a
`### Task 10 boundary — mandatory security review findings` heading.

Commit each fix separately, chained behind its own scoped test, then the
write-up:

```bash
scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain --output-on-failure" && \
  git add docs/project-history.md && \
  git commit -m "docs: record the P4 security review findings"
```

Expected: PASS, then one commit per fix plus this one.

**Task boundary:** full suite in all three configurations, since fixes may have
touched the decode path.

```bash
scripts/tw bash scripts/ci.sh
```

---

## Task 11: documentation and final verification

The phase is not done when the code works; it is done when the next session can
pick it up cold. `docs/wire-format.md` is authoritative for the format and now
describes a message that does not exist in it.

**Files:**
- Modify: `docs/wire-format.md`
- Modify: `docs/project-history.md`
- Modify: `CLAUDE.md`
- Modify: `README.md`

**Interfaces:** none.

**Checkpoint 1: the wire format documents the new message**

- [ ] **Step 1: Write the documentation**

`docs/wire-format.md` needs, at minimum:
- `MsgType` updated: `kSnapshotDelta = 6`, `kMaxMsgType = 6`; the sentence
  reserving `6..255` amended to note that `6` is now taken and P6's
  hit-feedback message should take `7`.
- A new `## SnapshotDelta payload — 16 + 20 × changed_count bytes` section with
  the offset table from Task 2, the mask semantics, the
  `changed_mask ⊆ present_mask` invariant, and the rule that a player absent
  from the baseline is always sent in full.
- The `ack_tick` header row amended: it is now populated by the **client** on
  `Input` packets too, naming the newest snapshot that client holds, and
  consumed by the server as the delta baseline selector. Note that `0` means
  "no snapshot held", which is what makes the server send a keyframe.
- The "Maximum packet size" section extended with the delta's own arithmetic
  and the 48 → 58 worst-case player figure from Decision 6.
- A Version history entry: **P4 (2026-09-11): no version bump,
  `kProtocolVersion` stays 2**; one additive message type, using the extension
  path the document itself designated.
- The decoder-strictness list extended with the delta's four rejection cases.

- [ ] **Step 2: Verify the docs against the tests, then commit**

The golden byte vector in `tests/net/snapshot_delta_test.cpp` is authoritative;
read it and confirm every offset in the new section matches it. The document is
wrong if they disagree.

```bash
scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain -R 'snapshot_delta_test|protocol_test' --output-on-failure" && \
  git add docs/wire-format.md && \
  git commit -m "docs: document the snapshot delta message and its baseline ack"
```

Expected: PASS, then one commit.

**Checkpoint 2: the project history records P4's decisions**

- [ ] **Step 1: Write the history entry**

Replace the `<!-- Next section: ## P4 ... -->` placeholder at the end of
`docs/project-history.md` with a full `## P4 — Entity interpolation, snapshot
delta` section, following P2's and P3's structure. It must record, at minimum:
- the six decisions from this plan's "Decisions this plan makes" section, each
  with its reasoning — particularly **Decision 3**, which is a deliberate
  deviation from the architecture-resolution doc's "`radius` is the first field
  to drop", and the reasoning that replaced it
- the measured byte savings from a real `tw_server` run (the
  `snapshot_bytes=` / `full_equiv_bytes=` line from Task 9), not the predicted ones
- the 48 → 58 worst-case player-ceiling figure, which is the concrete numeric
  justification the resolution doc asked P4 to produce
- anything the execution discovered that contradicted this plan — P3's history
  entry records three plan-writing mistakes caught by their own checkpoints, and
  that record is more valuable than a clean narrative
- the human verification outcome from Task 9 Checkpoint 3
- a new `<!-- Next section: ## P5 — Threading, queue benchmark -->` placeholder

Also add to `CLAUDE.md`, under "Verified constraints", any invariant this phase
established that a future session could break unknowingly — at minimum the
prediction/interpolation split (the local player is predicted and never
interpolated; `remotePosition` refuses the local id) if execution confirmed it
is load-bearing. Add nothing that execution did not actually verify.

Update `README.md`'s controls list with `I`, alongside the existing `P` and
`[`/`]`.

- [ ] **Step 2: Verify the whole branch, then commit**

```bash
scripts/tw bash scripts/ci.sh && \
scripts/tw bash -c "cmake --build build/gui -j8 && ctest --test-dir build/gui -R client_selftest --output-on-failure" && \
  git add docs/project-history.md CLAUDE.md README.md && \
  git commit -m "docs: record P4's interpolation and snapshot delta design"
```

Expected: all three configurations plus the toolchain assertions green, the GUI
selftest green, then one commit.

**Task boundary — the phase is complete when this passes.** The branch is now
green and verified.

**Do not merge.** `executing-plans` hands off to the
`finishing-a-development-branch` skill, which owns the merge/PR/keep decision
and deliberately keeps it with the user. Before that handoff, offer to write a
journal entry with the `journal` skill, following the pattern of
`journal/2026-09-09_0451_ansh_p3-execution.md`.
