# P2 — Authoritative Server, `World`, and the First Demo Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use the `executing-plans` skill to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Turn P1's bytes-on-a-wire into a running authoritative game: a deterministic `sim::World` (movement plus one hitscan shot), a single-threaded server that owns it behind an epoll/timerfd tick loop, a session layer that binds every input to the endpoint it arrived from, and two clients — headless for load, raylib for the demo — that render exactly what the server last said, visibly late.

**Architecture:** `libsim` gains `World`, the only place simulation state changes, still with no I/O, no clock, and no allocation. `libserver` sits above it: `SessionTable` (endpoint ↔ player binding, capacity, timeout), `PacketRing` (the I/O↔simulation seam, built with P5's exact `acquireWrite`/`commitWrite` API but single-threaded and non-atomic), and `Server<T>` — a template over P1's `Transport` concept, so the same server runs over `LoopbackTransport` in tests and `UdpTransport` in the demo. `libclient` mirrors it with `Client<T>`. Executables in `apps/` are thin: argument parsing, a clock, and a loop.

**Tech Stack:** C++20 (GCC 10.5.0), CMake 3.28.4, GoogleTest, POSIX `epoll`/`timerfd`/`clock_gettime`, raylib via `FetchContent` (GUI build only), `std::span`, concepts.

**Spec:**
- [`docs/specs/2026-09-04-tickwire-design.md`](../specs/2026-09-04-tickwire-design.md) — the design; the P2 row, § Architecture (points 4–7), § `libsim`, § The demo
- [`docs/specs/2026-09-04-architecture-resolution.md`](../specs/2026-09-04-architecture-resolution.md) — **authoritative**; Q2 (`libsim` surface, `kMaxPlayers` derivation), Q3 (transport shape), Q4 (`SpscRing` contract, which `PacketRing` prefigures), Q5 residual risk 2 (the raylib display question, explicitly deferred to this phase)
- [`docs/wire-format.md`](../wire-format.md) — the P1 format this phase amends once, deliberately, in Task 3
- [`docs/project-history.md`](../project-history.md) — P0/P1 decisions and findings, including the two P1 security findings deferred to P2
- [`CLAUDE.md`](../../CLAUDE.md) — project conventions, auto-loaded

**Plan format:** spec-driven (inline execution), same as P1. Wire-format byte vectors are
given verbatim — they are the deliverable being frozen, not code derived from a contract.

**Suggested branch:** `phase-2-authoritative-server`

## Global Constraints

Everything in `CLAUDE.md` applies. Restated here because a cold executing window must
not have to infer any of it, plus the constraints P2 adds.

**Carried from `CLAUDE.md` (P0/P1-verified):**

- **Never invoke the host `g++`, `cmake`, or `ctest`.** All build/test commands go through `scripts/tw`, which runs them inside the pinned image with the repo bind-mounted at `/work`.
- **TSan runs are wrapped in `setarch -R`**; the capability flags in `scripts/tw` and `setarch -R` are both required, neither alone.
- **Float flags on every target linking `libsim`:** `-march=x86-64`, `-ffp-contract=off`. Never `-march=native`, never `-ffast-math`, never `-march=x86-64-v2` (absent in GCC 10).
- **No `std::format`** (GCC 10 lacks it) and **no `std::bit_cast`** (libstdc++ ships it from GCC 11). Pun `float`↔`uint32_t` with `std::memcpy`. Format output with `std::ostream`; do not add fmtlib for this phase — nothing here needs it.
- **`libsim` has no I/O, no wall-clock reads, and no allocation.** Only trivially-copyable POD crosses its boundary.
- **Protocol fields are explicitly little-endian**, written byte by byte through `net::ByteWriter`/`net::ByteReader`. Never `memcpy` a struct onto the wire, never `reinterpret_cast` a buffer to a struct. `_be` suffixes are reserved for values the *kernel* requires in network order (`Endpoint::addr_be`, `Endpoint::port_be`).
- **Decoders are strict:** reject rather than normalize or clamp. The one documented exception stays `InputCommand::fire` (`u8 != 0`).
- **`-Wall -Wextra -Werror`.**
- **Commits:** `type: description`. One commit per checkpoint, chained behind its test with `&&` — never `;`, never a separate line. `git add` names exact paths; **never** `git add -A` or `git add .`. Never `git commit --amend` — a follow-up commit instead.
- **Test scoping:** inside a checkpoint use `-R <regex>`; the full suite only at a task boundary.
- **Coverage:** 80% project-wide; **`libsim` is held to 100%** — this phase is the first to put real branches in it, so every rejection path in `World` needs a test.
- **File size:** 200–400 lines typical, 800 hard maximum.

**Added by P2:**

- **The simulation never reads a clock, and neither does anything testable.** `Server::tick(uint32_t now_ms)` and `Client::tick(uint32_t now_ms)` take the current monotonic millisecond as a *parameter*. `clock_gettime` is called in exactly one place — `server::monotonicMs()` in `src/server/clock.cpp` — and only `apps/` calls it. A test that needs time passes a number.
- **Every input packet is authorized against the endpoint it arrived from.** A decoded `InputCommand` is applied only when the source `Endpoint` has a live session *and* `player_id` equals that session's assigned id. This discharges the P1 security finding recorded in `docs/project-history.md`; it is not optional and not a later hardening pass.
- **Large objects are heap-allocated in tests.** `Server` (~330 KB: a 256-slot packet ring), `LoopbackTransport` (~310 KB) and `SimulatedTransport` (~157 KB) go through `std::make_unique`, never a stack local — the P1 constraint, extended to the new types.
- **The wire format is amended exactly once, in Task 3, and re-frozen there.** `kProtocolVersion` goes 1 → 2 in the same commit as the layout change, the golden byte vectors in `tests/net/protocol_test.cpp` are updated with it, and `docs/wire-format.md` plus `docs/project-history.md` are updated in Task 10. No other task changes a byte layout. If a later task discovers it needs a format change, that is a finding to record and raise, not a change to make quietly.
- **Changing the `Dockerfile` requires bumping the image tag in `scripts/tw`.** `scripts/tw` skips the build when a matching tag already exists locally, so editing the `Dockerfile` without bumping the tag silently reuses the stale image and the change appears not to work. Task 9 does both in one commit.
- **The GUI is behind `-DTW_BUILD_GUI=ON`, default OFF.** raylib is fetched and built only for that configuration, so `scripts/ci.sh` — which configures three build directories — does not pay for it three times, and CI (which has no display) does not depend on it.
- **`libserver` and `libclient` link `libnet` `PUBLIC`**, inheriting `tickwire_sim_flags` through the same build-graph guarantee, one layer further out.

---

## File Structure

| File | Responsibility |
|---|---|
| `src/sim/sim.h` | Gains movement/arena constants and `aim_x`/`aim_y` on `InputCommand`. |
| `src/sim/world.h`, `.cpp` | `sim::World` — roster, movement integration, arena clamping, hitscan. The only mutable simulation state in the project. |
| `src/net/bytes.h`, `.cpp` | Gains `ByteWriter::bytes(std::span<const std::byte>)`. |
| `src/net/protocol.h`, `.cpp` | Version 2 `InputCommand` layout; `encodeJoinAccept`/`decodeJoinAccept`. |
| `src/net/framing.h`, `.cpp` | `framePacket` — header + payload into one datagram buffer, `payload_len` computed, never guessed. |
| `src/server/packet_ring.h` | `PacketRing<T, N>` — the I/O↔simulation seam. P5 swaps an implementation behind this API. |
| `src/server/session.h`, `.cpp` | `SessionTable` — endpoint ↔ player id binding, capacity, per-session timeout and fire cooldown. |
| `src/server/server.h` | `Server<T>` — template over the `Transport` concept; ingest, route, step, broadcast. Header-only. |
| `src/server/clock.h`, `.cpp` | `monotonicMs()` — the only `clock_gettime` in the project. |
| `src/server/tick_timer.h`, `.cpp` | `TickTimer` — RAII `timerfd` at 60 Hz. |
| `src/server/poll_set.h`, `.cpp` | `PollSet` — RAII `epoll` fd multiplexing the socket against the timer. |
| `src/client/client.h` | `Client<T>` — join handshake, input send, snapshot store. Header-only. |
| `src/client/view.h`, `.cpp` | Pure world→screen mapping and HUD strings, so the renderer itself stays a thin uncovered shell. |
| `apps/tw_server.cpp` | Server executable: `--port`, `--ticks`. |
| `apps/tw_loadclient.cpp` | Headless load client: `--host`, `--port`, `--players`, `--ticks`. |
| `apps/tw_client.cpp` | raylib client: window, input, latency slider, `--selftest`. |
| `tests/sim/world_test.cpp` | Roster, movement, clamping, hitscan — every branch (100% floor). |
| `tests/net/framing_test.cpp` | `ByteWriter::bytes`, `framePacket` golden bytes, join-payload codecs. |
| `tests/server/packet_ring_test.cpp` | FIFO, full/empty, wrap. |
| `tests/server/session_test.cpp` | Binding, capacity, authorization, timeout, cooldown. |
| `tests/server/server_test.cpp` | Join, input routing, spoof rejection, broadcast cadence, leave — over `LoopbackTransport`. |
| `tests/server/timer_test.cpp` | `TickTimer` expirations, `PollSet` readiness. |
| `tests/client/client_test.cpp` | Handshake retransmit, snapshot store, end-to-end against a real `Server`. |
| `tests/client/view_test.cpp` | World→screen mapping. |
| `scripts/e2e-udp.sh` | Server + load client over real UDP on loopback, driven by CTest. |
| `scripts/demo.sh` | Server + raylib client in one container (one network namespace, X socket mounted). |

**Two shippable halves.** Tasks 1–8 produce a green, tested, headless system: server,
clients, and a real-UDP end-to-end test. Tasks 9–10 add the renderer and the docs. If
Task 9's display spike fails, Tasks 1–8 still constitute a complete phase — record the
finding, finish Task 10, and the display question becomes a follow-up rather than a
blocker. **This ordering is deliberate: the one task with an unresolved environment risk
is last.**

---

## Task 1: `World` — roster, movement, arena

`libsim`'s first real behavior. Everything here is pure: no clock, no I/O, no allocation,
fixed-capacity storage. `step()` takes no delta-time argument — that is the enforcement
mechanism for "no wall-clock inside the simulation", per resolution doc § Q2.

**Files:**
- Create: `src/sim/world.h`, `src/sim/world.cpp`, `tests/sim/world_test.cpp`
- Modify: `src/sim/sim.h`, `CMakeLists.txt`

**Interfaces:**
- Consumes: `sim::PlayerState`, `sim::InputCommand`, `sim::WorldSnapshot`, `sim::kMaxPlayers`, `sim::kTickDt` (all existing).
- Produces, in `namespace sim` (`src/sim/sim.h`):

```cpp
inline constexpr float kMoveSpeed    = 8.0f;   // world units per second
inline constexpr float kArenaHalf    = 50.0f;  // arena spans [-50, 50] on both axes
inline constexpr float kPlayerRadius = 0.5f;
inline constexpr uint32_t kInvalidPlayerId = 0;  // ids are 1-based; 0 means "none"
```

- Produces, in `namespace sim` (`src/sim/world.h`):

```cpp
class World {
 public:
  bool addPlayer(uint32_t id, float x, float y);   // false if full, id taken, id 0, or a non-finite coordinate
  bool removePlayer(uint32_t id);                  // false if absent
  bool hasPlayer(uint32_t id) const noexcept;
  uint32_t playerCount() const noexcept;
  void applyInput(const InputCommand& in);         // silently ignores an unknown player_id
  void step();                                     // advances exactly kTickDt
  void writeSnapshot(WorldSnapshot& out) const;    // caller-owned buffer
  uint32_t tick() const noexcept;
};
```

**Checkpoint 1: the roster is bounded, id-unique, and visible in a snapshot**

- [ ] **Step 1: Write the failing test, then run it**

Write `tests/sim/world_test.cpp`. Add `src/sim/world.cpp` to `libsim`'s sources and
`tw_add_test(world_test tests/sim/world_test.cpp)` to `CMakeLists.txt`.

Spec — on a default-constructed `World` (heap-allocated via `std::make_unique`; a
`WorldSnapshot` is ~800 bytes but `World` holds one internally plus its roster):
- `playerCount() == 0`, `tick() == 0`, and `hasPlayer(1) == false`.
- `addPlayer(1, 2.0f, 3.0f)` returns `true`; `playerCount() == 1`; `hasPlayer(1) == true`.
- `addPlayer(1, 0.0f, 0.0f)` returns `false` (duplicate id) and `playerCount()` stays `1`.
- `addPlayer(0, 0.0f, 0.0f)` returns `false` — `kInvalidPlayerId` is not a usable id.
- `addPlayer(2, std::numeric_limits<float>::quiet_NaN(), 0.0f)` returns `false`, and so does an infinite `y`; `playerCount()` stays `1`.
- Filling to capacity: adding ids `2..32` all return `true` (`playerCount() == 32`); `addPlayer(33, 0.0f, 0.0f)` returns `false`.
- `writeSnapshot(out)` on that full world sets `out.tick == 0`, `out.count == 32`, and every id `1..32` appears exactly once in `out.players[0..32)`; each entry's `radius == kPlayerRadius`, and the entry for id 1 has `x == 2.0f`, `y == 3.0f`, `vx == 0.0f`, `vy == 0.0f`.
- `removePlayer(1)` returns `true`; `playerCount() == 31`; a fresh `writeSnapshot` yields `count == 31` and no entry with `id == 1`. `removePlayer(1)` again returns `false`.
- After removing id 1, `addPlayer(33, 0.0f, 0.0f)` now returns `true` — the freed slot is reusable.

Snapshot ordering is deliberately **not** specified beyond "each id exactly once"; assert
with a search, not by index, so the storage layout stays free to change.

Run:
```bash
scripts/tw cmake -S . -B build/plain -DCMAKE_BUILD_TYPE=RelWithDebInfo && \
scripts/tw cmake --build build/plain -j8 --target world_test
```
Expected: FAIL at the build step — `src/sim/world.h: No such file or directory`.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: `World` holds `std::array<PlayerState, kMaxPlayers> players_`, a parallel
`std::array<bool, kMaxPlayers> occupied_`, a `std::array<InputCommand, kMaxPlayers>`
of latched inputs (used in Checkpoint 2), a `uint32_t count_` and a `uint32_t tick_`.
No allocation, no `std::vector`, no map.

`addPlayer` returns `false` when `id == kInvalidPlayerId`, when `hasPlayer(id)`, when
`count_ == kMaxPlayers`, or when either coordinate is not finite (`std::isfinite`).
Otherwise it takes the lowest free slot, stores `{id, x, y, 0, 0, kPlayerRadius}`,
zeroes that slot's latched input, and increments `count_`.

`writeSnapshot` sets `out.tick = tick_`, `out.count = count_`, and copies the occupied
slots into `out.players[0..count_)` in ascending slot order. Entries at or beyond
`count_` are left as the caller had them.

```bash
scripts/tw cmake --build build/plain -j8 --target world_test && \
  scripts/tw ctest --test-dir build/plain -R world_test --output-on-failure && \
  git add CMakeLists.txt src/sim/sim.h src/sim/world.h src/sim/world.cpp tests/sim/world_test.cpp && \
  git commit -m "feat: add sim::World with a bounded, id-unique player roster"
```

Expected: PASS, then one commit.

**Checkpoint 2: input sets velocity, `step` integrates it, and the input latches**

- [ ] **Step 1: Write the failing test, then run it**

Spec — a `World` with `addPlayer(1, 0.0f, 0.0f)`; `snapshotFor(id)` in the test is a
helper that calls `writeSnapshot` and returns the matching `PlayerState`:
- `applyInput({.player_id = 1, .tick = 0, .move_x = 1.0f, .move_y = 0.0f, .aim_x = 0.0f, .aim_y = 0.0f, .fire = false})` then `step()`: the player's `vx == kMoveSpeed`, `vy == 0.0f`, and `x == kMoveSpeed * kTickDt` **bit-exactly** (compare the `uint32_t` obtained by `std::memcpy`, computed in the test as `kMoveSpeed * kTickDt`, not as a decimal literal). `tick() == 1`, and a snapshot's `tick` field reads `1`.
- **Latching:** a second `step()` with no further input leaves `vx == kMoveSpeed` and puts `x` at exactly `2.0f * kMoveSpeed * kTickDt` — a dropped input packet must not stop the player dead.
- **Normalization:** input `move_x = 3.0f, move_y = 4.0f` (length 5) yields `vx == kMoveSpeed * 0.6f` and `vy == kMoveSpeed * 0.8f`, bit-exactly against those expressions.
- **Sub-unit input passes through unnormalized:** `move_x = 0.5f, move_y = 0.0f` yields `vx == kMoveSpeed * 0.5f` — analog input is scaled, not snapped to full speed.
- **Zero input stops the player:** `move_x = 0.0f, move_y = 0.0f` yields `vx == 0.0f`, `vy == 0.0f`, and a following `step()` leaves `x` unchanged.
- **Degenerate input is treated as zero:** `move_x = NaN`, `move_x = infinity`, and `move_x = 1e30f, move_y = 1e30f` (whose squared length overflows to infinity) each yield `vx == 0.0f, vy == 0.0f`. Position must stay finite through a following `step()`.
- **Unknown player:** `applyInput` with `player_id = 99` and with `player_id = 0` changes nothing — `playerCount()`, every position, and `tick()` are unaffected. Neither call may crash.
- `step()` on an empty world increments `tick()` and does nothing else.

Run: `scripts/tw cmake --build build/plain -j8 --target world_test && scripts/tw ctest --test-dir build/plain -R world_test --output-on-failure`
Expected: FAIL at the build step — `applyInput` and `step` are not declared.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: `applyInput` finds the slot for `in.player_id` (no slot → return, no effect)
and stores `in` as that slot's latched input, then recomputes that player's velocity:
let `len2 = move_x * move_x + move_y * move_y`. If `len2` is not finite or is `0.0f`,
velocity is `{0, 0}`. If `len2 > 1.0f`, velocity is `{move_x, move_y} * (kMoveSpeed / std::sqrt(len2))`.
Otherwise velocity is `{move_x, move_y} * kMoveSpeed`. `std::sqrt` is correctly rounded
under IEEE-754 and the pinned flags forbid contraction, so this is reproducible between
the two binaries.

`step()` adds `v * kTickDt` to each occupied player's position (clamping arrives in
Checkpoint 3), then increments `tick_`. Multiplication order is written once and never
reordered: `pos + vel * kTickDt`, matching `sim::advance`.

```bash
scripts/tw cmake --build build/plain -j8 --target world_test && \
  scripts/tw ctest --test-dir build/plain -R world_test --output-on-failure && \
  git add src/sim/world.h src/sim/world.cpp tests/sim/world_test.cpp && \
  git commit -m "feat: integrate latched player input at a fixed timestep"
```

Expected: PASS, then one commit.

**Checkpoint 3: players are clamped inside the arena on every wall**

- [ ] **Step 1: Write the failing test, then run it**

Spec — let `kBound = kArenaHalf - kPlayerRadius` (49.5). For each of the four walls,
place a player one step short of it and drive it outward for enough ticks to overshoot:
- `addPlayer(1, kBound - 0.01f, 0.0f)`, input `move_x = 1.0f`, then 10 `step()` calls → `x == kBound` exactly, and `vx` is still `kMoveSpeed` (velocity is not zeroed; only position is clamped).
- The mirrored cases: `-x` wall with `move_x = -1.0f` → `x == -kBound`; `+y` wall → `y == kBound`; `-y` wall → `y == -kBound`.
- A player at the corner driven diagonally (`move_x = 1.0f, move_y = 1.0f`, 200 steps) ends at exactly `{kBound, kBound}` and stays there across further steps — clamping is idempotent, not oscillating.
- A player moving inward from the wall leaves it: from `x == kBound`, input `move_x = -1.0f`, one `step()` → `x < kBound`.

Run: `scripts/tw cmake --build build/plain -j8 --target world_test && scripts/tw ctest --test-dir build/plain -R world_test --output-on-failure`
Expected: FAIL — Checkpoint 2's `step()` integrates unbounded, so `x` reads roughly `50.8` instead of `49.5` on the first case.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: after integrating, `step()` clamps each player's `x` and `y` into
`[-(kArenaHalf - kPlayerRadius), kArenaHalf - kPlayerRadius]`. Velocity is left
untouched — the player keeps pushing against the wall, which is what makes clamping
idempotent and keeps the snapshot's `vx`/`vy` a faithful report of intent. There is no
player-player collision in this project; that is a deliberate omission, not an oversight.

```bash
scripts/tw cmake --build build/plain -j8 --target world_test && \
  scripts/tw ctest --test-dir build/plain -R world_test --output-on-failure && \
  git add src/sim/world.h src/sim/world.cpp tests/sim/world_test.cpp && \
  git commit -m "feat: clamp players inside the arena bounds"
```

Expected: PASS, then one commit.

**Task boundary:**
```bash
scripts/tw ctest --test-dir build/plain --output-on-failure && \
scripts/tw cmake -S . -B build/asan -DCMAKE_BUILD_TYPE=RelWithDebInfo -DTW_SANITIZER=address,undefined && \
scripts/tw cmake --build build/asan -j8 && \
scripts/tw ctest --test-dir build/asan --output-on-failure
```

---

## Task 2: Hitscan

The one shot. Pure geometry against the world's current state — P6 will call exactly this
function after rewinding the world, which is why it takes no time and reads no state
beyond the roster.

**Files:**
- Modify: `src/sim/world.h`, `src/sim/world.cpp`, `tests/sim/world_test.cpp`

**Interfaces:**
- Produces, in `namespace sim`:

```cpp
// Nearest player (excluding the shooter) whose circle the ray from the shooter's
// position along (aim_x, aim_y) intersects. std::nullopt when nothing is hit.
std::optional<uint32_t> World::resolveHitscan(uint32_t shooter, float aim_x, float aim_y) const;
```

**Checkpoint 1: the nearest target along the ray is returned, and misses are not**

- [ ] **Step 1: Write the failing test, then run it**

Spec — a `World` with `addPlayer(1, 0.0f, 0.0f)` as the shooter and targets added as
listed per case. Aim vectors need not be unit length; the implementation normalizes.
- Targets `2` at `{10, 0}` and `3` at `{20, 0}`; `resolveHitscan(1, 1.0f, 0.0f)` returns `2` — the **nearer** target, not the first added. Adding them in the reverse order (`3` first) must still return `2`.
- Same world, `resolveHitscan(1, -1.0f, 0.0f)` returns `std::nullopt` — both targets are behind the ray.
- Target `2` at `{10, 0}`; `resolveHitscan(1, 1.0f, 1.0f)` returns `std::nullopt` — the ray passes wide.
- **Grazing is a hit:** target `2` at `{10.0f, 0.4f}` with `kPlayerRadius == 0.5f`; `resolveHitscan(1, 1.0f, 0.0f)` returns `2`. Moving it to `{10.0f, 0.6f}` returns `std::nullopt`.
- **Aim need not be normalized:** with target `2` at `{10, 0}`, `resolveHitscan(1, 7.5f, 0.0f)` returns `2`.
- **The shooter never hits itself:** with only players `1` and `2` at `{10, 0}` present, no aim direction returns `1`. Specifically `resolveHitscan(1, 1.0f, 0.0f)` returns `2` and, with player `2` removed, returns `std::nullopt`.
- **Degenerate inputs return `std::nullopt` and must not crash:** `resolveHitscan(1, 0.0f, 0.0f)` (zero aim), `resolveHitscan(1, NaN, 0.0f)`, `resolveHitscan(1, infinity, 0.0f)`, `resolveHitscan(99, 1.0f, 0.0f)` (unknown shooter), `resolveHitscan(0, 1.0f, 0.0f)` (invalid id), and any call on a world with only the shooter in it.
- **A target behind the shooter is never hit, even when overlapping:** shooter `1` at `{0, 0}`, target `2` at `{-0.1f, 0.0f}` (circles overlap); `resolveHitscan(1, 1.0f, 0.0f)` returns `std::nullopt`. This is the documented simplification — record it in the contract, not as a surprise.

Run: `scripts/tw cmake --build build/plain -j8 --target world_test && scripts/tw ctest --test-dir build/plain -R world_test --output-on-failure`
Expected: FAIL at the build step — `resolveHitscan` is not declared.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: return `std::nullopt` when the shooter is absent, when `aim_len2 = aim_x*aim_x + aim_y*aim_y`
is not finite or is `0.0f`. Otherwise let `d = {aim_x, aim_y} / std::sqrt(aim_len2)`. For
each occupied player other than the shooter, let `m = target.pos - shooter.pos` and
`t = m.x*d.x + m.y*d.y`. Skip when `t < 0.0f` (behind the ray origin). Otherwise the
perpendicular distance squared is `m.x*m.x + m.y*m.y - t*t`; it is a hit when that is
`<= kPlayerRadius * kPlayerRadius`. Track the hit with the smallest `t`; on an exact tie,
keep the **lower player id**, so the result never depends on slot order. `<optional>` is
included by `world.h`; `std::optional<uint32_t>` is trivially copyable and allocates
nothing, so the `libsim` boundary rule holds.

```bash
scripts/tw cmake --build build/plain -j8 --target world_test && \
  scripts/tw ctest --test-dir build/plain -R world_test --output-on-failure && \
  git add src/sim/world.h src/sim/world.cpp tests/sim/world_test.cpp && \
  git commit -m "feat: resolve hitscan against the nearest player along the ray"
```

Expected: PASS, then one commit.

**Checkpoint 2: an exact distance tie resolves to the lower player id**

- [ ] **Step 1: Write the failing test, then run it**

Spec — shooter `1` at `{0, 0}`. Add target `7` at `{10.0f, 0.3f}` **first**, then target
`3` at `{10.0f, -0.3f}`: both are at the same `t == 10.0f` along the `+x` ray and both are
within `kPlayerRadius` of it. `resolveHitscan(1, 1.0f, 0.0f)` returns `3`. Then build the
same world with the insertion order swapped (`3` first, then `7`) and assert it still
returns `3`.

Run: `scripts/tw cmake --build build/plain -j8 --target world_test && scripts/tw ctest --test-dir build/plain -R world_test --output-on-failure`
Expected: FAIL on the first ordering — Checkpoint 1's "smallest `t` wins, first found kept
on a tie" returns `7`, because slot order follows insertion order.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: in the candidate loop, replace the current best when `t < best_t`, **or** when
`t == best_t && id < best_id`. The tie-break exists so a hit is a function of world state
alone; without it, two servers with the same state but different join order would
disagree, which is exactly the class of divergence this project is built to exclude.

```bash
scripts/tw cmake --build build/plain -j8 --target world_test && \
  scripts/tw ctest --test-dir build/plain -R world_test --output-on-failure && \
  scripts/tw cmake --build build/asan -j8 --target world_test && \
  scripts/tw ctest --test-dir build/asan -R world_test --output-on-failure && \
  git add src/sim/world.cpp tests/sim/world_test.cpp && \
  git commit -m "feat: break hitscan distance ties by lower player id"
```

Expected: PASS under both configurations, then one commit.

**Task boundary:**
```bash
scripts/tw ctest --test-dir build/plain --output-on-failure && \
scripts/tw cmake --build build/asan -j8 && \
scripts/tw ctest --test-dir build/asan --output-on-failure
```

---

## Task 3: The wire format amendment, framing, and join payloads

**This is the one task in the phase that changes the frozen format**, and it re-freezes it
in the same breath. `InputCommand` gains an aim vector: the design's hitscan needs a
direction, P1 froze the payload before anything consumed it, and P6's server rewind
replays the shot from whatever this decides. Nothing is deployed, so the cost is a golden
vector update; deferring it costs a second re-freeze during the phase that is already the
riskiest.

`kProtocolVersion` goes to `2` in the same commit, so the change is explicit on the wire
rather than silent drift — that field exists for exactly this.

**Files:**
- Create: `src/net/framing.h`, `src/net/framing.cpp`, `tests/net/framing_test.cpp`
- Modify: `src/sim/sim.h`, `src/net/bytes.h`, `src/net/bytes.cpp`, `src/net/protocol.h`, `src/net/protocol.cpp`, `tests/net/protocol_test.cpp`, `tests/net/bytes_test.cpp`, `tests/net/robustness_test.cpp`, `CMakeLists.txt`

**Interfaces:**
- Consumes: `net::ByteWriter`, `net::ByteReader`, `net::PacketHeader`, `net::encodeHeader`, `net::kHeaderBytes`, `net::kMaxPacket`.
- Produces, in `namespace sim`: `InputCommand` gains `float aim_x, aim_y;` after `move_y` and before `fire`.
- Produces, in `namespace net`:

```cpp
inline constexpr uint8_t kProtocolVersion = 2;   // was 1; aim_x/aim_y added to InputCommand
inline constexpr size_t  kInputBytes      = 25;  // was 17
inline constexpr size_t  kJoinAcceptBytes = 4;

void ByteWriter::bytes(std::span<const std::byte> src) noexcept;  // appends verbatim

bool encodeJoinAccept(uint32_t player_id, ByteWriter& w);
bool decodeJoinAccept(ByteReader& r, uint32_t& out);

// Frames header + payload into out. Sets h.payload_len from payload.size().
// Returns bytes written, or 0 on failure (payload too large, out too small).
size_t framePacket(PacketHeader h, std::span<const std::byte> payload,
                   std::span<std::byte> out);
```

**The amended `InputCommand` layout** — 25 bytes, every multi-byte field little-endian:

| Offset | Size | Field |
|---:|---:|---|
| 0 | 4 | `player_id` |
| 4 | 4 | `tick` |
| 8 | 4 | `move_x` (IEEE-754 binary32) |
| 12 | 4 | `move_y` |
| 16 | 4 | `aim_x` |
| 20 | 4 | `aim_y` |
| 24 | 1 | `fire` (`0` or `1`) |

The header layout, `PlayerState` and `WorldSnapshot` are **unchanged**; only byte 4 of the
header (`version`) changes value, from `01` to `02`.

`JoinRequest` and `Leave` carry **no payload** (`payload_len == 0`). `JoinAccept` carries
the assigned `player_id` as 4 little-endian bytes.

**Checkpoint 1: `ByteWriter::bytes` appends verbatim and respects the sticky bound**

- [ ] **Step 1: Write the failing test, then run it**

Spec — extend `tests/net/bytes_test.cpp`:
- On a writer over an 8-byte buffer: `u8(0xAA)` then `bytes(span of {0x01, 0x02, 0x03})` produces `AA 01 02 03` and `size() == 4`; `ok()` stays `true`.
- An empty span is a no-op: `bytes({})` leaves `size()` and `ok()` unchanged.
- On a writer over a 3-byte buffer with `u16(0x1234)` already written (`size() == 2`), `bytes(span of 2 bytes)` does not fit: **nothing is written** (the third byte of the buffer, pre-filled with `0xEE`, is unchanged), `size()` stays `2`, and `ok()` becomes `false`. A following `u8(0x7F)` — which would fit — is still refused, and `size()` stays `2`.

Run: `scripts/tw cmake --build build/plain -j8 --target bytes_test && scripts/tw ctest --test-dir build/plain -R bytes_test --output-on-failure`
Expected: FAIL at the build step — `ByteWriter::bytes` is not declared.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: `bytes(src)` returns immediately when `!ok_`; sets `ok_ = false` and writes
nothing when `src.size()` exceeds the remaining room; otherwise `std::memcpy`s `src` at the
cursor and advances it. Same sticky, all-or-nothing semantics as every other writer method
— this method exists so `framePacket` can copy a payload without computing an offset
outside the cursor.

```bash
scripts/tw cmake --build build/plain -j8 --target bytes_test && \
  scripts/tw ctest --test-dir build/plain -R bytes_test --output-on-failure && \
  git add src/net/bytes.h src/net/bytes.cpp tests/net/bytes_test.cpp && \
  git commit -m "feat: add ByteWriter::bytes for verbatim payload appends"
```

Expected: PASS, then one commit.

**Checkpoint 2: `InputCommand` carries an aim vector, at protocol version 2**

- [ ] **Step 1: Write the failing test, then run it**

Spec — update `tests/net/protocol_test.cpp`:
- The golden header vector's byte 4 becomes `02`. The full 24 golden bytes for the header `{magic, version = 2, type = kInput, payload_len = 4, tick = 1234, send_time_ms = 123456, ack_tick = 1200, seq = 7, ack_seq = 9}` are now **exactly**:

```
54 57 49 52 02 01 04 00 D2 04 00 00 40 E2 01 00 B0 04 00 00 07 00 09 00
```

- A header whose byte 4 is `01` is now **rejected** — the previous version is not accepted. The existing "wrong version" case (byte 4 = `02` in the old test) must be re-pointed at `03`, so a valid-version case and an invalid-version case both remain.
- `sim::InputCommand{.player_id = 3, .tick = 1234, .move_x = 1.0f, .move_y = -0.5f, .aim_x = 0.0f, .aim_y = 1.0f, .fire = true}` encoded into a 25-byte buffer produces **exactly**:

```
03 00 00 00 D2 04 00 00 00 00 80 3F 00 00 00 BF 00 00 00 00 00 00 80 3F 01
```

with `w.size() == kInputBytes` (25). Decoding those 25 bytes returns `true`, every field compares equal, and all four floats compare **bit-exactly** via `std::memcpy` to `uint32_t`.
- Framing is still strict in both directions: every prefix length `0` through `24` is rejected, and the golden 25 bytes plus one trailing `0xEE` is rejected, each leaving the caller's `InputCommand` (sentinel `player_id = 0xAAAAAAAA`) untouched.
- Non-finite rejection extends to the new fields: the golden bytes with `aim_x` replaced by `0x7FC00000` (`NaN`) and, separately, by `0x7F800000` (`+infinity`) are rejected, as are the same substitutions in `aim_y`. The existing `move_x`/`move_y` non-finite cases must still pass.
- The existing truncation and random-byte sweeps in `tests/net/robustness_test.cpp` must be updated for the new sizes wherever they hard-code `17` or the version byte, and must still pass.

Run: `scripts/tw cmake --build build/plain -j8 --target protocol_test && scripts/tw ctest --test-dir build/plain -R protocol_test --output-on-failure`
Expected: FAIL — the designated initializer for `aim_x` does not compile, and once the
struct exists, the golden byte comparison fails at 17 bytes against the expected 25.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: add `float aim_x, aim_y;` to `sim::InputCommand` between `move_y` and `fire`.
Bump `kProtocolVersion` to `2` and `kInputBytes` to `25`. `encodeInput` writes the seven
fields in table order; `decodeInput` keeps its `r.remaining() == kInputBytes` entry guard,
reads the seven fields, and rejects when any of the four floats is not finite — the aim
fields go through the same `std::isfinite` check as the move fields, before `out` is
assigned. The `static_assert` on the maximum snapshot size is unaffected (`24 + 8 + 32*24 = 800 ≤ 1200`).

```bash
scripts/tw cmake --build build/plain -j8 --target "protocol_test|robustness_test" && \
  scripts/tw ctest --test-dir build/plain -R "protocol_test|robustness_test" --output-on-failure && \
  git add src/sim/sim.h src/net/protocol.h src/net/protocol.cpp tests/net/protocol_test.cpp tests/net/robustness_test.cpp && \
  git commit -m "feat: carry an aim vector in InputCommand at protocol version 2"
```

Expected: PASS, then one commit.

**Checkpoint 3: `JoinAccept` carries the assigned player id**

- [ ] **Step 1: Write the failing test, then run it**

Write `tests/net/framing_test.cpp`; add `src/net/framing.cpp` to `libnet` and
`tw_add_test(framing_test tests/net/framing_test.cpp)` to `CMakeLists.txt`.

Spec:
- `encodeJoinAccept(7, w)` into a 4-byte buffer produces **exactly** `07 00 00 00`, `w.size() == kJoinAcceptBytes`, and returns `true`.
- Decoding those 4 bytes returns `true` with `out == 7`.
- Strict framing: 3 bytes are rejected; 5 bytes are rejected; both leave the caller's `uint32_t` (sentinel `0xAAAAAAAA`) untouched.
- `player_id == 0` is rejected on **both** sides: `encodeJoinAccept(0, w)` returns `false` and writes nothing (`w.size() == 0`), and decoding `00 00 00 00` returns `false` with the sentinel untouched — `kInvalidPlayerId` never travels as an accepted id.

Run: `scripts/tw cmake --build build/plain -j8 --target framing_test && scripts/tw ctest --test-dir build/plain -R framing_test --output-on-failure`
Expected: FAIL at the build step — `net::encodeJoinAccept` does not exist.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: `encodeJoinAccept` returns `false` immediately when `player_id == sim::kInvalidPlayerId`,
otherwise writes it as `u32` and returns `w.ok()`. `decodeJoinAccept` returns `false`
unless `r.remaining() == kJoinAcceptBytes` on entry, reads the `u32`, and returns `false`
when `!r.ok()` or the value is `sim::kInvalidPlayerId`; only then assigns `out`.

```bash
scripts/tw cmake --build build/plain -j8 --target framing_test && \
  scripts/tw ctest --test-dir build/plain -R framing_test --output-on-failure && \
  git add CMakeLists.txt src/net/framing.h src/net/framing.cpp src/net/protocol.h src/net/protocol.cpp tests/net/framing_test.cpp && \
  git commit -m "feat: add the JoinAccept payload codec"
```

Expected: PASS, then one commit.

**Checkpoint 4: `framePacket` builds a whole datagram with a correct `payload_len`**

- [ ] **Step 1: Write the failing test, then run it**

Spec — in `tests/net/framing_test.cpp`:
- Framing the header `{type = kInput, tick = 1234, send_time_ms = 123456, ack_tick = 1200, seq = 7, ack_seq = 9}` — with `payload_len` left at its default `0`, to prove the function sets it — around the 25 golden `InputCommand` bytes, into a `kMaxPacket`-sized buffer, returns `49` (`kHeaderBytes + kInputBytes`). The first 24 bytes are the golden header vector **with bytes 6–7 reading `19 00`** (25), and bytes 24..48 are the golden input payload verbatim.
- The result decodes end to end: a `ByteReader` over the first 49 bytes gives `decodeHeader == true` with `payload_len == 25`, and `decodeInput` on the same reader then returns `true` with the original fields.
- An empty payload frames to exactly `kHeaderBytes` (24) with bytes 6–7 reading `00 00`, and `decodeHeader` accepts it — this is the shape `JoinRequest` and `Leave` use.
- Rejection, each returning `0` and writing nothing detectable (assert the destination buffer, pre-filled with `0xEE`, is unchanged): a payload of `kMaxPacket - kHeaderBytes + 1` bytes; a destination buffer of 23 bytes with an empty payload; a destination buffer of 48 bytes with the 25-byte payload.

Run: `scripts/tw cmake --build build/plain -j8 --target framing_test && scripts/tw ctest --test-dir build/plain -R framing_test --output-on-failure`
Expected: FAIL at the build step — `net::framePacket` does not exist.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: `framePacket` returns `0` when `payload.size() > kMaxPacket - kHeaderBytes` or
when `out.size() < kHeaderBytes + payload.size()`. Otherwise it sets
`h.payload_len = static_cast<uint16_t>(payload.size())` (`h` is taken **by value** so the
caller's header is not mutated), constructs a `ByteWriter` over `out`, calls
`encodeHeader`, then `w.bytes(payload)`, and returns `w.size()` when `w.ok()` and `0`
otherwise. The size guards come before any write, so a rejected frame leaves `out`
untouched — that is what the pre-filled-buffer assertions pin.

```bash
scripts/tw cmake --build build/plain -j8 --target framing_test && \
  scripts/tw ctest --test-dir build/plain -R framing_test --output-on-failure && \
  git add src/net/framing.h src/net/framing.cpp tests/net/framing_test.cpp && \
  git commit -m "feat: frame a header and payload into one datagram buffer"
```

Expected: PASS, then one commit.

**Task boundary:**
```bash
scripts/tw ctest --test-dir build/plain --output-on-failure && \
scripts/tw cmake --build build/asan -j8 && \
scripts/tw ctest --test-dir build/asan --output-on-failure
```

---

## Task 4: `PacketRing` — the I/O↔simulation seam

The design doc requires this seam to exist at P2 even while single-threaded, "so P5 swaps
an implementation rather than restructuring — and yields a real before/after measurement
instead of an asserted one." The API is resolution doc § Q4's verbatim: `acquireWrite` /
`commitWrite` / `acquireRead` / `commitRead`, monotonic `uint64_t` indices, power-of-two
capacity, masking at access time.

**This is not `SpscRing` and must not be described as one.** It is single-threaded, holds
no atomics, and pairs with no memory ordering. P5 introduces the lock-free version behind
the same four calls and benchmarks one against the other.

**Files:**
- Create: `src/server/packet_ring.h`, `tests/server/packet_ring_test.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Produces, in `namespace server`:

```cpp
template <typename T, size_t N>
class PacketRing {
  static_assert((N & (N - 1)) == 0, "N must be a power of two");
 public:
  T* acquireWrite() noexcept;   // slot to fill, or nullptr when full
  void commitWrite() noexcept;  // publishes the slot acquireWrite returned
  T* acquireRead() noexcept;    // oldest unread slot, or nullptr when empty
  void commitRead() noexcept;   // releases the slot acquireRead returned
  size_t size() const noexcept;
  static constexpr size_t capacity() noexcept { return N; }
};
```

**Checkpoint 1: a committed write becomes readable, in order**

- [ ] **Step 1: Write the failing test, then run it**

Write `tests/server/packet_ring_test.cpp`; add
`tw_add_test(packet_ring_test tests/server/packet_ring_test.cpp)` to `CMakeLists.txt`.
Use `PacketRing<uint32_t, 4>` for the ordering cases — the element type is a template
parameter precisely so the tests need not carry 1200-byte slots.

Spec:
- A fresh ring: `size() == 0`, `capacity() == 4`, and `acquireRead()` returns `nullptr`.
- `acquireWrite()` returns a non-null pointer; writing `11` through it and calling `commitWrite()` leaves `size() == 1`.
- **An uncommitted write is not visible:** after `acquireWrite()` alone (no commit), `size()` is unchanged and `acquireRead()` still returns `nullptr`.
- **`acquireWrite` twice without a commit returns the same slot** — the producer holds one slot at a time.
- `acquireRead()` now returns a pointer to the value `11`; `size()` is still `1` until `commitRead()`, after which `size() == 0` and `acquireRead()` returns `nullptr`.
- FIFO: write and commit `11`, `22`, `33`, then three read/commit pairs yield `11`, `22`, `33` in that order.

Run:
```bash
scripts/tw cmake --build build/plain -j8 --target packet_ring_test && \
scripts/tw ctest --test-dir build/plain -R packet_ring_test --output-on-failure
```
Expected: FAIL at the build step — `src/server/packet_ring.h: No such file or directory`.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: two monotonic `uint64_t` members, `write_` and `read_`, never wrapped, plus
`std::array<T, N> buf_`. `acquireWrite` returns `&buf_[write_ & (N - 1)]` unless
`write_ - read_ == N`, in which case `nullptr`. `commitWrite` increments `write_`.
`acquireRead` returns `&buf_[read_ & (N - 1)]` unless `write_ == read_`, in which case
`nullptr`. `commitRead` increments `read_`. `size()` is `write_ - read_`. Header-only, no
allocation, no atomics — and no `#include <atomic>`, so nobody mistakes it for the
lock-free ring.

```bash
scripts/tw cmake --build build/plain -j8 --target packet_ring_test && \
  scripts/tw ctest --test-dir build/plain -R packet_ring_test --output-on-failure && \
  git add CMakeLists.txt src/server/packet_ring.h tests/server/packet_ring_test.cpp && \
  git commit -m "feat: add PacketRing as the I/O-to-simulation seam"
```

Expected: PASS, then one commit.

**Checkpoint 2: the ring is bounded and its indices wrap correctly**

- [ ] **Step 1: Write the failing test, then run it**

Spec — on a `PacketRing<uint32_t, 4>`:
- Four write/commit pairs of `1, 2, 3, 4` all succeed and `size() == 4`; a fifth `acquireWrite()` returns `nullptr`, and `size()` stays `4` — full means full, and no queued value is overwritten.
- Draining yields `1, 2, 3, 4` in order.
- **Wrap:** after that drain, run 100 further write/commit/read/commit cycles with the values `100..199`, asserting each read yields the value just written. This crosses the mask boundary 25 times; an implementation that wraps its indices instead of masking at access time breaks here.
- Interleaved: fill to 4, drain 2, write 2 more (both succeed, `size() == 4`), then drain all 4 and assert the order is `3, 4, 5, 6`.

Run: `scripts/tw cmake --build build/plain -j8 --target packet_ring_test && scripts/tw ctest --test-dir build/plain -R packet_ring_test --output-on-failure`
Expected: FAIL — a Checkpoint 1 implementation that omits the full check (or compares
masked indices) accepts the fifth write and the drain yields the wrong first value.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: as Checkpoint 1's, with the `write_ - read_ == N` full check present. The reason
indices are monotonic rather than wrapped is that full and empty are then unambiguous
without sacrificing a slot — the same reasoning resolution doc § Q4 gives for `SpscRing`,
so P5's swap changes the synchronization and nothing about how callers reason.

```bash
scripts/tw cmake --build build/plain -j8 --target packet_ring_test && \
  scripts/tw ctest --test-dir build/plain -R packet_ring_test --output-on-failure && \
  git add src/server/packet_ring.h tests/server/packet_ring_test.cpp && \
  git commit -m "feat: bound PacketRing and mask indices at access time"
```

Expected: PASS, then one commit.

**Task boundary:**
```bash
scripts/tw ctest --test-dir build/plain --output-on-failure && \
scripts/tw cmake --build build/asan -j8 && \
scripts/tw ctest --test-dir build/asan --output-on-failure
```

---

## Task 5: `SessionTable` — binding an endpoint to a player

The P1 security review recorded, as a P2 requirement: *"no binding between a decoded
`InputCommand::player_id` and the UDP source `Endpoint` it arrived from... any attacker who
can reach the port can forge an `InputCommand` claiming to be any `player_id`."* This
class is where that binding lives, and Task 6 Checkpoint 2 is where it is proven.

**Files:**
- Create: `src/server/session.h`, `src/server/session.cpp`, `tests/server/session_test.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `net::Endpoint`, `sim::kMaxPlayers`, `sim::kInvalidPlayerId`.
- Produces, in `namespace server`:

```cpp
inline constexpr uint32_t kSessionTimeoutTicks = 300;  // 5 s at 60 Hz
inline constexpr uint32_t kFireCooldownTicks   = 12;   // 5 shots per second
inline constexpr size_t   kMaxExpired          = sim::kMaxPlayers;

class SessionTable {
 public:
  // Returns the endpoint's player id, assigning one on first sight. Returns
  // kInvalidPlayerId only when the table is full and the endpoint is new.
  uint32_t joinOrGet(const net::Endpoint& from, uint32_t now_tick);
  uint32_t playerFor(const net::Endpoint& from) const noexcept;  // 0 if none
  bool endpointFor(uint32_t player_id, net::Endpoint& out) const noexcept;
  // True only when `from` holds a live session whose assigned id is player_id.
  bool authorize(const net::Endpoint& from, uint32_t player_id) const noexcept;
  void touch(const net::Endpoint& from, uint32_t now_tick, uint32_t input_tick) noexcept;
  uint32_t lastInputTick(uint32_t player_id) const noexcept;
  bool tryFire(uint32_t player_id, uint32_t now_tick) noexcept;  // false while cooling down
  bool remove(const net::Endpoint& from) noexcept;
  // Removes every session silent for >= kSessionTimeoutTicks; writes the removed
  // ids into `out` and returns how many. `out` must hold kMaxExpired entries.
  size_t expire(uint32_t now_tick, std::span<uint32_t> out) noexcept;
  uint32_t count() const noexcept;
  // Iteration for broadcast: the i-th live session, i < count().
  net::Endpoint endpointAt(size_t i) const noexcept;
  uint32_t playerAt(size_t i) const noexcept;
};
```

**Checkpoint 1: an endpoint gets one id, keeps it, and the table is bounded**

- [ ] **Step 1: Write the failing test, then run it**

Write `tests/server/session_test.cpp`; add `src/server/session.cpp` to a new `libserver`
target and `tw_add_test(session_test tests/server/session_test.cpp)` to `CMakeLists.txt`.
`libserver` is `STATIC`, links `libnet` `PUBLIC`, and `tw_add_test` links it into every
test executable.

Spec — endpoints in this test are `net::Endpoint{0x7F000001, static_cast<uint16_t>(0x1F90 + i)}`:
- A fresh table: `count() == 0`, `playerFor(ep0) == 0`, `authorize(ep0, 1) == false`.
- `joinOrGet(ep0, 0)` returns a nonzero id; `count() == 1`; `playerFor(ep0)` returns that same id.
- `joinOrGet(ep0, 5)` again returns **the same id** and `count()` stays `1` — a retransmitted join is idempotent, which is what makes the handshake safe to repeat.
- `joinOrGet(ep1, 0)` returns a **different** nonzero id; `count() == 2`.
- Ids are `1`-based and unique: joining 32 distinct endpoints yields 32 distinct ids, each in `[1, 32]`, and `count() == 32`.
- A 33rd distinct endpoint gets `kInvalidPlayerId` (`0`) and `count()` stays `32`.
- `endpointFor(id, out)` returns `true` and yields the endpoint that owns `id`; `endpointFor(99, out)` returns `false` and leaves `out` untouched.
- `remove(ep0)` returns `true`, `count() == 31`, `playerFor(ep0) == 0`; `remove(ep0)` again returns `false`. A new endpoint may then join, and gets the freed id.
- Iteration covers exactly the live sessions: collecting `playerAt(i)` for `i` in `[0, count())` after the removal yields 31 distinct ids, none of them the removed one, and `endpointAt(i)` agrees with `endpointFor` for each.

Run:
```bash
scripts/tw cmake --build build/plain -j8 --target session_test && \
scripts/tw ctest --test-dir build/plain -R session_test --output-on-failure
```
Expected: FAIL at the build step — `src/server/session.h: No such file or directory`.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: a fixed `std::array<Entry, sim::kMaxPlayers>` where `Entry` holds
`{net::Endpoint peer; uint32_t player_id; uint32_t last_seen_tick; uint32_t last_input_tick; uint32_t last_fire_tick; bool live;}`.
No map, no allocation — 32 entries is a linear scan and this is not a hot path
(one lookup per received packet). Player ids are slot index + 1, so they are unique by
construction and a freed slot's id is reusable. Live sessions are kept **compacted at the
front** so `endpointAt(i)`/`playerAt(i)` for `i < count()` are exactly the live ones;
`remove` swaps the last live entry into the hole.

```bash
scripts/tw cmake --build build/plain -j8 --target session_test && \
  scripts/tw ctest --test-dir build/plain -R session_test --output-on-failure && \
  git add CMakeLists.txt src/server/session.h src/server/session.cpp tests/server/session_test.cpp && \
  git commit -m "feat: add SessionTable binding endpoints to player ids"
```

Expected: PASS, then one commit.

**Checkpoint 2: authorization rejects a mismatched or unknown endpoint**

- [ ] **Step 1: Write the failing test, then run it**

Spec — join `ep0` (id `a`) and `ep1` (id `b`, `b != a`):
- `authorize(ep0, a) == true`.
- `authorize(ep0, b) == false` — **this is the spoof case**: a packet arriving from `ep0` claiming to be `ep1`'s player.
- `authorize(ep1, a) == false`, the mirror.
- `authorize(ep2, a) == false` — an endpoint with no session at all.
- `authorize(ep0, 0) == false` and `authorize(ep0, 99) == false`.
- After `remove(ep0)`, `authorize(ep0, a) == false`.
- A *different port* is a different endpoint: `authorize(net::Endpoint{ep0.addr_be, static_cast<uint16_t>(ep0.port_be + 1)}, a) == false`.

Run: `scripts/tw cmake --build build/plain -j8 --target session_test && scripts/tw ctest --test-dir build/plain -R session_test --output-on-failure`
Expected: FAIL at the build step — `authorize` is not declared.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: `authorize(from, player_id)` returns `player_id != sim::kInvalidPlayerId && playerFor(from) == player_id`.
The comparison is on the whole `Endpoint` (address **and** port), using its
`operator==`. This is the single chokepoint the server routes every input through;
there is no second path that applies an input without it.

```bash
scripts/tw cmake --build build/plain -j8 --target session_test && \
  scripts/tw ctest --test-dir build/plain -R session_test --output-on-failure && \
  git add src/server/session.h src/server/session.cpp tests/server/session_test.cpp && \
  git commit -m "feat: authorize inputs against the endpoint that owns the player id"
```

Expected: PASS, then one commit.

**Checkpoint 3: silent sessions expire, and firing is rate-limited**

- [ ] **Step 1: Write the failing test, then run it**

Spec — expiry, with a `std::array<uint32_t, kMaxExpired>` output buffer:
- Join `ep0` at tick `0` and `ep1` at tick `100`. At tick `100`, `expire(100, out)` returns `0` and `count() == 2`.
- `touch(ep0, 250, 7)` then `expire(300, out)` returns `0` — `ep0` was seen 50 ticks ago and `ep1` 200 ticks ago, both under the 300-tick timeout.
- `expire(400, out)` returns `1` with `out[0]` equal to `ep1`'s id (silent since tick 100); `count() == 1`; `playerFor(ep1) == 0`; `playerFor(ep0)` is unchanged.
- `expire(600, out)` returns `1` with `ep0`'s id; `count() == 0`.
- Boundary: with only `ep0` joined at tick `0`, `expire(299, out)` returns `0` and `expire(300, out)` returns `1` — the timeout is inclusive at exactly `kSessionTimeoutTicks`.
- `touch(ep0, 10, 42)` sets `lastInputTick(a) == 42`; a `touch` with an **older** `input_tick` (`touch(ep0, 11, 40)`) leaves `lastInputTick(a) == 42` — reordered UDP must not walk the ack backwards. `lastInputTick(99) == 0`.
- `touch` on an endpoint with no session is a no-op and must not crash.

Spec — fire cooldown, on a freshly joined `ep0` (id `a`):
- `tryFire(a, 0) == true`; `tryFire(a, 1) == false`; `tryFire(a, 11) == false`; `tryFire(a, 12) == true` — exactly `kFireCooldownTicks` apart.
- `tryFire(0, 100) == false` and `tryFire(99, 100) == false` — unknown ids never fire.
- Cooldown is per player: after `tryFire(a, 0)`, a second session's `tryFire(b, 0)` returns `true`.

Run: `scripts/tw cmake --build build/plain -j8 --target session_test && scripts/tw ctest --test-dir build/plain -R session_test --output-on-failure`
Expected: FAIL at the build step — `expire`, `touch`, `lastInputTick` and `tryFire` are not declared.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: `touch(from, now_tick, input_tick)` sets `last_seen_tick = now_tick` for the
matching session and raises `last_input_tick` only when `input_tick > last_input_tick`.
`expire(now_tick, out)` removes every live session with
`now_tick - last_seen_tick >= kSessionTimeoutTicks`, appending each removed id to `out`
(capped at `out.size()`) and returning the count. Subtraction is done as
`now_tick >= last_seen_tick ? now_tick - last_seen_tick : 0` so a caller passing a smaller
tick can never produce a huge unsigned difference and mass-evict. `tryFire(id, now_tick)`
returns `false` for an unknown or invalid id, otherwise returns `true` and records
`last_fire_tick = now_tick` when `now_tick - last_fire_tick >= kFireCooldownTicks` or the
player has never fired; a first shot at tick `0` is allowed.

```bash
scripts/tw cmake --build build/plain -j8 --target session_test && \
  scripts/tw ctest --test-dir build/plain -R session_test --output-on-failure && \
  git add src/server/session.h src/server/session.cpp tests/server/session_test.cpp && \
  git commit -m "feat: expire silent sessions and rate-limit firing"
```

Expected: PASS, then one commit.

**Task boundary:**
```bash
scripts/tw ctest --test-dir build/plain --output-on-failure && \
scripts/tw cmake --build build/asan -j8 && \
scripts/tw ctest --test-dir build/asan --output-on-failure
```

---

## Task 6: `Server<T>` — the authoritative loop

The phase's centre. A template over P1's `Transport` concept, so every test here runs over
`LoopbackTransport` with no kernel, no ports, and no timing — and the same code runs over
`UdpTransport` in Task 7. It reads no clock: `tick(now_ms)` takes the millisecond value it
stamps into outgoing headers.

**Files:**
- Create: `src/server/server.h`, `tests/server/server_test.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `net::Transport`, `net::PacketSlot`, `net::Endpoint`, `net::framePacket`, all four codecs, `server::SessionTable`, `server::PacketRing`, `sim::World`.
- Produces, in `namespace server`:

```cpp
inline constexpr uint32_t kSnapshotIntervalTicks = 3;    // 60 Hz sim -> 20 Hz snapshots
inline constexpr size_t   kIngestCapacity        = 256;  // power of two, per PacketRing

template <net::Transport T>
class Server {
 public:
  explicit Server(T& transport) noexcept;
  size_t ingest();                 // drains the transport into the ring; returns packets accepted
  void tick(uint32_t now_ms);      // drains the ring, steps the world, broadcasts on schedule
  size_t queuedPackets() const noexcept;
  uint32_t worldTick() const noexcept;
  const sim::World& world() const noexcept;
  uint32_t playerFor(const net::Endpoint&) const noexcept;
  uint64_t hits(uint32_t player_id) const noexcept;      // shots by this player that connected
  uint64_t droppedPackets() const noexcept;              // malformed, unauthorized, or unroutable
  uint64_t ingestOverflows() const noexcept;             // packets dropped because the ring was full
};
```

**Spawn positions are server policy, not simulation.** Player id `i` (1-based) spawns at
`x = -35.0f + 10.0f * static_cast<float>((i - 1) % 8)`,
`y = -35.0f + 10.0f * static_cast<float>((i - 1) / 8)` — an 8×4 grid inside the arena, no
trigonometry, fully deterministic.

**Checkpoint 1: a join request is accepted, bound, and answered**

- [ ] **Step 1: Write the failing test, then run it**

Write `tests/server/server_test.cpp`; add `tw_add_test(server_test tests/server/server_test.cpp)`.
Every transport and the `Server` itself go through `std::make_unique`. The test's fixture
builds `client_tp` and `server_tp` as connected `LoopbackTransport`s, and a helper
`sendTo(transport, to, MsgType, payload_span, seq)` frames and sends one packet.

Spec:
- A fresh `Server`: `queuedPackets() == 0`, `worldTick() == 0`, `world().playerCount() == 0`, `droppedPackets() == 0`.
- The client sends `kJoinRequest` with an empty payload and `seq = 1`. `server->ingest()` returns `1` and `queuedPackets() == 1` — the packet is in the seam, not yet processed. `world().playerCount()` is still `0`.
- `server->tick(1000)`: `queuedPackets() == 0`, `world().playerCount() == 1`, `worldTick() == 1`, and `playerFor(client endpoint)` returns `1`.
- The client receives exactly one packet: header `type == kJoinAccept`, `version == 2`, `ack_seq == 1`, `payload_len == 4`, `send_time_ms == 1000`, `tick == 1`; `decodeJoinAccept` yields `1`.
- The joined player is in the snapshot at its spawn point: a `WorldSnapshot` written from `world()` has `count == 1`, the entry's `id == 1`, `x == -35.0f`, `y == -35.0f`.
- **Idempotent re-join:** the client sends `kJoinRequest` with `seq = 1` again; after `ingest()`/`tick(1016)`, `world().playerCount()` is still `1`, the assigned id is still `1`, and the client receives another `kJoinAccept` carrying `1` — a retransmitted request is re-answered, never re-allocated. The player's position is **not** reset to spawn.
- **Full table:** with 32 distinct client endpoints joined (build them by sending from 32 loopback transports, or by exercising `SessionTable` directly through a second server fixture — the test may use a helper that joins N endpoints), a 33rd endpoint's `kJoinRequest` produces a `kLeave` reply with `ack_seq` echoing the request's `seq` and `payload_len == 0`, and `world().playerCount()` stays `32`.

> `LoopbackTransport` is point-to-point (P1, deliberately), so the 32-endpoint case cannot
> be driven through one loopback pair. Drive it with 32 loopback pairs — one client
> transport per endpoint — all sending into servers sharing one `SessionTable` is *not*
> possible either, so instead: for this case only, construct the `Server` over a small
> test-local transport stub that satisfies the `Transport` concept and records
> `(endpoint, bytes)` sends into a fixed array, letting the test inject packets from
> arbitrary source endpoints. That stub is ~40 lines, lives in `server_test.cpp`, and is
> reused by Checkpoints 2–4. Build it in this checkpoint.

Run:
```bash
scripts/tw cmake --build build/plain -j8 --target server_test && \
scripts/tw ctest --test-dir build/plain -R server_test --output-on-failure
```
Expected: FAIL at the build step — `src/server/server.h: No such file or directory`.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: `Server` holds `T& transport_`, a `sim::World`, a `SessionTable`, a
`PacketRing<net::PacketSlot, kIngestCapacity>`, a scratch send buffer, and the counters.

`ingest()` loops `transport_.tryReceive(*slot)` into slots obtained from `acquireWrite()`,
committing each; when `acquireWrite()` returns `nullptr` it stops, counting one
`ingestOverflows()` — the ring is the backpressure point and the loop is bounded by
`kIngestCapacity` per call, so a flood cannot make one `ingest()` run unboundedly.

`tick(now_ms)` drains the ring completely, routing each packet: decode the header
(failure → `++dropped_`, continue), then dispatch on `type`. `kJoinRequest`
(`payload_len == 0` required) → `joinOrGet`; on a new id, `world_.addPlayer(id, spawn_x, spawn_y)`
and reply `kJoinAccept`; on a full table reply `kLeave`. Every reply echoes the request's
`seq` in `ack_seq`, stamps `tick = world_.tick()` and `send_time_ms = now_ms`, and goes
back to the packet's own source endpoint — never to a stored one. Types the server does not
accept (`kSnapshot`, `kJoinAccept`, `kInvalid`) → `++dropped_`. After the drain, `world_.step()`.

Sends go through `framePacket` into the scratch buffer, then `transport_.send`. A `send`
returning `false` is not an error — UDP may drop, and the loopback inbox may be full;
count it in `dropped_` and continue.

```bash
scripts/tw cmake --build build/plain -j8 --target server_test && \
  scripts/tw ctest --test-dir build/plain -R server_test --output-on-failure && \
  git add CMakeLists.txt src/server/server.h tests/server/server_test.cpp && \
  git commit -m "feat: accept join requests and bind players to endpoints"
```

Expected: PASS, then one commit.

**Checkpoint 2: inputs move only the player the sender owns**

This is the checkpoint that discharges the P1 security finding. It must fail before the
authorization call exists.

- [ ] **Step 1: Write the failing test, then run it**

Spec — using the recording transport stub, join `epA` (id 1) and `epB` (id 2):
- An `kInput` from `epA` with `player_id = 1`, `move_x = 1.0f`, `move_y = 0.0f`, `aim = {0,0}`, `fire = false`, then `tick(now)`: player 1's `vx == sim::kMoveSpeed` and its `x` has advanced by exactly `kMoveSpeed * kTickDt` from spawn. Player 2 has not moved.
- **Spoof:** an `kInput` from `epA` with `player_id = 2` and `move_x = 1.0f`. After `tick(now)`, **player 2's velocity is still zero** and its position is unchanged; `droppedPackets()` has increased by one. Player 1 keeps its previously latched velocity.
- An `kInput` from an endpoint with no session at all (`epC`), claiming `player_id = 1`: player 1's velocity is unchanged by it, and `droppedPackets()` increases.
- An `kInput` whose payload is malformed (23 bytes, so `payload_len` disagrees with the frame; and a well-framed 25-byte payload whose `move_x` bytes are `0x7FC00000`/NaN) is dropped: `droppedPackets()` increases, no state changes, no crash.
- A packet whose header magic is wrong, and a 3-byte packet, are both dropped without touching state.
- **`ack_tick` reflects the input:** after an accepted `kInput` with `tick = 77` from `epA`, the next `kSnapshot` sent to `epA` carries `ack_tick == 77`. A subsequent accepted input with `tick = 70` (a reordered datagram) leaves `ack_tick` at `77`.
- **Firing:** an accepted `kInput` with `fire = true`, `aim = {1, 0}` from `epA`, with player 2 positioned on the `+x` ray from player 1's spawn — place it by sending player 2 a movement input first, or by asserting the miss case instead — increments `hits(1)` by exactly `1` when it connects and leaves it unchanged when it misses. Two `fire = true` inputs in consecutive ticks increment `hits(1)` at most once, because of `kFireCooldownTicks`.

Run: `scripts/tw cmake --build build/plain -j8 --target server_test && scripts/tw ctest --test-dir build/plain -R server_test --output-on-failure`
Expected: FAIL on the spoof case — Checkpoint 1's router has no authorization step, so an
`kInput` naming any `player_id` is applied and player 2 moves.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: on `kInput`, decode the payload (failure → `++dropped_`), then require
`sessions_.authorize(packet source endpoint, in.player_id)` — on failure `++dropped_` and
**return without touching the world**. On success: `sessions_.touch(from, world_.tick(), in.tick)`,
`world_.applyInput(in)`, and when `in.fire` and `sessions_.tryFire(in.player_id, world_.tick())`,
call `world_.resolveHitscan(in.player_id, in.aim_x, in.aim_y)` and increment that shooter's
hit counter when it returns a value. Hit counts live in the server (a
`std::array<uint64_t, sim::kMaxPlayers>`), not in `libsim` and not on the wire — P6 is what
gives hits a client-visible representation, and the `MsgType` enum has room for it without
touching the header.

```bash
scripts/tw cmake --build build/plain -j8 --target server_test && \
  scripts/tw ctest --test-dir build/plain -R server_test --output-on-failure && \
  git add src/server/server.h tests/server/server_test.cpp && \
  git commit -m "feat: apply inputs only from the endpoint that owns the player id"
```

Expected: PASS, then one commit.

**Checkpoint 3: snapshots go out at 20 Hz, to every live session**

- [ ] **Step 1: Write the failing test, then run it**

Spec — join `epA` and `epB`, then clear the stub's recorded sends:
- Ticks where `worldTick() % kSnapshotIntervalTicks == 0` send exactly one `kSnapshot` to **each** live endpoint; the other ticks send none. Concretely: run 12 consecutive `tick(now_ms)` calls with `now_ms` advancing by 16 each time, and assert the stub recorded exactly `2 * (12 / 3) == 8` snapshot packets, none on the two ticks after each snapshot tick.
- Each snapshot's header: `type == kSnapshot`, `version == 2`, `tick == worldTick()` at send time, `send_time_ms` equal to the `now_ms` passed to that `tick()` call, `payload_len == 8 + count * 24`, `seq == 0`.
- The payload decodes with `decodeSnapshot` and reports `count == 2` with both players' ids and current positions matching `world()`.
- A snapshot to `epA` carries `ack_tick` equal to `epA`'s last input tick, and the one to `epB` carries `epB`'s — the field is per-recipient, not global.
- With zero sessions, no snapshot is sent at all, and `tick()` still advances `worldTick()`.

Run: `scripts/tw cmake --build build/plain -j8 --target server_test && scripts/tw ctest --test-dir build/plain -R server_test --output-on-failure`
Expected: FAIL — Checkpoint 1's `tick()` sends only handshake replies, so the stub records
zero snapshots.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: at the end of `tick(now_ms)`, after `world_.step()`, when
`world_.tick() % kSnapshotIntervalTicks == 0`: write one `sim::WorldSnapshot` (a member, not
a local — it is ~800 bytes and this runs every third tick), encode it once per recipient
through `framePacket` with that recipient's `ack_tick`, and send to
`sessions_.endpointAt(i)` for `i` in `[0, sessions_.count())`. The payload bytes are
identical for every recipient; only the header differs, which is why the snapshot is
encoded into a scratch payload buffer once and framed per recipient.

The 60/20 Hz mismatch is deliberate and load-bearing: it is what creates the need for P4's
entity interpolation. Do not "fix" it by sending every tick.

```bash
scripts/tw cmake --build build/plain -j8 --target server_test && \
  scripts/tw ctest --test-dir build/plain -R server_test --output-on-failure && \
  git add src/server/server.h tests/server/server_test.cpp && \
  git commit -m "feat: broadcast world snapshots at 20 Hz to every session"
```

Expected: PASS, then one commit.

**Checkpoint 4: leaving and timing out both remove the player**

- [ ] **Step 1: Write the failing test, then run it**

Spec:
- Join `epA` and `epB` (`world().playerCount() == 2`). A `kLeave` from `epA` with `payload_len == 0`, then `tick(now)`: `world().playerCount() == 1`, `playerFor(epA) == 0`, and the remaining player is `epB`'s. No reply is sent to `epA`.
- A `kLeave` from an endpoint with no session is dropped: `droppedPackets()` increases, `world().playerCount()` is unchanged.
- **Timeout:** join `epA` and `epB`; keep sending a valid `kInput` from `epB` on every tick while `epA` stays silent. After `kSessionTimeoutTicks` ticks, `world().playerCount() == 1` and `playerFor(epA) == 0`, while `epB` is untouched and still receiving snapshots.
- A rejoin after a timeout works: `epA` sends `kJoinRequest` again and gets a `kJoinAccept`; `world().playerCount() == 2`. The reassigned id may differ, and the test must not assume it does not.
- The freed id is reusable: after `epA` times out, the next endpoint to join receives `epA`'s old id.

Run: `scripts/tw cmake --build build/plain -j8 --target server_test && scripts/tw ctest --test-dir build/plain -R server_test --output-on-failure`
Expected: FAIL — `kLeave` currently falls into the unhandled-type branch and only increments
`droppedPackets()`; the world keeps both players, and nothing expires.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: on `kLeave` with `payload_len == 0` from an endpoint holding a session, look up
the id, `sessions_.remove(from)` and `world_.removePlayer(id)`. From an unbound endpoint,
`++dropped_`. At the end of every `tick()`, after the broadcast, call
`sessions_.expire(world_.tick(), expired_span)` and `world_.removePlayer` each returned id
— the session table and the world are removed from together, in one place, so they cannot
drift.

```bash
scripts/tw cmake --build build/plain -j8 --target server_test && \
  scripts/tw ctest --test-dir build/plain -R server_test --output-on-failure && \
  scripts/tw cmake --build build/asan -j8 --target server_test && \
  scripts/tw ctest --test-dir build/asan -R server_test --output-on-failure && \
  git add src/server/server.h tests/server/server_test.cpp && \
  git commit -m "feat: remove players on leave and on session timeout"
```

Expected: PASS under both configurations, then one commit.

**Task boundary:**
```bash
scripts/tw ctest --test-dir build/plain --output-on-failure && \
scripts/tw cmake --build build/asan -j8 && \
scripts/tw ctest --test-dir build/asan --output-on-failure
```

---

## Task 7: The tick loop and the server executable

Design doc § Architecture point 4: "epoll multiplexes the UDP socket against a timerfd for
the tick." This task builds those two wrappers and the executable that puts them, the
`UdpTransport`, and `Server` together.

**Files:**
- Create: `src/server/clock.h`, `src/server/clock.cpp`, `src/server/tick_timer.h`, `src/server/tick_timer.cpp`, `src/server/poll_set.h`, `src/server/poll_set.cpp`, `tests/server/timer_test.cpp`, `apps/tw_server.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Produces, in `namespace server`:

```cpp
uint32_t monotonicMs() noexcept;   // CLOCK_MONOTONIC, wrapped to 32 bits

class TickTimer {                  // RAII timerfd
 public:
  explicit TickTimer(uint32_t hz);   // arms a periodic timer; valid() is false on failure
  ~TickTimer();
  TickTimer(const TickTimer&) = delete;
  TickTimer& operator=(const TickTimer&) = delete;
  TickTimer(TickTimer&&) noexcept;
  TickTimer& operator=(TickTimer&&) noexcept;
  bool valid() const noexcept;
  int fd() const noexcept;             // -1 when invalid
  uint64_t consumeExpirations() noexcept;  // 0 when nothing has fired
};

class PollSet {                    // RAII epoll fd
 public:
  PollSet();
  ~PollSet();
  PollSet(const PollSet&) = delete;
  PollSet& operator=(const PollSet&) = delete;
  PollSet(PollSet&&) noexcept;
  PollSet& operator=(PollSet&&) noexcept;
  bool valid() const noexcept;
  bool add(int fd, uint32_t token);          // level-triggered read readiness
  // Waits up to timeout_ms; writes ready tokens into `out`, returns how many.
  size_t wait(int timeout_ms, std::span<uint32_t> out);
};
```

**Checkpoint 1: the timer fires at its configured rate and closes itself**

- [ ] **Step 1: Write the failing test, then run it**

Write `tests/server/timer_test.cpp`; add the three new `.cpp` files to `libserver` and
`tw_add_test(timer_test tests/server/timer_test.cpp)`.

Spec:
- `TickTimer t(1000)` (1 kHz, 1 ms period): `valid() == true` and `fd() >= 0`.
- Immediately after construction, `consumeExpirations()` may legitimately return `0`; the test must not assert otherwise.
- Blocking on the fd via a `PollSet`-free `poll()`-equivalent is out of scope here — instead, loop calling `consumeExpirations()` at most 5000 times, summing results, and stop as soon as the sum reaches `3`. Assert the sum reaches at least `3`. A 1 ms timer reaches 3 expirations in ~3 ms; the iteration cap is what keeps a broken timer from hanging the suite, and failing the assertion after 5000 empty reads is the correct failure, not a timeout.
- `consumeExpirations()` immediately after a nonzero read returns `0` — expirations are consumed, not re-reported.
- **Moving transfers ownership:** move-construct `u` from `t`; `u.valid() && u.fd() >= 0`, and `t.valid() == false` with `t.fd() == -1`. `consumeExpirations()` on the moved-from object returns `0` and must not touch fd `-1`.
- **RAII:** record `fd()` inside a scope, let the timer destruct, then assert `fcntl(recorded_fd, F_GETFD) == -1` with `errno == EBADF` — the fd is closed, not leaked. (This is the same fd-lifetime assertion P1's `udp_test` makes.)
- `TickTimer t0(0)` — an invalid rate — leaves `valid() == false`, `fd() == -1`, and does not crash.

Run:
```bash
scripts/tw cmake --build build/plain -j8 --target timer_test && \
scripts/tw ctest --test-dir build/plain -R timer_test --output-on-failure
```
Expected: FAIL at the build step — `src/server/tick_timer.h: No such file or directory`.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: `TickTimer(hz)` returns early leaving `fd_ == -1` when `hz == 0`; otherwise
`timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC)` then `timerfd_settime` with
both the initial expiry and the interval set to `1'000'000'000 / hz` nanoseconds. Any
failing syscall closes the fd and leaves `fd_ == -1`. `consumeExpirations` does a
non-blocking `read` of a `uint64_t`: `EAGAIN` → `0`, `EINTR` → retry, short/failed read →
`0`. The destructor closes a non-negative fd; the move operations transfer it and leave the
source at `-1`. `monotonicMs()` is `clock_gettime(CLOCK_MONOTONIC)` reduced to
`uint32_t` milliseconds — it wraps every ~49 days, which is fine because every consumer
uses differences, and P3's RTT estimation will subtract two of them.

```bash
scripts/tw cmake --build build/plain -j8 --target timer_test && \
  scripts/tw ctest --test-dir build/plain -R timer_test --output-on-failure && \
  git add CMakeLists.txt src/server/clock.h src/server/clock.cpp src/server/tick_timer.h src/server/tick_timer.cpp tests/server/timer_test.cpp && \
  git commit -m "feat: add a monotonic clock and an RAII timerfd tick timer"
```

Expected: PASS, then one commit.

**Checkpoint 2: epoll reports which of the socket and the timer is ready**

- [ ] **Step 1: Write the failing test, then run it**

Spec — build a `PollSet`, a `TickTimer(1000)` registered with token `1`, and two bound
`UdpTransport`s (`std::make_unique`, `127.0.0.1`, ephemeral ports) with the receiver's
`nativeHandle()` registered under token `2`:
- `valid() == true` for the `PollSet`; `add` returns `true` for both fds and `false` for `-1`.
- With nothing sent and the timer not yet fired, `wait(0, out)` returns `0` — a zero timeout with nothing pending reports nothing.
- After sending a 5-byte datagram from the other transport, `wait(100, out)` returns at least `1` and `out` contains token `2` among the returned tokens.
- After sleeping past a timer period (call `wait(50, out)` in a loop of at most 20 iterations until token `1` appears), token `1` is reported.
- Draining matters: consume the datagram with `tryReceive` and the expirations with `consumeExpirations`, then `wait(0, out)` returns `0` again.
- `wait` with an `out` span smaller than the number of ready fds returns at most `out.size()` and does not write past it — register both fds, make both ready, and call `wait(50, out)` with a 1-element span.

Run: `scripts/tw cmake --build build/plain -j8 --target timer_test && scripts/tw ctest --test-dir build/plain -R timer_test --output-on-failure`
Expected: FAIL at the build step — `src/server/poll_set.h: No such file or directory`.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: `PollSet` wraps `epoll_create1(EPOLL_CLOEXEC)`. `add(fd, token)` rejects
`fd < 0` and otherwise registers `EPOLLIN` (level-triggered — the loop must not lose a
readiness edge it did not fully drain) with `epoll_event::data.u32 = token`. `wait` calls
`epoll_wait` into a fixed `std::array<epoll_event, 8>`, retries once on `EINTR`, returns
`0` on error or timeout, and copies at most `out.size()` tokens. Same RAII and move rules
as `TickTimer`.

```bash
scripts/tw cmake --build build/plain -j8 --target timer_test && \
  scripts/tw ctest --test-dir build/plain -R timer_test --output-on-failure && \
  git add src/server/poll_set.h src/server/poll_set.cpp tests/server/timer_test.cpp && \
  git commit -m "feat: multiplex the socket against the tick timer with epoll"
```

Expected: PASS, then one commit.

**Checkpoint 3: `tw_server` runs a bounded number of ticks and reports its port**

- [ ] **Step 1: Write the failing test, then run it**

Write `apps/tw_server.cpp`; add `add_executable(tw_server apps/tw_server.cpp)` linking
`libserver`, and register a CTest case `server_app_runs` that runs
`tw_server --port 0 --ticks 5`.

Spec:
- `tw_server --port 0 --ticks 5` exits `0`, prints a line matching `port=<n>` with `n` nonzero on stdout, and a final line `ticks=5`. Pin it with `set_tests_properties(server_app_runs PROPERTIES PASS_REGULAR_EXPRESSION "ticks=5")`.
- `tw_server --port 0 --ticks 0` is the run-forever form and is **not** exercised by CTest.
- `tw_server --bogus` exits nonzero and prints a usage line to stderr. Register this as a second CTest case with `set_tests_properties(... PROPERTIES WILL_FAIL TRUE)`.

Run:
```bash
scripts/tw cmake --build build/plain -j8 --target tw_server && \
scripts/tw ctest --test-dir build/plain -R server_app --output-on-failure
```
Expected: FAIL at the build step — `apps/tw_server.cpp` does not exist.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: parse `--port <n>` (default `41234`, `0` = ephemeral) and `--ticks <n>`
(default `0` = forever); any unknown argument prints usage to `std::cerr` and returns `1`.
Bind a `UdpTransport` to `INADDR_LOOPBACK`; on failure print the reason and return `1`.
Print `port=<host-order port>` (convert with `ntohs`) and flush, so a driving script can
read it. Build the `Server` with `std::make_unique` — it is far too large for the stack.

The loop: register the socket fd (token `2`) and a `TickTimer(sim::kTickHz)` (token `1`)
in a `PollSet`; call `wait(50, tokens)`; on token `2` call `server->ingest()`; on token `1`
call `consumeExpirations()` and run `server->tick(monotonicMs())` **once per expiration**,
so a late wakeup catches up rather than silently dropping ticks. Stop after `--ticks`
ticks when nonzero, printing `ticks=<n>`. Install a `SIGINT` handler setting a
`volatile std::sig_atomic_t` flag that also ends the loop, printing the same line.

```bash
scripts/tw cmake --build build/plain -j8 --target tw_server && \
  scripts/tw ctest --test-dir build/plain -R server_app --output-on-failure && \
  git add CMakeLists.txt apps/tw_server.cpp && \
  git commit -m "feat: add the tw_server executable with an epoll tick loop"
```

Expected: PASS, then one commit.

**Task boundary:**
```bash
scripts/tw ctest --test-dir build/plain --output-on-failure && \
scripts/tw cmake --build build/asan -j8 && \
scripts/tw ctest --test-dir build/asan --output-on-failure && \
scripts/tw cmake -S . -B build/tsan -DCMAKE_BUILD_TYPE=RelWithDebInfo -DTW_SANITIZER=thread && \
scripts/tw cmake --build build/tsan -j8 && \
scripts/tw setarch -R ctest --test-dir build/tsan --output-on-failure
```

---

## Task 8: `Client<T>`, the load client, and end-to-end over real UDP

The client has **no prediction and no interpolation** — it renders the last snapshot the
server sent, which at 20 Hz plus latency is visibly late. That is the correct P2 behavior
and the "before" half of P3's demo; do not smooth it here.

**Files:**
- Create: `src/client/client.h`, `tests/client/client_test.cpp`, `apps/tw_loadclient.cpp`, `scripts/e2e-udp.sh`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `net::Transport`, `net::framePacket`, the codecs, `server::Server` (tests only).
- Produces, in `namespace client`:

```cpp
inline constexpr uint32_t kJoinRetryTicks  = 15;   // 250 ms at 60 Hz
inline constexpr uint32_t kJoinMaxAttempts = 40;   // ~10 s, then kFailed
inline constexpr uint16_t kJoinSeq  = 1;
inline constexpr uint16_t kLeaveSeq = 2;

enum class State : uint8_t { kIdle, kJoining, kJoined, kRejected, kFailed };

template <net::Transport T>
class Client {
 public:
  Client(T& transport, net::Endpoint server) noexcept;
  void beginJoin(uint32_t now_ms);
  void tick(uint32_t now_ms);   // drains inbound, retransmits the join if still waiting
  bool sendInput(uint32_t now_ms, float move_x, float move_y,
                 float aim_x, float aim_y, bool fire);   // false unless kJoined
  void leave(uint32_t now_ms);
  State state() const noexcept;
  uint32_t playerId() const noexcept;                 // 0 until joined
  const sim::WorldSnapshot& latestSnapshot() const noexcept;
  uint32_t latestSnapshotTick() const noexcept;       // 0 before the first snapshot
  uint32_t serverTimeMs() const noexcept;             // send_time_ms of the newest snapshot
  uint32_t snapshotsReceived() const noexcept;
  uint32_t joinAttempts() const noexcept;
};
```

**Checkpoint 1: the join handshake retransmits until answered, then stops**

- [ ] **Step 1: Write the failing test, then run it**

Write `tests/client/client_test.cpp`; add `tw_add_test(client_test tests/client/client_test.cpp)`.
`libclient` is a new `STATIC` target linking `libnet` `PUBLIC`; `Client` is header-only, so
`libclient`'s only source at this point is `src/client/view.cpp` from Task 9 — until then,
declare `libclient` as an `INTERFACE` target carrying `src/client` on its include path and
switch it to `STATIC` in Task 9. Tests use a recording transport stub like Task 6's (a
second one local to this file, or the Task 6 stub moved to `tests/support/recording_transport.h`
and included by both — **do the move**, and update `server_test.cpp`'s include in the same
commit).

Spec:
- A fresh `Client`: `state() == State::kIdle`, `playerId() == 0`, `latestSnapshotTick() == 0`, `snapshotsReceived() == 0`, `joinAttempts() == 0`.
- `sendInput(...)` while `kIdle` returns `false` and sends nothing.
- `beginJoin(1000)`: `state() == State::kJoining`, `joinAttempts() == 1`, and exactly one packet was sent to the server endpoint — `type == kJoinRequest`, `version == 2`, `payload_len == 0`, `seq == kJoinSeq`, `send_time_ms == 1000`.
- **Retransmit:** 14 further `tick()` calls send nothing more; the 15th sends a second identical `kJoinRequest` (same `seq`), and `joinAttempts() == 2`.
- **Acceptance stops it:** feed the stub a `kJoinAccept` with `ack_seq == kJoinSeq` and payload `player_id = 4`, then `tick()`: `state() == State::kJoined`, `playerId() == 4`, and no further `kJoinRequest` is sent over the next 60 ticks.
- **A mismatched ack is ignored:** from `kJoining`, a `kJoinAccept` with `ack_seq == 9` leaves `state() == kJoining` and `playerId() == 0`.
- **Rejection:** from `kJoining`, a `kLeave` with `ack_seq == kJoinSeq` puts the client in `State::kRejected`, and no further requests are sent.
- **Give up:** from `kJoining` with no reply, after `kJoinMaxAttempts` attempts the state becomes `State::kFailed` and sending stops.
- Junk in is ignored: a 3-byte packet, a wrong-magic packet, and a `kInput` packet each leave the state and counters unchanged.

Run:
```bash
scripts/tw cmake --build build/plain -j8 --target client_test && \
scripts/tw ctest --test-dir build/plain -R client_test --output-on-failure
```
Expected: FAIL at the build step — `src/client/client.h: No such file or directory`.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: `Client` holds the transport reference, the server endpoint, a `State`, the
assigned id, a `sim::WorldSnapshot` member, counters, its own `uint32_t tick_` incremented
by `tick()`, and `next_retry_tick_`. `beginJoin` sends the request, sets `kJoining`,
`joinAttempts() = 1`, and schedules `next_retry_tick_ = tick_ + kJoinRetryTicks`.
`tick(now_ms)` first drains the transport, dispatching by type, then — if still `kJoining`
— retransmits when `tick_ >= next_retry_tick_`, moving to `kFailed` at
`kJoinMaxAttempts`. All outbound packets go through `framePacket` and carry
`send_time_ms = now_ms` and `tick = tick_`.

```bash
scripts/tw cmake --build build/plain -j8 --target "client_test|server_test" && \
  scripts/tw ctest --test-dir build/plain -R "client_test|server_test" --output-on-failure && \
  git add CMakeLists.txt src/client/client.h tests/support/recording_transport.h tests/client/client_test.cpp tests/server/server_test.cpp && \
  git commit -m "feat: add the client join handshake with bounded retransmits"
```

Expected: PASS, then one commit.

**Checkpoint 2: inputs go out and snapshots land, newest-wins**

- [ ] **Step 1: Write the failing test, then run it**

Spec — drive a joined client (`playerId() == 4`):
- `sendInput(2000, 1.0f, 0.0f, 0.0f, 1.0f, true)` returns `true` and sends one `kInput` whose payload decodes to `player_id == 4`, the given move and aim values bit-exactly, and `fire == true`; its header `send_time_ms == 2000` and `payload_len == 25`.
- A `kSnapshot` for tick `30` with two players is fed in; after `tick()`: `snapshotsReceived() == 1`, `latestSnapshotTick() == 30`, `serverTimeMs()` equals that packet's `send_time_ms`, and `latestSnapshot()` reports `count == 2` with both players' fields matching what was encoded.
- **Newest wins, and stale is dropped:** feeding tick `27` after tick `30` leaves `latestSnapshotTick() == 30` and the stored snapshot unchanged, but `snapshotsReceived()` still increments to `2` — the packet was received, just not adopted. Feeding tick `33` adopts it (`latestSnapshotTick() == 33`).
- A `kSnapshot` whose payload is malformed (a `count` of `33`) is ignored entirely: `latestSnapshotTick()` unchanged, no crash.
- `leave(3000)` sends one `kLeave` with `seq == kLeaveSeq` and `payload_len == 0`, and moves the state to `kIdle`; a following `sendInput` returns `false`.

Run: `scripts/tw cmake --build build/plain -j8 --target client_test && scripts/tw ctest --test-dir build/plain -R client_test --output-on-failure`
Expected: FAIL at the build step — `sendInput`, `latestSnapshot` and `leave` are not declared.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: `sendInput` returns `false` unless `state() == State::kJoined`; otherwise it
builds a `sim::InputCommand{playerId(), tick_, move_x, move_y, aim_x, aim_y, fire}`,
encodes it, frames it as `kInput`, and sends. On an inbound `kSnapshot`, decode into a
scratch `sim::WorldSnapshot`; increment `snapshotsReceived()`; adopt it (copy into the
member, record `latestSnapshotTick()` and `serverTimeMs()`) only when the header's `tick`
is **strictly greater** than `latestSnapshotTick()`. A decode failure adopts nothing. There
is deliberately no interpolation and no prediction here — P3 and P4 own those, and the
visible lag is the point at P2.

```bash
scripts/tw cmake --build build/plain -j8 --target client_test && \
  scripts/tw ctest --test-dir build/plain -R client_test --output-on-failure && \
  git add src/client/client.h tests/client/client_test.cpp && \
  git commit -m "feat: send inputs and adopt the newest snapshot on the client"
```

Expected: PASS, then one commit.

**Checkpoint 3: a client and a server converge, in memory and over real UDP**

- [ ] **Step 1: Write the failing test, then run it**

Spec — **in-memory integration**, in `client_test.cpp`, over a connected
`LoopbackTransport` pair (all objects via `std::make_unique`), with an explicit pump helper
that calls `server->ingest()`, `server->tick(ms)`, `client->tick(ms)` in that order,
advancing `ms` by 16 each iteration:
- `client->beginJoin(0)`, then pump 5 iterations: `client->state() == State::kJoined`, `client->playerId() == 1`, and `server->world().playerCount() == 1`.
- Then, on each of 60 further iterations, `client->sendInput(ms, 1.0f, 0.0f, 0.0f, 0.0f, false)`. Afterwards: `client->snapshotsReceived() >= 15` (60 ticks at one snapshot per 3), and the client's `latestSnapshot()` shows its own player with `x` greater than its spawn `x` by at least `50 * sim::kMoveSpeed * sim::kTickDt` — the client sees the server move it.
- `client->leave(ms)` then two more pumps: `server->world().playerCount() == 0`.
- **Through the simulator:** repeat the join with the client's transport wrapped in `net::SimulatedTransport` configured `{latency_ms = 100, jitter_ms = 0, loss_permille = 0, seed = 1}` (calling `advanceTick()` once per pump iteration): the join still completes, and it completes **later** — `client->joinAttempts()` is at least `1` and the iteration index at which `kJoined` is first observed is strictly greater than in the unwrapped case. This is the latency slider proving itself against real server code before any GUI exists.

Spec — **over real UDP**, as a shell-driven CTest case: write `scripts/e2e-udp.sh` taking
`<tw_server path> <tw_loadclient path>`. It starts `tw_server --port 0 --ticks 600` in the
background, reads the `port=<n>` line from its stdout, runs
`tw_loadclient --host 127.0.0.1 --port <n> --players 4 --ticks 300`, and requires the load
client to exit `0` printing `joined=4` and a `snapshots=<n>` line with `n >= 4`. It kills
the server on exit (`trap`) and fails if the port line never appears within 5 seconds.
Register it as `add_test(NAME e2e_udp COMMAND bash ${CMAKE_SOURCE_DIR}/scripts/e2e-udp.sh $<TARGET_FILE:tw_server> $<TARGET_FILE:tw_loadclient>)`,
following `determinism_two_binaries`' existing shape.

`apps/tw_loadclient.cpp`: parse `--host` (default `127.0.0.1`), `--port`, `--players`
(default `1`, max `sim::kMaxPlayers`), `--ticks` (default `600`). Create one
`UdpTransport` and one `Client` **per simulated player**, all heap-allocated; each joins,
then each sends an input every tick with a deterministic per-player movement pattern —
player `i` uses `move_x = ((tick / 30 + i) % 2 == 0) ? 1.0f : -1.0f`, `move_y = 0.0f`, no
firing, which needs no RNG and keeps the run reproducible. Sleep to the next 16 ms boundary
with `nanosleep` between ticks. At the end print `joined=<n>` and `snapshots=<total>` and
return `0` if every client reached `kJoined`, else `1`.

Run:
```bash
scripts/tw cmake --build build/plain -j8 --target "client_test tw_server tw_loadclient" && \
scripts/tw ctest --test-dir build/plain -R "client_test|e2e_udp" --output-on-failure
```
Expected: FAIL at the build step — `apps/tw_loadclient.cpp` does not exist, and the
in-memory integration case does not compile until the pump helper is written.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: as specified above. The in-memory case is the authoritative correctness test —
deterministic, sanitizer-friendly, no ports. The UDP case is the integration proof that the
same code survives a real socket, a real bind, and two processes; it is deliberately
tolerant (`snapshots >= 4`, not an exact count) because real scheduling is not
reproducible, and a strict count would make it flaky. If the container cannot bind
`127.0.0.1` here, that is a finding for `docs/project-history.md`, not something to work
around — P1 already verified loopback works inside this image, so a failure would be new
information.

```bash
scripts/tw cmake --build build/plain -j8 && \
  scripts/tw ctest --test-dir build/plain -R "client_test|e2e_udp" --output-on-failure && \
  scripts/tw cmake --build build/asan -j8 --target client_test && \
  scripts/tw ctest --test-dir build/asan -R client_test --output-on-failure && \
  git add CMakeLists.txt apps/tw_loadclient.cpp scripts/e2e-udp.sh tests/client/client_test.cpp && \
  git commit -m "test: converge a client and server in memory and over real UDP"
```

Expected: PASS under both configurations, then one commit.

**Task boundary — the mandatory security review.** Not a checkpoint; it produces findings,
not a commit, and it gates Task 9. `CLAUDE.md` makes this non-optional for any phase
touching the network surface, and P2 adds the first code that feeds decoded packets into
authoritative state — the exact condition the P1 review said made its deferred finding
harmless.

- [ ] Run the full suite under all three configurations:
```bash
scripts/tw bash scripts/ci.sh
```
- [ ] Invoke the **`security-review` skill** (or the `security-reviewer` agent) over `src/server/`, `src/client/`, `apps/` and `tests/`, with the question stated as: *an unauthenticated attacker controls every byte of every datagram, can send them at any rate, and can forge any source address the network lets through; what can they cause?* Give it these specific surfaces to answer for: session exhaustion (32 slots, no proof of work), the ingest ring as a backpressure point, `expire()`'s tick arithmetic, whether any path applies an input without `authorize`, and whether a reply can ever be sent to an endpoint other than the packet's own source.
- [ ] Also run the **`cpp-reviewer` agent** over the same surface for RAII and lifetime issues — `TickTimer`/`PollSet` fd ownership and `Server`/`Client` holding a `T&` transport reference are where a lifetime bug would hide.
- [ ] Address every **CRITICAL** and **HIGH** finding before starting Task 9, each as a normal cycle: a failing test naming the specific input, the fix, one commit.
- [ ] Record every finding in `docs/project-history.md` under P2, including findings judged not worth fixing **and why** — a dismissed finding with no recorded reason gets re-litigated in P3.

---

## Task 9: The raylib client and the demo

**The one task carrying unresolved environment risk**, which is why it is last. Resolution
doc § Q5 residual risk 2 defers the question to this phase: *"the raylib GUI client is the
open question... Validate it at P2, do not assume it."* Checkpoint 2 is that validation.

Known before starting, from a check run while planning: the pinned image **has** `libX11`
but is **missing** `libGL`, `libXrandr`, `libXi`, `libXcursor`, `libXinerama` and the GL
headers. WSLg is present on the host (`DISPLAY=:0`, `/tmp/.X11-unix/X0` exists).

**Files:**
- Create: `src/client/view.h`, `src/client/view.cpp`, `tests/client/view_test.cpp`, `apps/tw_client.cpp`, `scripts/demo.sh`
- Modify: `Dockerfile`, `scripts/tw`, `CMakeLists.txt`

**Interfaces:**
- Produces, in `namespace client`:

```cpp
struct ScreenPos { float x, y; };

// Maps arena coordinates ([-kArenaHalf, kArenaHalf] on both axes) to a
// square viewport of `side` pixels at origin (ox, oy). +y is up in world
// space and down on screen, so the y axis is flipped.
ScreenPos worldToScreen(float wx, float wy, float side, float ox, float oy) noexcept;
float worldToScreenRadius(float r, float side) noexcept;
// Aim direction from the player's world position to the cursor's world
// position; returns {0, 0} when they coincide or either input is non-finite.
void aimFromCursor(float px, float py, float cx, float cy,
                   float& aim_x, float& aim_y) noexcept;
```

**Checkpoint 1: the world→screen mapping is exact at the corners and centre**

- [ ] **Step 1: Write the failing test, then run it**

Write `tests/client/view_test.cpp`; add `src/client/view.cpp` to `libclient` (switching it
from `INTERFACE` to `STATIC`, per Task 8's note) and
`tw_add_test(view_test tests/client/view_test.cpp)`.

Spec — with `side = 800.0f`, `ox = 0.0f`, `oy = 0.0f`:
- `worldToScreen(0, 0, ...)` returns `{400.0f, 400.0f}` — the arena centre is the viewport centre.
- `worldToScreen(-kArenaHalf, kArenaHalf, ...)` returns `{0.0f, 0.0f}` — world top-left is screen top-left, proving the y flip.
- `worldToScreen(kArenaHalf, -kArenaHalf, ...)` returns `{800.0f, 800.0f}`.
- With `ox = 100.0f, oy = 50.0f`, `worldToScreen(0, 0, ...)` returns `{500.0f, 450.0f}` — the offset translates and does not scale.
- `worldToScreenRadius(kPlayerRadius, 800.0f)` returns `4.0f` (`0.5 / 100 * 800`).
- `aimFromCursor(0, 0, 3, 4, ax, ay)` yields `{0.6f, 0.8f}` bit-exactly; `aimFromCursor(1, 1, 1, 1, ...)` yields `{0, 0}`; a NaN or infinite input yields `{0, 0}`; and `aimFromCursor` never returns a non-finite value for any of those.

Run:
```bash
scripts/tw cmake --build build/plain -j8 --target view_test && \
scripts/tw ctest --test-dir build/plain -R view_test --output-on-failure
```
Expected: FAIL at the build step — `src/client/view.h: No such file or directory`.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: `worldToScreen` computes `scale = side / (2.0f * sim::kArenaHalf)` and returns
`{ox + (wx + kArenaHalf) * scale, oy + (kArenaHalf - wy) * scale}`.
`worldToScreenRadius` is `r * scale`. `aimFromCursor` computes the delta, returns `{0, 0}`
when either input is non-finite or the squared length is `0` or non-finite, and otherwise
divides by `std::sqrt`. This file is pure arithmetic with no raylib include, which is
exactly why it is testable while the renderer is not — keep every non-drawing decision
here.

```bash
scripts/tw cmake --build build/plain -j8 --target view_test && \
  scripts/tw ctest --test-dir build/plain -R view_test --output-on-failure && \
  git add CMakeLists.txt src/client/view.h src/client/view.cpp tests/client/view_test.cpp && \
  git commit -m "feat: add the pure world-to-screen mapping for the renderer"
```

Expected: PASS, then one commit.

**Checkpoint 2: the GUI client builds against raylib and renders one frame**

This checkpoint answers the deferred display question. Its RED is a build failure for a
target that does not exist; its GREEN is a window that actually opened.

- [ ] **Step 1: Write the failing test, then run it**

Changes required, all in this checkpoint:
- `Dockerfile`: add `libgl1-mesa-dev libx11-dev libxrandr-dev libxi-dev libxcursor-dev libxinerama-dev` to the existing `apt-get install` line.
- `scripts/tw`: bump `IMAGE` to `tickwire-dev:gcc10-cmake3.28.4-x11` — **required**, since `scripts/tw` skips the build when the tag already exists, so editing the `Dockerfile` alone silently reuses the old image. Add, conditionally, X passthrough so a GUI run can reach WSLg: when `/tmp/.X11-unix` exists **and** `$DISPLAY` is set, append `-e DISPLAY="$DISPLAY" -v /tmp/.X11-unix:/tmp/.X11-unix`. The condition is what keeps CI (no X socket) working unchanged.
- `CMakeLists.txt`: add `option(TW_BUILD_GUI "Build the raylib client" OFF)`. Inside `if(TW_BUILD_GUI)`, `FetchContent_Declare(raylib GIT_REPOSITORY https://github.com/raysan5/raylib.git GIT_TAG 4.5.0)` with `set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)` and `set(BUILD_GAMES OFF CACHE BOOL "" FORCE)`, then `add_executable(tw_client apps/tw_client.cpp)` linking `libclient` and `raylib`, and `add_test(NAME client_selftest COMMAND tw_client --selftest)` with `set_tests_properties(client_selftest PROPERTIES SKIP_RETURN_CODE 77)`.
- raylib's warnings must not fail our build: apply `-Wall -Wextra -Werror` only to our targets (they come from `tickwire_sim_flags`, which raylib does not link), and do not add them globally.

Spec:
- `tw_client --selftest` with a display available: opens an 800×800 window titled `tickwire`, draws one frame (a background plus the arena border), closes it, and exits `0`.
- `tw_client --selftest` with `DISPLAY` unset: exits `77`, which CTest reports as **skipped**, not failed.

Run:
```bash
scripts/tw cmake -S . -B build/gui -DCMAKE_BUILD_TYPE=RelWithDebInfo -DTW_BUILD_GUI=ON && \
scripts/tw cmake --build build/gui -j8 --target tw_client && \
scripts/tw ctest --test-dir build/gui -R client_selftest --output-on-failure
```
Expected: FAIL at the configure or build step — `TW_BUILD_GUI` does not exist and
`apps/tw_client.cpp` does not exist.

> **If this checkpoint cannot go green**, stop and record the exact failure in
> `docs/project-history.md` as a P2 finding — the message, and which of the three layers
> failed (raylib's build, the X connection, or GLFW's window creation). Then skip to
> Checkpoint 3's fallback note and Task 10; Tasks 1–8 stand alone as a complete,
> tested, shippable phase. Do not spend the session fighting the display: the design doc's
> deliverable is the numbers table and the netcode, and the fallback (building the renderer
> on the host) needs a `g++-10` install that requires a password this session does not have,
> so it is the **user's** decision, not a workaround to improvise.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: `apps/tw_client.cpp` parses `--selftest`, `--host`, `--port`, `--latency-ms`.
In `--selftest` mode it returns `77` immediately when `std::getenv("DISPLAY")` is null or
empty; otherwise it calls `InitWindow(800, 800, "tickwire")`, one
`BeginDrawing`/`ClearBackground`/`DrawRectangleLines`/`EndDrawing`, `CloseWindow()`, and
returns `0`. No socket is opened in selftest mode — this checkpoint proves the display path
and nothing else.

```bash
scripts/tw cmake -S . -B build/gui -DCMAKE_BUILD_TYPE=RelWithDebInfo -DTW_BUILD_GUI=ON && \
  scripts/tw cmake --build build/gui -j8 --target tw_client && \
  scripts/tw ctest --test-dir build/gui -R client_selftest --output-on-failure && \
  git add Dockerfile scripts/tw CMakeLists.txt apps/tw_client.cpp && \
  git commit -m "feat: build the raylib client and prove it can open a window"
```

Expected: PASS (or SKIP if no display, which is a pass for CTest), then one commit.

**Checkpoint 3: the demo runs — two clients, a server, and a latency slider**

- [ ] **Step 1: Write the failing test, then run it**

Spec — `scripts/demo.sh` runs **inside one container invocation**, so the server and the
clients share one network namespace and `127.0.0.1` means the same thing to all of them:
it starts `tw_server --port 0`, reads its `port=<n>` line, launches two `tw_client --port <n>`
processes, waits for them, and kills the server on exit via `trap`. It takes the three
binary paths as arguments so it does not hard-code a build directory.

The RED, following the precedent of P1's Task 8:
```bash
test -x scripts/demo.sh && scripts/tw bash -n scripts/demo.sh
```
Expected: FAIL — exit 1; the file does not exist.

Full-client behavior, verified by running the demo and recording the result in the journal:
- The window shows the arena border, one filled circle per player in the latest snapshot, the local player in a distinct color, and a HUD line reading `tick=<n> latency=<n>ms players=<n>`.
- `WASD` sets the move vector (normalized by `World`, so diagonal is not faster); the mouse position sets aim through `aimFromCursor`; left click sets `fire` for that tick.
- `[` and `]` change the client's simulated inbound latency by 25 ms per press, clamped to `[0, 500]`, by reconstructing the `SimulatedTransport` wrapper's config. At 0 ms the local circle tracks the keys with a visible ~50 ms lag (one snapshot interval plus a tick); at 200 ms it lags by roughly a third of a second, which is the "visibly unplayable" state P3 exists to fix. **Do not add smoothing to hide it.**
- Closing the window sends `kLeave` before exiting, and the other client sees the player disappear within one snapshot.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: `tw_client`'s non-selftest path binds a `UdpTransport` to an ephemeral port,
wraps it in `net::SimulatedTransport` with `{latency_ms = --latency-ms, jitter_ms = 0, loss_permille = 0, seed = 1}`,
constructs a `Client` over the wrapper, and runs a raylib loop at 60 FPS
(`SetTargetFPS(60)`): each frame call `client->tick(server::monotonicMs())`,
`transport wrapper .advanceTick()`, read input, `client->sendInput(...)`, then draw
`client->latestSnapshot()` through `worldToScreen`. Draw only what the snapshot says — no
local extrapolation of any kind.

```bash
test -x scripts/demo.sh && scripts/tw bash -n scripts/demo.sh && \
  scripts/tw cmake --build build/gui -j8 --target tw_client && \
  scripts/tw ctest --test-dir build/gui -R client_selftest --output-on-failure && \
  git add scripts/demo.sh apps/tw_client.cpp && \
  git commit -m "feat: render snapshots with a latency slider and add the demo script"
```

Expected: PASS, then one commit.

**Task boundary:**
```bash
scripts/tw bash scripts/ci.sh && \
scripts/tw cmake --build build/gui -j8 && \
scripts/tw ctest --test-dir build/gui --output-on-failure
```

---

## Task 10: Record what changed

P1's format was frozen in writing; this phase amended it once and added three subsystems.
Write both down, so P3 reads a specification rather than reverse-engineering a server.

**Files:**
- Modify: `docs/wire-format.md`, `docs/project-history.md`, `CLAUDE.md`
- Create: `README.md` (the project has none, and the demo now exists to describe)

**Checkpoint 1: the format, the conventions, and the phase's decisions are recorded**

- [ ] **Step 1: Write the failing test, then run it**

Run:
```bash
grep -q "aim_x" docs/wire-format.md && grep -q "version 2" docs/wire-format.md && \
grep -q "P2 — Authoritative server" docs/project-history.md && \
grep -q "src/server/" CLAUDE.md && test -f README.md
```
Expected: FAIL — exit 1; none of that is in place.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract — `docs/wire-format.md` gains:
- the amended `InputCommand` table (25 bytes, `aim_x` at 16, `aim_y` at 20, `fire` at 24) replacing the 17-byte one, and the version bump `1 → 2` stated as a **deliberate amendment with its date and reason** (hitscan needs an aim direction; P1 froze the payload before any consumer existed);
- the `JoinRequest` (empty), `JoinAccept` (4 bytes, `player_id`, never `0`) and `Leave` (empty) payload layouts, and the rule that a server rejects a full-table join by replying `kLeave` with `ack_seq` echoing the request;
- which header fields P2 now actually populates — `tick`, `send_time_ms`, `ack_tick` (per-recipient, the highest input tick that recipient has sent), `seq`/`ack_seq` (the join/leave channel) — and which remain reserved for P3's clock sync;
- a note that `MsgType` values `6..255` are unused and that P6's hit-feedback message can be added additively, without touching the header or the version.

`docs/project-history.md` gains a `## P2 — Authoritative server, `World`, first demo`
section with one entry per load-bearing event: the **pivot** that reopened the wire format
for `aim_x`/`aim_y` at version 2 (with the alternatives considered — a separate `kFire`
message, and aiming along the movement vector — and why they lost); the decision that
`PacketRing` prefigures P5's `SpscRing` API while being explicitly non-atomic; the decision
that endpoint↔player binding is a single chokepoint in the router, discharging P1's
deferred finding; the decision that hit counts stay server-side because `PlayerState` is
frozen; spawn positions as server policy rather than simulation; the 60/20 Hz cadence as
the thing that creates P4's need; the outcome of the raylib display question, stated
plainly either way; and every finding from Task 8's security review.

`CLAUDE.md` gains: `src/server/`, `src/client/` and `apps/` rows in the File Structure
table; a line in the Wire protocol section recording that the format is at **version 2**
and that `docs/wire-format.md` is authoritative; the `-DTW_BUILD_GUI=ON` build
configuration and the fact that changing the `Dockerfile` requires bumping the image tag in
`scripts/tw`; and the rule that `Server::tick`/`Client::tick` take `now_ms` as a parameter
so nothing testable reads a clock.

`README.md`: what Tickwire is (three sentences, no fabricated claims — the design doc's
rejection of the "top 1% saturation" statistic applies to every number in this file), how
to build and test (`scripts/tw bash scripts/ci.sh`), how to run the demo
(`scripts/tw bash scripts/demo.sh`, with the display caveat stated honestly), and what
works today versus what P3–P6 add. State explicitly that the client has **no prediction
yet** and that the lag is intentional at this phase.

```bash
grep -q "aim_x" docs/wire-format.md && grep -q "version 2" docs/wire-format.md && \
  grep -q "P2 — Authoritative server" docs/project-history.md && \
  grep -q "src/server/" CLAUDE.md && test -f README.md && \
  scripts/tw bash scripts/ci.sh && \
  git add docs/wire-format.md docs/project-history.md CLAUDE.md README.md && \
  git commit -m "docs: record the version 2 format and P2's structure"
```

Expected: PASS — all three configurations plus the toolchain assertions green, then one commit.

**Task boundary:** `scripts/tw bash scripts/ci.sh`, plus `build/gui` green if Task 9
completed. **The branch is now green and verified.**

Hand off to `finishing-a-development-branch` for the integration decision. Do not merge or
push from within this plan.

---

## Self-Review

**Spec coverage.** The design doc's P2 row asks for an authoritative single-threaded
server that is visibly laggy and demoable. *Authoritative* — Task 6: the server owns the
only `World`, and no client state is trusted (Checkpoint 2 proves an input from the wrong
endpoint changes nothing). *Single-threaded* — Task 7's loop is one thread; `PacketRing`
(Task 4) is the seam the design doc requires be built now so P5 swaps an implementation.
*Visibly laggy* — Task 8 Checkpoint 2's contract forbids interpolation and prediction;
Task 9 Checkpoint 3 says not to smooth it. *Demoable* — Task 9. Resolution doc § Q2's
`World` surface lands in Tasks 1–2 (`applyInput`, `step`, `writeSnapshot`, `resolveHitscan`),
plus the `addPlayer`/`removePlayer` roster calls the server needs, which § Q2 did not list
and which are additive. § Q4's ring contract is honored in shape by Task 4. § Q5's
deferred display question is Task 9 Checkpoint 2. Design doc § Architecture point 4
(epoll + timerfd) is Task 7; point 5 (60 Hz sim, 20 Hz snapshots) is Task 6 Checkpoint 3;
point 6 (no wall-clock in the simulation) is enforced by `tick(now_ms)` and stated in
Global Constraints; point 7 (unreliable inputs/snapshots, minimal seq/ack for join/leave
only) is Tasks 6 and 8. `CLAUDE.md`'s mandatory network-surface security review is Task 8's
boundary. P1's deferred finding — `player_id` unbound from `Endpoint` — is Task 5
Checkpoint 2 plus Task 6 Checkpoint 2.

**Deferred deliberately, and where to:** prediction, reconciliation, clock sync, RTT and
drift estimation → P3 (the header fields carry them already and P2 populates
`send_time_ms`/`ack_tick`, so P3 adds logic, not layout). Entity interpolation and snapshot
delta → P4. Threading, `SpscRing`, `recvmmsg`/`sendmmsg` batching, the per-call drain cap
P1 deferred, and the numbers table → P5. Lag compensation and a client-visible hit event →
P6. Player-player collision, health, and score: **not in the project** — the design doc's
one-sentence game rule is the boundary, and this is recorded so it is not re-litigated.

**Type consistency.** `sim::World`'s methods keep their names from Task 1 through Task 8.
`sim::InputCommand` gains `aim_x`/`aim_y` once, in Task 3, and every later task uses the
7-field form. `net::framePacket` and `ByteWriter::bytes` (Task 3) are used unchanged by
Tasks 6, 8 and 9. `server::SessionTable::authorize` (Task 5) is called only from Task 6's
router. `PacketRing`'s four calls (Task 4) are used only inside `Server`. `Client::tick`
and `Server::tick` both take `uint32_t now_ms`, in that name and order. `kInvalidPlayerId`
means "no player" everywhere: `SessionTable::joinOrGet`, `playerFor`, `Client::playerId`,
and `encodeJoinAccept`'s rejection.

**Checkpoint falsifiability.** Every checkpoint names a failing assertion at the surface
its own test calls. Build-step REDs on a symbol that does not exist: T1C1, T1C2, T2C1,
T3C1, T3C3, T3C4, T4C1, T5C1, T5C2, T5C3, T6C1, T7C1, T7C2, T7C3, T8C1, T8C2, T8C3, T9C1,
T9C2. Behavioral REDs: T1C3 (position reads ~50.8 instead of 49.5 at the wall), T2C2 (a tie
returns `7` instead of `3` under insertion order), T3C2 (the golden vector is 17 bytes, not
25), T4C2 (a fifth write into a 4-slot ring is accepted), T6C2 (**the spoofed input moves
another player**), T6C3 (zero snapshots recorded), T6C4 (`kLeave` falls through to the
dropped-packet branch and the player stays), T9C3 and T10C1 (`test -x` / `grep`, the same
documentation-RED shape P1's Task 8 used).

**Checkpoints deliberately not created.** "Add the bounds check" style checkpoints are
absent by construction — every guard ships with the code it guards, per Global Constraints.
`Server`'s ingest-overflow counter has no checkpoint of its own: filling a 256-slot ring
from a `LoopbackTransport` capped at 256 packets cannot exceed it, so the assertion would
be unfalsifiable at the surface the test can reach; the counter exists for P5's benchmark,
and that is where it gets a test. `TickTimer`'s move semantics and fd lifetime are folded
into T7C1 rather than split out, because they are consequences of the same constructor
contract, and P1's identical `UdpTransport` split produced a checkpoint that passed when
written.

**Two risks worth stating.** First, **Task 9's display path is unproven** — the deps are
missing from the image (verified while planning) and WSLg passthrough into a container is
plausible but untested here. The plan is ordered so this costs nothing else, and the
checkpoint says explicitly to stop and record rather than improvise, because the documented
fallback (host build) needs a `g++-10` install requiring a password this session does not
have. Second, **Task 6 is the largest task** — four checkpoints over a template class that
touches the world, the sessions and the ring. It is not split because each checkpoint is a
genuine RED at the same public surface, and splitting the class across tasks would mean
shipping a `Server` that accepts joins but drops inputs. If it runs long, the task boundary
after Checkpoint 2 (join + authorized input) is a defensible place to pause.
