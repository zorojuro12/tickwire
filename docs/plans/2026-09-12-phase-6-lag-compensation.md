# Phase 6 — Lag Compensation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use the `executing-plans` skill to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Resolve every shot against the world **exactly as the shooter drew it**
— the server rewinds remote players to the shooter's view tick using the same
interpolation code the client rendered with — show confirmed hits on the client,
and measure the hit rate on shots aimed at a moving target at 200 ms RTT with
compensation on vs off.

**Architecture:** The client already renders remote players at
`Interpolator::renderTick()`, sampled from its `SnapshotRing`, and the server
already keeps an identical 16-snapshot `history_` ring (it is what P4's delta
baselines are looked up in). P6 extracts the sampling logic into one shared pure
function (`net::samplePlayerAt`), has the client stamp the tick it drew on every
input (`InputCommand::view_tick`, protocol v3), and has the server build a
scratch *rewound view* — targets sampled from `history_` at that tick through the
same function, the shooter at its live position — and resolve the hitscan against
it through a snapshot-based `sim::resolveHitscan`. Nothing about movement or
authoritative state changes: the rewound view is a throwaway snapshot used for
one ray test, never written back. A hit is reported to the shooter in a new
additive `kHitConfirm` message.

**Tech Stack:** C++20 (GCC 10.5 pinned), CMake 3.28.4, GoogleTest/CTest, raylib
(GUI build only). All inside the pinned `tickwire-dev:gcc10-cmake3.28.4-x11` image.

**Spec:**
- [`docs/specs/2026-09-04-tickwire-design.md`](../specs/2026-09-04-tickwire-design.md)
  — § "The idea" (lag compensation via server-side rewind), § "The demo"
  (*"Toggle lag compensation and shots that looked like hits start registering"*),
  § "Build order" (P6: *"High, but localized and additive"*)
- [`docs/specs/2026-09-04-architecture-resolution.md`](../specs/2026-09-04-architecture-resolution.md)
  — § Q2 (`libsim` surface: POD only, no allocation)
- [`docs/wire-format.md`](../wire-format.md) — the v2 format this plan amends
- [`docs/project-history.md`](../project-history.md) — P2's hit-feedback
  deferral ("P6 can add a client-visible hit-feedback `MsgType` additively"), P3's
  `hits_` id-reuse note, P4's Decisions 4–5 (the interpolation timeline), and the
  **P6 planning entry recording why the wire format reopens here**

---

## Global Constraints

Every task's requirements implicitly include this section. Values are copied
verbatim from `CLAUDE.md` and the specs — do not re-derive them.

**Build and test**

- **Never invoke the host `g++`, `cmake`, or `ctest`.** The host has GCC 9.4 and
  CMake 3.16 and cannot build this project. Everything goes through
  `scripts/tw <command...>`.
- **`ctest` does not build. Always `cmake --build` first.** A bare `ctest -R foo`
  runs the *previously built* binary (a newly written test case is absent, so the
  run is falsely green), and a regex matching no target **exits 0**. Every test
  command is `cmake --build <dir> -j8 && ctest --test-dir <dir> -R <regex> --output-on-failure`.
- **A brand-new test target must be registered in `CMakeLists.txt` in the same
  Step 1 that writes its first test** — otherwise the RED run matches nothing and
  exits 0. Every new target name below is chosen so its `-R` regex matches it
  alone.
- **Scope every checkpoint with `-R <regex>`.** Run the full suite only at a task
  boundary (`scripts/tw bash scripts/ci.sh`). Never inside a checkpoint.
- **TSan runs need `setarch -R` around `ctest`, not around the build.** `ci.sh`
  already does this.
- Configurations: `build/plain`, `build/asan`, `build/tsan` (all via `ci.sh`), and
  `build/gui` (`-DTW_BUILD_GUI=ON`, for `tw_client` only).

**Language**

- **No `std::format`** (GCC 10 lacks it). **No `std::bit_cast`** — pun
  `float`↔`uint32_t` with `std::memcpy`.
- `-Wall -Wextra -Werror`, non-negotiable.
- **Verified during planning, in the pinned image:** GCC 10 raises
  `-Werror=missing-field-initializers` on a C++20 designated initializer that
  omits a trailing member **with no default member initializer** (e.g.
  `S{.a = 1, .c = true}` where `S` has a fourth field `uint32_t d;`). Giving that
  field a default member initializer (`uint32_t d = 0;`) silences it and the
  struct stays an aggregate and trivially copyable. This is why
  `InputCommand::view_tick` is declared `= 0` — more than a dozen existing designated
  initializers of `InputCommand` across `src/` and `tests/` must keep
  compiling untouched.
- Types `PascalCase`, constants `kPascalCase`, members `snake_case_`.

**Architecture invariants**

- **The wire format reopens exactly once, in Task 1**, to protocol version 3:
  `InputCommand` gains `view_tick` (25 → 29 bytes) and `MsgType::kHitConfirm = 7`
  is added. Any later task that believes it needs another format change must
  **stop and record a finding**, not make the change. `sim::kMaxPlayers` stays 32.
- **Rewind symmetry — the load-bearing invariant of this phase.** A rewound
  target's position must come from `net::samplePlayerAt` over the server's
  `history_`, the *same function* `client::Interpolator::sample` calls over the
  client's ring. Never a server-side reimplementation, never per-tick
  authoritative positions substituted in. Two code paths computing "where was
  player 2 at tick 91" is exactly the divergence class `libsim` exists to close.
- **Lag compensation changes hit resolution and nothing else.** No position,
  velocity, input, or `World` state is ever written from a rewound view. The
  rewound `sim::WorldSnapshot` is a local scratch value.
- **The shooter is never rewound.** The shooter's own origin is its live
  position, which is what its client drew (it predicts itself exactly — P3).
- **The local player is predicted, never interpolated; a remote player is
  interpolated, never predicted** (`CLAUDE.md` verified constraint) — P6 relies on
  this split rather than changing it.
- **The server consumes exactly one input per player per tick, at the tick it
  was stamped for** (`CLAUDE.md`). A fire resolves in `Server::tick()`'s pass 2
  for that same tick — P6 changes only *what* pass 2 tests the ray against.
- **No wall-clock reads in testable code.** `Server::tick(now_ms)` /
  `Client::tick(now_ms)` take time as a parameter; only `apps/` calls
  `server::monotonicMs()`.
- **`apps/` are thin** — argument parsing, a clock, a loop, drawing.
- **`libsim` boundary:** no I/O, no wall-clock, no allocation; only trivially-
  copyable POD crosses it. Float determinism flags propagate `PUBLIC` via
  `tickwire_sim_flags` (and therefore into `libnet`, `libserver`, `libclient`,
  which all link `libsim` `PUBLIC`). Never `-march=native`, never `-ffast-math`.
- **`ThreadedRunner` ownership split (P5):** everything `tick()` touches is
  sim-thread-only. Every counter and member P6 adds to `Server` is touched only by
  `tick()`/`route()`, so the split needs no change — keep it that way.

**Test hygiene**

- **Heap-allocate large test objects** via `std::make_unique`:
  `RecordingTransport` (~1.24 MB), `Server<…>`, `LoopbackTransport` (~310 KB),
  `SimulatedTransport` (~157 KB), `net::SnapshotRing` (~12.5 KB). `sim::World`
  (~2 KB) and `SessionTable` (~1 KB) may stay on the stack.
- **`tests/server/server_test.cpp` (1133 lines) and `tests/client/client_test.cpp`
  (977 lines) are already past the 800-line hard maximum.** Do not grow them. New
  server and client tests go in new files (`server_lagcomp_test.cpp`,
  `client_lagcomp_test.cpp`), each with its own small file-local inject helpers
  modeled on the existing ones. Deduplicating those helpers into `tests/support/`
  is a `/refactor-clean` follow-up, deliberately out of scope here.
- Coverage: 80% project-wide; **`libsim` is held to 100%** — Task 2's new function
  must have every branch exercised.
- File size 200–400 lines typical, 800 hard maximum.

**Commits**

- `type: description` (feat, fix, refactor, docs, test, chore, perf, ci).
- One commit per checkpoint, **chained behind its test with `&&`** — never `;`,
  never a separate line.
- `git add` names exact paths. **Never** `git add -A` or `git add .`.

**Measurement honesty**

- The phase's headline number is a hit rate, reported **as measured**. The
  expected shape is *near-total with compensation, near-zero without*, but the
  uncompensated arm will register some hits (a target reversing direction passes
  back through where it was drawn). Do not retune the scenario to push either
  number toward a rounder one; report it and explain it.

---

## File Structure

**New files**

| Path | Responsibility |
|---|---|
| `src/server/rewind.h` / `rewind.cpp` | `server::RewindRequest`, `server::kMaxRewindTicks`, and `server::buildRewoundView` — pure: builds the world as a shooter saw it, or refuses. |
| `tests/server/rewind_test.cpp` | `buildRewoundView`: placement, every rejection rule, id-reuse exclusion. |
| `tests/server/server_lagcomp_test.cpp` | `Server` resolves shots against the rewound view, reports hits, resets per-id counters on reuse. |
| `tests/client/client_lagcomp_test.cpp` | `Client` stamps the view tick it drew; handles `kHitConfirm`. |
| `tests/client/lagcomp_hitrate_test.cpp` | The measurement: hit rate on shots at a moving target, 200 ms RTT, compensation on vs off. |

**Modified files**

| Path | Change |
|---|---|
| `src/sim/sim.h` | `InputCommand::view_tick` (`uint32_t`, `= 0`, last member). |
| `src/sim/world.h` / `world.cpp` | Free function `sim::resolveHitscan(const WorldSnapshot&, …)`; `World::resolveHitscan` delegates to it. |
| `src/net/protocol.h` / `protocol.cpp` | `kProtocolVersion = 3`, `kInputBytes = 29`, `MsgType::kHitConfirm = 7`, `kMaxMsgType = 7`, `view_tick` codec. |
| `src/net/framing.h` / `framing.cpp` | `net::HitConfirm`, `kHitConfirmBytes = 8`, its codec. |
| `src/net/snapshot_ring.h` / `snapshot_ring.cpp` | Free function `net::samplePlayerAt`. |
| `src/client/interpolation.cpp` | `Interpolator::sample` delegates to `net::samplePlayerAt`. |
| `src/server/session.h` / `session.cpp` | `Entry::joined_tick`, `SessionTable::joinedTick`. |
| `src/server/server.h` | Pass 2 resolves via the rewound view; `shots`, `rewoundShots`, `rewindsRejected`; `kHitConfirm` send; per-id counter reset. |
| `src/client/client.h` | `view_tick` stamping, lag-comp toggle, `kHitConfirm` handling. |
| `apps/tw_server.cpp` | Prints the rewind counters. |
| `apps/tw_client.cpp` | `L` toggle, HUD fields, hit flash, aim tracer. |
| `CMakeLists.txt` | `rewind.cpp` in `libserver`; four new test targets; one app-output test. |
| `tests/net/{protocol,framing,robustness,snapshot_ring}_test.cpp`, `tests/sim/world_test.cpp`, `tests/server/session_test.cpp` | New cases per task (none of these is near 800 lines except as noted). |
| `docs/wire-format.md`, `CLAUDE.md` | v3 amendment (Task 1). File-structure and verified-constraint rows (Task 10). |
| `README.md`, `docs/project-history.md` | Writeup (Task 10). |

**Interface summary** (the exact names later tasks depend on):

```cpp
// src/sim/sim.h
struct InputCommand { uint32_t player_id; uint32_t tick; float move_x, move_y;
                      float aim_x, aim_y; bool fire; uint32_t view_tick = 0; };

// src/sim/world.h
std::optional<uint32_t> resolveHitscan(const WorldSnapshot& view, uint32_t shooter,
                                       float aim_x, float aim_y);

// src/net/protocol.h
inline constexpr uint8_t kProtocolVersion = 3;
inline constexpr size_t  kInputBytes = 29;
enum class MsgType : uint8_t { /* ... */ kSnapshotDelta = 6, kHitConfirm = 7 };
inline constexpr uint8_t kMaxMsgType = 7;

// src/net/framing.h
inline constexpr size_t kHitConfirmBytes = 8;
struct HitConfirm { uint32_t target_id; uint32_t fire_tick; };
bool encodeHitConfirm(const HitConfirm& in, ByteWriter& w);
bool decodeHitConfirm(ByteReader& r, HitConfirm& out);

// src/net/snapshot_ring.h
bool samplePlayerAt(const SnapshotRing& ring, uint32_t player_id, uint32_t tick,
                    float& x, float& y) noexcept;

// src/server/session.h
uint32_t SessionTable::joinedTick(uint32_t player_id) const noexcept;  // 0 if unknown

// src/server/rewind.h
inline constexpr uint32_t kMaxRewindTicks = 45;   // 750 ms; derivation in Task 4
struct RewindRequest { uint32_t shooter; uint32_t view_tick; uint32_t fire_tick; };
bool buildRewoundView(const sim::World& live, const net::SnapshotRing& history,
                      const SessionTable& sessions, const RewindRequest& req,
                      sim::WorldSnapshot& out) noexcept;

// src/server/server.h (additions)
uint64_t Server::shots(uint32_t player_id) const noexcept;
uint64_t Server::rewoundShots() const noexcept;
uint64_t Server::rewindsRejected() const noexcept;

// src/client/client.h (additions)
void     Client::setLagCompensationEnabled(bool on) noexcept;
bool     Client::lagCompensationEnabled() const noexcept;
uint32_t Client::hitsConfirmed() const noexcept;
uint32_t Client::lastHitTarget() const noexcept;
uint32_t Client::lastHitFireTick() const noexcept;
```

---

## Task 1: Wire format v3 — `view_tick` and `kHitConfirm`

The format's one reopening this phase, contained to this task the same way P2
contained its v2 amendment to one task. Why a version bump rather than a derived
or smuggled value is recorded in `docs/project-history.md`'s P6 planning entry;
do not re-litigate it here.

**Files:**
- Modify: `src/sim/sim.h`, `src/net/protocol.h`, `src/net/protocol.cpp`,
  `src/net/framing.h`, `src/net/framing.cpp`
- Modify: `docs/wire-format.md`, `CLAUDE.md` (§ "Wire protocol")
- Test: `tests/net/protocol_test.cpp`, `tests/net/framing_test.cpp`,
  `tests/net/robustness_test.cpp`

**Interfaces:**
- Produces: `sim::InputCommand::view_tick`, `net::kProtocolVersion == 3`,
  `net::kInputBytes == 29`, `net::MsgType::kHitConfirm`, `net::kMaxMsgType == 7`,
  `net::HitConfirm`, `net::kHitConfirmBytes`, `net::encodeHitConfirm`,
  `net::decodeHitConfirm`.

**Checkpoint 1: `InputCommand` carries `view_tick` under protocol version 3**

- [ ] **Step 1: Write the failing test, then run it**

Spec — edit existing tests in `tests/net/protocol_test.cpp`:
- `InputCommandCodecTest.EncodesToExactBytesAndDecodesBack`: the input gains
  `.view_tick = 1210`. The golden vector becomes **29 bytes**: the existing 25
  bytes unchanged, followed by `0xBA, 0x04, 0x00, 0x00` (1210 = `0x000004BA`,
  little-endian). The decode assertions add `EXPECT_EQ(out.view_tick, in.view_tick)`.
- `InputCommandCodecTest.RejectsFramingMismatch`: add a 25-byte payload (the v2
  size) and a 30-byte payload, both rejected with the output struct untouched.
- `InputCommandCodecTest.FireIsLenientToAnyNonzeroByte` and every other test
  building a raw `kInputBytes` buffer: extend to 29 bytes (trailing view tick
  bytes `0x00`) wherever a literal 25-byte array is spelled out.
- The golden header (`kGoldenHeaderBytes`): the version byte at offset 4 becomes
  `0x03`. In `ProtocolHeaderTest.RejectsUnknownMagicVersionOrType`, the
  wrong-version case changes from `mutated(4, 0x03)` to `mutated(4, 0x02)` — **a
  version-2 header must now be rejected**; add `mutated(4, 0x04)` as well.

Run: `scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain -R 'protocol_test|robustness_test|framing_test' --output-on-failure"`
Expected: FAIL — compile error, `'struct sim::InputCommand' has no member named 'view_tick'`.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: `sim::InputCommand` gains `uint32_t view_tick = 0;` as its **last**
member (default member initializer required — see Global Constraints).
`kInputBytes = 29`; `encodeInput` appends `w.u32(in.view_tick)` after `fire`;
`decodeInput` reads it last. The decoder accepts any `view_tick` value: its
plausibility depends on session state and is judged by the server (Task 4), not
the codec. `kProtocolVersion = 3` with the comment updated to say what v3 added.

`docs/wire-format.md`, in the same commit: title/intro ("amended at P2 and P6"),
`kProtocolVersion = 3`, a **v3 (P6)** entry in "Version history" (adds
`InputCommand::view_tick`; v2 rejected outright; points to the P6 history entry),
the `InputCommand` table grows to 29 bytes with row
`| 25 | 4 | view_tick — the tick whose world the sender drew when it sent this input; 0 = uncompensated (added at v3) |`,
and the decoder-strictness list notes that `view_tick` is not range-checked by
the codec and why. `CLAUDE.md` § "Wire protocol": heading and first bullet say
version 3, amended at P2 and P6.

**If `RobustnessTest.RandomByteBuffersNeverCrashADecoder` now fails
`any_payload_decoded`:** the input-shaped trial length moved from 49 to 53 bytes,
the same seed-sensitivity P1/P2/P4 each recorded. Re-search seeds upward from `1`
for the smallest that makes the sweep pass, update the seed and extend the
comment's seed history with this change. A fixed seed is for reproducibility, not
sacred — but record it; Task 10 carries it into `docs/project-history.md`.

```bash
scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain -R 'protocol_test|robustness_test|framing_test' --output-on-failure" && \
  git add src/sim/sim.h src/net/protocol.h src/net/protocol.cpp tests/net/protocol_test.cpp tests/net/robustness_test.cpp docs/wire-format.md CLAUDE.md && \
  git commit -m "feat: add view_tick to InputCommand at protocol version 3"
```

Expected: PASS, then one commit. (If the robustness seed did not need changing,
`git add` of an unmodified path is harmless.)

**Checkpoint 2: `kHitConfirm` is a valid message type with a strict 8-byte codec**

- [ ] **Step 1: Write the failing test, then run it**

Spec:
- `tests/net/framing_test.cpp`, new `HitConfirmCodecTest`:
  - `EncodesToExactBytesAndDecodesBack`: `HitConfirm{.target_id = 2, .fire_tick = 1234}`
    encodes to exactly `02 00 00 00 D2 04 00 00` and decodes back field-for-field.
  - `RejectsFramingMismatch`: 7-byte and 9-byte payloads rejected, output untouched.
  - `RejectsInvalidTargetIdOnBothSides`: `encodeHitConfirm` with `target_id == 0`
    returns `false`; decoding `00 00 00 00 D2 04 00 00` returns `false` with the
    output untouched.
- `tests/net/protocol_test.cpp`:
  - `ProtocolTest.HeaderAcceptsSnapshotDeltaType`: remove its
    `EXPECT_EQ(kMaxMsgType, 6)` and its raw-type-7-is-rejected block (both become
    false at this checkpoint); keep the delta-acceptance assertions.
  - New `ProtocolTest.HeaderAcceptsHitConfirmType`: `MsgType::kHitConfirm == 7`,
    `kMaxMsgType == 7`, a header with type `kHitConfirm` round-trips, and a raw
    header with type byte `0x08` is rejected.
  - `RejectsUnknownMagicVersionOrType`: `mutated(5, 0x07)` ("one past
    kMaxMsgType") becomes `mutated(5, 0x08)`.

Run: `scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain -R 'protocol_test|robustness_test|framing_test' --output-on-failure"`
Expected: FAIL — compile error, `'kHitConfirm' is not a member of 'net::MsgType'`.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: `MsgType::kHitConfirm = 7`, `kMaxMsgType = 7`. In `framing.h`:
`inline constexpr size_t kHitConfirmBytes = 8;`, `struct HitConfirm { uint32_t target_id; uint32_t fire_tick; };`,
and the two codec functions, written exactly like `encodeJoinAccept`/
`decodeJoinAccept`: strict `remaining() == kHitConfirmBytes`, `target_id != kInvalidPlayerId`
on both sides, output assigned only on success. `fire_tick` is any value.

`docs/wire-format.md`, same commit: the `MsgType` list gains `kHitConfirm = 7`
(`kMaxMsgType = 7`, `8..255` unused), a new "`HitConfirm` payload — 8 bytes"
section (server → shooter only; `target_id` never 0; `fire_tick` echoes the
`InputCommand::tick` of the shot that hit; the header's `tick` is the server tick
the hit resolved on), and a decoder-strictness bullet.

Same robustness-seed rule as Checkpoint 1: widening `kMaxMsgType` shifts every
draw in `RandomByteBuffersNeverCrashADecoder` (P4 hit exactly this). Re-search if
it fails; record it.

```bash
scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain -R 'protocol_test|robustness_test|framing_test' --output-on-failure" && \
  git add src/net/protocol.h src/net/framing.h src/net/framing.cpp tests/net/protocol_test.cpp tests/net/framing_test.cpp tests/net/robustness_test.cpp docs/wire-format.md && \
  git commit -m "feat: add the HitConfirm message type"
```

Expected: PASS, then one commit.

- [ ] **Task boundary:** `scripts/tw bash scripts/ci.sh` — full suite, all three
  configurations. Every existing client/server test must still pass: both sides
  share the one encoder, so a v3 client and v3 server interoperate unchanged.

---

## Task 2: `sim::resolveHitscan` over a snapshot

`World::resolveHitscan` reads `players_` directly, so it can only ever test the
live world. A rewound view is a `WorldSnapshot`. Moving the ray test onto a
snapshot — and making `World`'s version a thin wrapper — gives both paths one
implementation, and the existing `WorldTest.Hitscan*` suite becomes the proof
that the live path's behavior did not move.

**Files:**
- Modify: `src/sim/world.h`, `src/sim/world.cpp`
- Test: `tests/sim/world_test.cpp`

**Interfaces:**
- Produces: `std::optional<uint32_t> sim::resolveHitscan(const WorldSnapshot& view, uint32_t shooter, float aim_x, float aim_y);`

**Checkpoint 1: hitscan resolves against an arbitrary snapshot's positions**

- [ ] **Step 1: Write the failing test, then run it**

Spec — new `WorldTest.HitscanOverASnapshotUsesOnlyTheSnapshotsPositions`. Build a
`sim::WorldSnapshot` directly (no `World`), `tick = 0`, `count = 5`, every
`radius = kPlayerRadius`, velocities 0:

| id | x | y |
|---:|---:|---:|
| 1 | 0 | 0 |
| 2 | 5 | 0.4 |
| 3 | 3 | -0.6 |
| 4 | 5 | -0.3 |
| 5 | -4 | 0 |

Assertions:
- `resolveHitscan(view, 1, 1.0f, 0.0f) == 2u` — id 3 is nearer but 0.6 off the ray
  (outside 0.5); ids 2 and 4 tie at `t == 5`, both inside the radius, and the
  lower id wins.
- `resolveHitscan(view, 1, -1.0f, 0.0f) == 5u` — id 5 is behind for `+x`, ahead
  for `-x`.
- `resolveHitscan(view, 1, 0.0f, 1.0f) == std::nullopt` — nothing along `+y`.
- `resolveHitscan(view, 9, 1.0f, 0.0f) == std::nullopt` — shooter absent from the
  snapshot.
- `resolveHitscan(view, 1, 0.0f, 0.0f)`, and with `aim_x = quiet_NaN`, and with
  `aim_x = infinity` → all `std::nullopt`.
- A snapshot with `count = 1` (only the shooter) → `std::nullopt`.

Run: `scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain -R world_test --output-on-failure"`
Expected: FAIL — compile error, no matching `sim::resolveHitscan(const sim::WorldSnapshot&, …)`.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: the free function walks `view.players[0 .. view.count)` using the
**exact math of today's `World::resolveHitscan`**, moved rather than rewritten —
finite, non-zero `aim_len2` check; normalize; origin is the shooter's record;
skip the shooter; skip `t < 0`; skip `perp2 > kPlayerRadius²`; nearest `t` wins;
equal `t` resolves to the lower id. `World::resolveHitscan` becomes: return
`std::nullopt` if the shooter is absent, else `writeSnapshot` into a local
`WorldSnapshot` and return the free function's result. No allocation (the local
is on the stack). Every pre-existing `WorldTest.Hitscan*` test must pass
unmodified — that is the behavior-preservation proof, so do not edit them.

```bash
scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain -R world_test --output-on-failure" && \
  git add src/sim/world.h src/sim/world.cpp tests/sim/world_test.cpp && \
  git commit -m "refactor: resolve hitscan over a snapshot, World delegates"
```

Expected: PASS, then one commit.

- [ ] **Task boundary:** `scripts/tw bash scripts/ci.sh`. The two-binary
  determinism test (`determinism_two_binaries`) must stay green — `libsim`
  changed.

---

## Task 3: One sampling function for the client's render and the server's rewind

`Interpolator::sample` is the definition of "where the client drew player N at
tick T". The server must compute the same answer from the same kind of ring, so
the logic moves to a free function both sides call. This is the rewind-symmetry
invariant made structural.

**Files:**
- Modify: `src/net/snapshot_ring.h`, `src/net/snapshot_ring.cpp`,
  `src/client/interpolation.cpp`
- Test: `tests/net/snapshot_ring_test.cpp`

**Interfaces:**
- Produces: `bool net::samplePlayerAt(const SnapshotRing& ring, uint32_t player_id, uint32_t tick, float& x, float& y) noexcept;`

**Checkpoint 1: sampling a ring at an arbitrary tick reproduces the interpolation rules**

- [ ] **Step 1: Write the failing test, then run it**

Spec — new `SnapshotRingTest.SamplePlayerAtFollowsTheInterpolationRules`. Heap-
allocate the ring. Store two snapshots (radius `kPlayerRadius`, velocities 0):
- tick 100: id 2 at `(0, 0)`, id 4 at `(7, 7)`
- tick 104: id 2 at `(10, -4)`, id 3 at `(20, 20)`

Assertions (floats compared with `EXPECT_EQ` — these values are exact in binary32):
- id 2 at tick 102 → `true`, `(5, -2)` (bracketed lerp, α = 0.5).
- id 2 at tick 100 → `true`, `(0, 0)` exactly (α = 0 with a newer bracket).
- id 2 at tick 110 → `true`, `(10, -4)` (past the newest: frozen, never
  extrapolated).
- id 2 at tick 90 → `true`, `(0, 0)` (before the oldest: held at the oldest).
- id 3 at tick 102 → `true`, `(20, 20)` (joined mid-window: held at first known).
- id 4 at tick 102 → `false` (departed mid-window).
- id 4 at tick 110 → `false` (absent from the newest snapshot at-or-before).
- id 9 at tick 102 → `false`; any id on an empty ring → `false`.
- On every `false`, `x`/`y` are untouched (pre-set sentinels unchanged).

Run: `scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain -R 'snapshot_ring_test|interpolation_test' --output-on-failure"`
Expected: FAIL — compile error, `'samplePlayerAt' is not a member of 'net'`.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: `samplePlayerAt` is today's `Interpolator::sample` body **moved
verbatim** into `snapshot_ring.cpp` (including its file-local `findById`), with
`render_tick_` replaced by the `tick` parameter and the `have_` guard removed.
`Interpolator::sample` becomes `if (!have_) return false; return net::samplePlayerAt(ring, player_id, render_tick_, x, y);`.
Keep the existing comments (Decision 5, joined/departed mid-window) with the code
they describe. All five existing `InterpolatorTest` tests must pass unmodified.

```bash
scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain -R 'snapshot_ring_test|interpolation_test' --output-on-failure" && \
  git add src/net/snapshot_ring.h src/net/snapshot_ring.cpp src/client/interpolation.cpp tests/net/snapshot_ring_test.cpp && \
  git commit -m "refactor: share snapshot sampling between render and rewind"
```

Expected: PASS, then one commit.

- [ ] **Task boundary:** `scripts/tw bash scripts/ci.sh`.

---

## Task 4: `buildRewoundView` — the world as the shooter saw it

A pure function over state the server already has. It is where every rule about
*when a rewind is allowed* lives, so `Server` only has to ask and fall back.

**`kMaxRewindTicks = 45` is derived, not chosen.** When pass 2 of tick `F` runs,
the server's `history_` holds `kSnapshotRingSlots` (16) snapshots spaced
`kSnapshotIntervalTicks` (3) apart, the newest at a tick ≥ `F − 3`. Its oldest
entry is therefore at tick ≥ `F − 48` and at most `F − 46`. A view tick no more
than 45 behind `F` is always at or after the oldest entry, so the bracketing
snapshot the client sampled is guaranteed to still be in the server's ring.
Beyond that, the server could silently sample a *different* bracket than the
client did — so it refuses instead. 45 ticks is 750 ms. Expected real depth at
the 200 ms demo is ~24 ticks (lead 3 + RTT ~12 + snapshot wait ≤ 3 + interpolation
delay 6); the `tw_client` slider's 500 ms maximum lands near ~40, so the headroom
at the top of the slider is thin — Task 7 measures the real depth at 200 ms.

**Files:**
- Create: `src/server/rewind.h`, `src/server/rewind.cpp`,
  `tests/server/rewind_test.cpp`
- Modify: `CMakeLists.txt` (`src/server/rewind.cpp` into `libserver`;
  `tw_add_test(rewind_test tests/server/rewind_test.cpp)`)
- Modify (Checkpoint 3): `src/server/session.h`, `src/server/session.cpp`,
  `tests/server/session_test.cpp`

**Interfaces:**
- Consumes: `sim::World::writeSnapshot`, `net::samplePlayerAt` (Task 3),
  `net::SnapshotRing::newestAtOrBefore`, `SessionTable::ackedSnapshotTick`.
- Produces: `server::kMaxRewindTicks`, `server::RewindRequest`,
  `server::buildRewoundView`, `SessionTable::joinedTick` (all per the Interface
  summary).

**Shared fixture for this task's tests** (spell it out in the test file once):
- `sim::World live` with player 1 at `(-5, 0)`, player 2 at `(20, 0)`, player 3
  at `(0, 30)`.
- Heap `net::SnapshotRing history` storing, in order: tick 90 with id 1 at
  `(-10, 0)` and id 2 at `(0, 0)`; tick 93 with id 1 at `(-10, 0)` and id 2 at
  `(3, -3)`. Id 3 appears in neither.
- `SessionTable sessions`: `joinOrGet(kEp1, 0) == 1`, `joinOrGet(kEp2, 0) == 2`,
  `joinOrGet(kEp3, 0) == 3`, then `noteSnapshotAck(kEp1, 93)`.

**Checkpoint 1: targets sit where the shooter drew them; the shooter sits where it is**

- [ ] **Step 1: Write the failing test, then run it**

Spec — `RewindTest.PlacesTargetsAtTheSampledPositionAndTheShooterLive`: request
`{.shooter = 1, .view_tick = 91, .fire_tick = 100}` → returns `true`;
`out.tick == 91`; `out.count == 2`; the record with id 1 has `x == -5`, `y == 0`
(live, **not** `-10` from history); the record with id 2 has `x`/`y` bit-equal
(`memcpy`-compared) to what `net::samplePlayerAt(history, 2, 91, x, y)` returns;
no record has id 3 (live, but nothing in history to sample — the client could not
have drawn it). Records are located by id; their order is unspecified. Every
record's `radius` is `kPlayerRadius`.

Also register `rewind_test` in `CMakeLists.txt` in this step.

Run: `scripts/tw bash -c "cmake -S . -B build/plain && cmake --build build/plain -j8 && ctest --test-dir build/plain -R rewind_test --output-on-failure"`
Expected: FAIL — compile error, `server/rewind.h: No such file or directory`.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: `buildRewoundView` returns `false` without touching `out` when the
shooter is not in `live`. Otherwise it assembles a local `WorldSnapshot`
(`tick = req.view_tick`): the shooter's live record copied as-is, then for each
other live player whose `net::samplePlayerAt(history, id, req.view_tick, x, y)`
succeeds, that player's live record with `x`/`y` replaced by the sample. Assign
`out` once, at the end, and return `true` — including when there are zero
targets (a valid rewind with nothing to hit). No validation of the request beyond
shooter presence at this checkpoint. `kMaxRewindTicks` is declared with the
derivation above as its comment.

```bash
scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain -R rewind_test --output-on-failure" && \
  git add src/server/rewind.h src/server/rewind.cpp tests/server/rewind_test.cpp CMakeLists.txt && \
  git commit -m "feat: build the rewound view a shot resolves against"
```

Expected: PASS, then one commit.

**Checkpoint 2: an implausible view tick is refused**

- [ ] **Step 1: Write the failing test, then run it**

Spec — `RewindTest.RefusesImplausibleViewTicks`, same fixture. Before each call
set `out.tick = 0xDEADBEEF`; each of these returns `false` and leaves
`out.tick == 0xDEADBEEF`:
- `{1, 0, 100}` — view tick 0 means "uncompensated".
- `{1, 94, 100}` — beyond the shooter's acknowledged snapshot (93): it claims to
  have drawn a snapshot it never acknowledged holding.
- `{1, 93, 139}` — 46 ticks behind the fire tick, past `kMaxRewindTicks`.
- `{1, 93, 93}` — view tick not strictly before the fire tick.
- `{7, 91, 100}` — shooter not live.

And the boundary is inclusive: `{1, 93, 138}` (exactly 45 behind) returns `true`.

Run: `scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain -R rewind_test --output-on-failure"`
Expected: FAIL — the first four cases return `true`.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: before building, refuse when `req.view_tick == 0`, or
`req.view_tick >= req.fire_tick`, or `req.fire_tick - req.view_tick > kMaxRewindTicks`,
or `req.view_tick > sessions.ackedSnapshotTick(req.shooter)`. Each refusal is a
comment-documented rule (the second-to-last cannot underflow because of the one
before it). Refusal never touches `out`.

```bash
scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain -R rewind_test --output-on-failure" && \
  git add src/server/rewind.h src/server/rewind.cpp tests/server/rewind_test.cpp && \
  git commit -m "feat: refuse rewinds the shooter could not have seen"
```

Expected: PASS, then one commit.

**Checkpoint 3: a reused player id is never rewound into its previous occupant**

`SessionTable::joinOrGet` hands out the lowest free id, so after a leave, id 2's
history entries may describe *someone else*. Sampling a bracket that predates the
current occupant's session would place the new player at the old player's
position — and credit a hit on the new player for a shot at a ghost.

- [ ] **Step 1: Write the failing test, then run it**

Spec:
- `tests/server/session_test.cpp`, new `SessionTableTest.JoinedTickRecordsTheFirstJoinOnly`:
  `joinOrGet(kEpA, 50)` returns id `n`; `joinedTick(n) == 50`; a retransmitted
  `joinOrGet(kEpA, 80)` returns `n` and `joinedTick(n)` stays `50`;
  `joinedTick(0) == 0` and `joinedTick(31) == 0` (unknown); after `remove(kEpA)`,
  `joinOrGet(kEpB, 120)` returns `n` again and `joinedTick(n) == 120`.
- `tests/server/rewind_test.cpp`, new `RewindTest.NeverRewindsAReusedIdIntoItsPreviousOccupant`,
  its own fixture: `live` has id 1 at `(-5, 0)` and id 2 at `(31, 0)`; `history`
  stores tick 90 (id 1 at `(-10, 0)`, id 2 at `(0, 0)` — the **previous**
  occupant) and tick 93 (id 1 at `(-10, 0)`, id 2 at `(30, 0)` — the new one);
  `sessions`: `joinOrGet(kEp1, 0) == 1`, `joinOrGet(kEp2, 91) == 2`,
  `noteSnapshotAck(kEp1, 93)`.
  - `{1, 92, 100}` → `true`, `out.count == 1`, no id-2 record (the bracket at-or-
    before 92 is tick 90, which is not after id 2's join at 91).
  - `{1, 93, 100}` → `true`, `out.count == 2`, id 2 at exactly `(30, 0)`.

Run: `scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain -R 'rewind_test|session_test' --output-on-failure"`
Expected: FAIL — compile error, `'class server::SessionTable' has no member named 'joinedTick'`.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: `SessionTable::Entry` gains `uint32_t joined_tick = 0;`, **assigned
explicitly** in the new-entry branch of `joinOrGet` (`e.joined_tick = now_tick;`)
— the existing comment there warns that unassigned fields rely on an implicit
reset; do not rely on it for this one. `joinedTick(id)` returns it, or 0 for an
unknown id. In `buildRewoundView`, a non-shooter target is skipped when
`history.newestAtOrBefore(req.view_tick)` is null or its `tick` is
`<= sessions.joinedTick(id)` — a bracket must post-date the occupant's session.
Comment the rule with the reasoning above.

```bash
scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain -R 'rewind_test|session_test' --output-on-failure" && \
  git add src/server/session.h src/server/session.cpp src/server/rewind.cpp tests/server/session_test.cpp tests/server/rewind_test.cpp && \
  git commit -m "fix: never rewind a reused player id into its previous occupant"
```

Expected: PASS, then one commit.

- [ ] **Task boundary:** `scripts/tw bash scripts/ci.sh`.

---

## Task 5: `Server` resolves shots against the rewound view

**Files:**
- Modify: `src/server/server.h`, `CMakeLists.txt`
  (`tw_add_test(server_lagcomp_test tests/server/server_lagcomp_test.cpp)`)
- Create: `tests/server/server_lagcomp_test.cpp`

**Interfaces:**
- Consumes: `server::buildRewoundView`, `server::RewindRequest`,
  `server::kMaxRewindTicks` (Task 4); `sim::resolveHitscan` (Task 2);
  `net::HitConfirm`, `net::encodeHitConfirm`, `net::kHitConfirmBytes`,
  `net::MsgType::kHitConfirm` (Task 1); `SessionTable::endpointFor`.
- Produces: `Server::shots`, `Server::rewoundShots`, `Server::rewindsRejected`
  (per the Interface summary).

**Test helpers for this file** (file-local, modeled on `server_test.cpp`'s):
`injectJoin(tp, from, seq)`, `injectLeave(tp, from)`, and
`injectShot(tp, from, player_id, input_tick, move_x, move_y, aim_x, aim_y, fire, ack_tick, view_tick)`
— the latter sets both the header's `ack_tick` and the payload's `view_tick`.

**Shared scenario ("the moving target")** for Checkpoints 1 and 2: shooter A
(endpoint `kEpA`, id 1, spawns `(-35, -35)`) and target B (`kEpB`, id 2, spawns
`(-25, -35)`) join; `tick(0)` (world tick 1). Then for input ticks 2 through 32:
inject B moving `move_y = 1` and A stationary with no fire and `ack_tick` = the
newest multiple of 3 that is `<= world tick` at injection; `ingest()`; `tick()`.
After the `tick()` that brings `worldTick()` to 9, record B's `(x, y)` from
`srv->world().writeSnapshot(...)` as `B9` — the same state `history_` stored at
tick 9. At input tick 33, A fires: aim `(B9.x − (−35), B9.y − (−35))`,
`ack_tick = 30`, and the arm's `view_tick`. B is then ~3 units above `B9`, well
outside the 0.5 radius of that ray.

**Checkpoint 1: a compensated shot hits where the target was drawn; an uncompensated one misses; an implausible view tick falls back**

- [ ] **Step 1: Write the failing test, then run it**

Spec — `ServerLagCompTest.CompensatedShotHitsWhereTheTargetWasDrawn`, three arms,
each a fresh `Server` (heap) and transport:
- `view_tick = 9` → after tick 33: `hits(1) == 1`, `shots(1) == 1`,
  `rewoundShots() == 1`, `rewindsRejected() == 0`.
- `view_tick = 0` → `hits(1) == 0`, `shots(1) == 1`, `rewoundShots() == 0`,
  `rewindsRejected() == 0`.
- `view_tick = 31` (beyond the acknowledged 30) with the aim instead pointed at
  B's **live** position (read from `srv->world()` just before firing) →
  `hits(1) == 1`, `shots(1) == 1`, `rewoundShots() == 0`, `rewindsRejected() == 1`:
  a refused rewind resolves exactly like an uncompensated shot, it does not void
  the shot.

Register `server_lagcomp_test` in `CMakeLists.txt` in this step.

Run: `scripts/tw bash -c "cmake -S . -B build/plain && cmake --build build/plain -j8 && ctest --test-dir build/plain -R server_lagcomp_test --output-on-failure"`
Expected: FAIL — compile error, `'class server::Server<…>' has no member named 'shots'`.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract — pass 2 of `Server::tick()`, per fire candidate `in` that passes
`sessions_.tryFire(in.player_id, next)`:
1. `++shots_[in.player_id - 1]`.
2. If `in.view_tick != 0` and `buildRewoundView(world_, history_, sessions_, {in.player_id, in.view_tick, next}, rewound)`
   succeeds (`rewound` a local `sim::WorldSnapshot`): `++rewound_shots_`, resolve
   with `sim::resolveHitscan(rewound, in.player_id, in.aim_x, in.aim_y)`.
3. Otherwise: if `in.view_tick != 0`, `++rewinds_rejected_`; resolve with
   `world_.resolveHitscan(...)` exactly as today.
4. A resolved target → `++hits_[in.player_id - 1]`.

New members `std::array<uint64_t, sim::kMaxPlayers> shots_{}`,
`uint64_t rewound_shots_ = 0`, `uint64_t rewinds_rejected_ = 0`; accessors
`shots(id)` (same out-of-range guard as `hits`), `rewoundShots()`,
`rewindsRejected()`. Add
`static_assert(kMaxRewindTicks <= (net::kSnapshotRingSlots - 1) * kSnapshotIntervalTicks)`
beside `kSnapshotIntervalTicks`, so a future change to either constant cannot
silently break Task 4's derivation. Update pass 2's comment to say what it tests
the ray against and why the shooter is never rewound.

```bash
scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain -R 'server_lagcomp_test|server_test' --output-on-failure" && \
  git add src/server/server.h tests/server/server_lagcomp_test.cpp CMakeLists.txt && \
  git commit -m "feat: resolve shots against the shooter's rewound view"
```

Expected: PASS (including every existing `ServerTest` — an uncompensated shot's
path is unchanged), then one commit.

**Checkpoint 2: a hit is confirmed to the shooter, and only the shooter**

- [ ] **Step 1: Write the failing test, then run it**

Spec — `ServerLagCompTest.HitIsConfirmedToTheShooterOnly`, the moving-target
scenario. Call `tp->clearSent()` immediately before A's tick-33 shot is ingested.
After `tick()`:
- `view_tick = 9` arm: exactly one sent packet with header type `kHitConfirm`,
  addressed to `kEpA`; its payload decodes to `target_id == 2`,
  `fire_tick == 33`; its header `tick == 33` (the tick the hit resolved on). No
  `kHitConfirm` addressed to `kEpB`.
- `view_tick = 0` arm (a miss): zero `kHitConfirm` packets.

Run: `scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain -R server_lagcomp_test --output-on-failure"`
Expected: FAIL — no `kHitConfirm` packet was sent.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: on a resolved hit, look up the shooter's endpoint with
`sessions_.endpointFor(in.player_id, ep)` and send a framed `kHitConfirm`
(`tick = next`, `send_time_ms = now_ms`, payload
`{target, in.tick}`) through the existing `sendFramed`. An encode failure or a
missing endpoint counts in `dropped_`, like every other send site. `tick()`
already has `now_ms`.

```bash
scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain -R server_lagcomp_test --output-on-failure" && \
  git add src/server/server.h tests/server/server_lagcomp_test.cpp && \
  git commit -m "feat: confirm hits to the shooter"
```

Expected: PASS, then one commit.

**Checkpoint 3: shot and hit counts do not survive id reuse**

Closes the note P3's security review left open (*"`hits_` is also id-indexed and
never reset on removal"*). P6 makes it client-visible — confirmed hits now reach a
HUD — and adds `shots_` with the same indexing, so it is fixed here rather than
duplicated.

- [ ] **Step 1: Write the failing test, then run it**

Spec — `ServerLagCompTest.ShotAndHitCountsResetWhenAnIdIsReused`: A (`kEpA`, id 1)
and B (`kEpB`, id 2) join; `tick(0)`. A fires along `+x` at input tick 2 with
`view_tick = 0` (B is directly on that line, per the spawn grid) →
`hits(1) == 1`, `shots(1) == 1`. A sends `Leave`; `ingest(); tick()`. A new
endpoint `kEpC` joins; `ingest(); tick()`; `playerFor(kEpC) == 1`. Then
`hits(1) == 0` and `shots(1) == 0`.

Run: `scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain -R server_lagcomp_test --output-on-failure"`
Expected: FAIL — `hits(1)` is still 1.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: zero `hits_[id - 1]` and `shots_[id - 1]` at both removal sites
(`handleLeave` and the expiry loop in `tick()`), beside the existing
`inputs_[…].reset()` and under the same comment's reasoning. Only the leave path
is tested, for the reason P3 recorded for `inputs_`: expiry needs 300 silent ticks
and exercises the identical line.

```bash
scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain -R server_lagcomp_test --output-on-failure" && \
  git add src/server/server.h tests/server/server_lagcomp_test.cpp && \
  git commit -m "fix: reset per-id shot and hit counts on removal"
```

Expected: PASS, then one commit.

- [ ] **Task boundary:** `scripts/tw bash scripts/ci.sh`. TSan matters here:
  `threaded_runner_test` drives this `Server` across two threads, and every new
  member must stay sim-thread-only.

---

## Task 6: `Client` stamps what it drew and hears about hits

**Files:**
- Modify: `src/client/client.h`, `CMakeLists.txt`
  (`tw_add_test(client_lagcomp_test tests/client/client_lagcomp_test.cpp)`)
- Create: `tests/client/client_lagcomp_test.cpp`

**Interfaces:**
- Consumes: `sim::InputCommand::view_tick`, `net::HitConfirm`,
  `net::decodeHitConfirm`, `net::MsgType::kHitConfirm` (Task 1).
- Produces: `Client::setLagCompensationEnabled`, `lagCompensationEnabled`,
  `hitsConfirmed`, `lastHitTarget`, `lastHitFireTick`.

**Test helpers for this file** (file-local, modeled on `client_test.cpp`'s):
`injectJoinAccept`, `injectSnapshot`, `injectHitConfirm(tp, from, target_id, fire_tick)`,
and `lastSentInput(tp, out)` — decodes the most recent sent packet's header
(asserting type `kInput`) and payload.

**Checkpoint 1: every input names the tick its sender drew**

- [ ] **Step 1: Write the failing test, then run it**

Spec — `ClientLagCompTest.InputCarriesTheTickTheClientDrew`. Heap
`RecordingTransport`. Setup identical to `ClientTest.RemotePlayerIsInterpolatedBetweenSnapshots`
(join as id 1; snapshot tick 100 with ids 1 and 2; snapshot tick 104 moving id 2;
tick until `renderTick() == 102`). Then:
- `lagCompensationEnabled()` is `true` by default. `sendInput(...)` → the sent
  input's `view_tick == 102` (interpolating: the render tick).
- `setInterpolationEnabled(false)`; send → `view_tick == 104` (drawing the raw
  newest snapshot). `setInterpolationEnabled(true)`.
- `setLagCompensationEnabled(false)`; `lagCompensationEnabled()` is `false`; send
  → `view_tick == 0`. `setLagCompensationEnabled(true)`.
- Tick with no further snapshots until `renderTick() > 104` (bounded loop, assert
  it got there); send → `view_tick == 104` (starved: drawn frozen at the newest,
  so that is what was seen).
- A second, fresh client that has joined but received no snapshot: send →
  `view_tick == 0`.

Register `client_lagcomp_test` in `CMakeLists.txt` in this step.

Run: `scripts/tw bash -c "cmake -S . -B build/plain && cmake --build build/plain -j8 && ctest --test-dir build/plain -R client_lagcomp_test --output-on-failure"`
Expected: FAIL — compile error, `'class client::Client<…>' has no member named 'lagCompensationEnabled'`.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: `sendInput` sets `in.view_tick` to `0` when lag compensation is off, or
no snapshot is held (`latest_snapshot_tick_ == 0`), or interpolation is on but
`interp_.haveTimeline()` is false; otherwise `latest_snapshot_tick_` when
interpolation is off, else `std::min(interp_.renderTick(), latest_snapshot_tick_)`.
Comment it as "the tick whose world `remotePosition` drew this frame", and why
the starved case clamps. The toggle is a pure flag (`lag_compensation_enabled_ = true`),
matching the prediction and interpolation toggles' style and comments. The value
recorded into `pending_` carries the same `view_tick` (it is the same `in`).

```bash
scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain -R 'client_lagcomp_test|client_test' --output-on-failure" && \
  git add src/client/client.h tests/client/client_lagcomp_test.cpp CMakeLists.txt && \
  git commit -m "feat: stamp each input with the tick the client drew"
```

Expected: PASS, then one commit.

**Checkpoint 2: a hit confirmation from the server is counted; anything else is ignored**

- [ ] **Step 1: Write the failing test, then run it**

Spec — `ClientLagCompTest.CountsHitConfirmationsFromTheServerOnly`: joined client.
- Inject `kHitConfirm {target 2, fire_tick 777}` from `kServerEp`; `tick()` →
  `hitsConfirmed() == 1`, `lastHitTarget() == 2`, `lastHitFireTick() == 777`.
- Inject a valid `kHitConfirm {5, 900}` from a different endpoint; `tick()` →
  all three unchanged.
- Inject a `kHitConfirm` from `kServerEp` whose payload has `target_id == 0`, and
  one whose payload is 7 bytes; `tick()` → all three unchanged.
- A fresh, never-joined (`kIdle`) client given a valid confirmation from
  `kServerEp` → `hitsConfirmed() == 0`.

Run: `scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain -R client_lagcomp_test --output-on-failure"`
Expected: FAIL — compile error, no member `hitsConfirmed`.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: `handlePacket` gains a `kHitConfirm` case (after the existing
`slot.peer != server_` guard, which already covers the foreign-source case).
Ignore unless `state_ == State::kJoined`; decode with `net::decodeHitConfirm`;
on success increment `hits_confirmed_` and store `last_hit_target_`/
`last_hit_fire_tick_`. HUD-only state: nothing here feeds prediction, clock sync,
or anything sent.

```bash
scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain -R client_lagcomp_test --output-on-failure" && \
  git add src/client/client.h tests/client/client_lagcomp_test.cpp && \
  git commit -m "feat: count hit confirmations on the client"
```

Expected: PASS, then one commit.

- [ ] **Task boundary:** `scripts/tw bash scripts/ci.sh`.

---

## Task 7: Measure it — hit rate at 200 ms RTT, on vs off

The design doc's demo claim as a number. This is a **measurement task**, the same
kind as P5's Task 11: after Tasks 1–6 the behavior already exists, so this test is
**expected to pass the first time it runs.** That is deliberate, not a broken
RED step. If it fails, **that is a finding** — a real divergence between what the
client drew and what the server rewound to — to be investigated and recorded,
never fixed by loosening a threshold.

**Files:**
- Create: `tests/client/lagcomp_hitrate_test.cpp`
- Modify: `CMakeLists.txt` (`tw_add_test(lagcomp_hitrate_test tests/client/lagcomp_hitrate_test.cpp)`)

**Checkpoint 1: shots aimed at the drawn target register with compensation and mostly miss without**

- [ ] **Step 1: Write the measurement**

Spec — `LagCompHitRateTest.ShotsAtTheDrawnTargetRegisterOnlyWithCompensation`.
Harness copied from `ConvergenceTest`'s real-latency test
(`tests/client/convergence_test.cpp`): three real `UdpTransport`s on loopback,
each wrapped in a `SimulatedTransport` with `latency_ms = 100`, `jitter_ms = 10`,
`loss_permille = 0`, `seed = 7` (200 ms round trip), everything heap-allocated,
the same per-iteration order (`srv->ingest(); srv->tick(now_ms); server_tp->advanceTick();`
then each client's `tick` and `advanceTick`), `now_ms = i * 16`.

A helper `ArmResult runArm(bool lag_comp)` builds a fresh server, a **shooter**
client, and a **target** client, calls `shooter->setLagCompensationEnabled(lag_comp)`,
and runs:
- **Settle, 700 iterations:** both send inputs every tick with no fire. The
  shooter moves `move_y = 1` for its first 60 joined ticks (8 units up, so the
  target's vertical sweep crosses the shooter's row mid-swing rather than at a
  turnaround), then stays still. The target stays still.
- **Shoot, 900 iterations:** the target sends `move_y = ((i / 120) % 2 == 0) ? 1 : -1`
  (a 16-unit vertical sweep reversing every 2 s, clear of the arena clamp). Each
  tick the shooter reads `localPosition` and `remotePosition(target->playerId(), …)`;
  when both succeed it computes the aim with `client::aimFromCursor(local, drawn)`
  and sends `fire = true`. Also track
  `max_depth = max(clientTick() − min(renderTick(), latestSnapshotTick()))`
  over those ticks.

`ArmResult` holds `shots = srv->shots(shooter id)`, `hits = srv->hits(shooter id)`,
`rewound = srv->rewoundShots()`, `rejected = srv->rewindsRejected()`, `max_depth`.
Print one line per arm with `std::printf`:
`lagcomp=<on|off> shots=<n> hits=<n> hit_rate=<0.000> rewound=<n> rejected=<n> max_depth_ticks=<n>`.

Assertions:
- both arms: both clients `kJoined`; `shots >= 60` (900 ticks ÷ the 12-tick
  cooldown is 75).
- on: `hits / shots >= 0.90`, `rejected == 0`, `rewound == shots`,
  `max_depth < server::kMaxRewindTicks`.
- off: `rewound == 0`, `hits / shots <= 0.50`.
- `on_rate − off_rate >= 0.50`.

- [ ] **Step 2: Run, record the numbers, commit**

```bash
scripts/tw bash -c "cmake -S . -B build/plain && cmake --build build/plain -j8 && ctest --test-dir build/plain -R lagcomp_hitrate_test --output-on-failure && build/plain/lagcomp_hitrate_test --gtest_brief=1" && \
  git add tests/client/lagcomp_hitrate_test.cpp CMakeLists.txt && \
  git commit -m "test: measure hit rate with and without lag compensation"
```

The direct binary run is what shows the two `lagcomp=` lines (`ctest` hides a
passing test's stdout). Copy both lines, verbatim, into the commit message body
and keep them for Task 10. Expected: PASS, then one commit.

- [ ] **Task boundary:** `scripts/tw bash scripts/ci.sh`. The test must be green
  under ASan and TSan too. TSan slows the run down but the loop runs in lockstep
  on the tick index, not the wall clock, so the numbers should not move. If they
  do move, that is a finding.

---

## Task 8: Demo and operator surface

**Files:**
- Modify: `apps/tw_server.cpp`, `apps/tw_client.cpp`, `CMakeLists.txt`

**Checkpoint 1: `tw_server` reports its rewind counters**

- [ ] **Step 1: Write the failing test, then run it**

Spec — register in `CMakeLists.txt`:
`add_test(NAME server_app_lagcomp_counters COMMAND tw_server --port 0 --ticks 5)`
with `PASS_REGULAR_EXPRESSION "rewound_shots=0 rewinds_rejected=0"`.

Run: `scripts/tw bash -c "cmake -S . -B build/plain && cmake --build build/plain -j8 && ctest --test-dir build/plain -R server_app_lagcomp_counters --output-on-failure"`
Expected: FAIL — `Required regular expression not found`. (Check it says that,
not `No tests were found`. The second means the registration didn't take, and it
exits 0.)

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: both summary sites in `apps/tw_server.cpp` (the single-threaded path and
`runThreaded`) print
`lagcomp rewound_shots=<n> rewinds_rejected=<n>` right after the existing
`packets_ingested=` line, formatted like the neighbouring `printf`s.

```bash
scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain -R server_app --output-on-failure" && \
  git add apps/tw_server.cpp CMakeLists.txt && \
  git commit -m "feat: report rewind counters from tw_server"
```

Expected: every `server_app_*` test passes, then one commit.

**Checkpoint 2: `tw_client` toggles compensation and shows hits**

GUI drawing has no test surface. The P2–P4 precedent applies: build it, run the
display self-test, and leave the visual check for a human. No RED step.

- [ ] **Step 1: Implement**

Contract, in `apps/tw_client.cpp`:
- `L` toggles `setLagCompensationEnabled`, next to the `P`/`I` handlers.
- The HUD adds `lagcomp=<on|off> hits=<hitsConfirmed()>`.
- When `hitsConfirmed()` increases, draw a white ring (radius about 1.6× the
  player radius, in screen units) around `lastHitTarget()`'s *drawn* position,
  from `remotePosition`, for the next 12 frames.
- While the fire button is held, draw a thin line from the local player along the
  aim vector to the arena edge, so a player can see a shot "look like" a hit.
- Update `printUsage`'s key list if it has one. Keep the file thin: drawing and
  input only, no logic.

- [ ] **Step 2: Verify and commit**

```bash
scripts/tw bash -c "cmake -S . -B build/gui -DTW_BUILD_GUI=ON && cmake --build build/gui -j8 && ctest --test-dir build/gui -R client_selftest --output-on-failure" && \
  git add apps/tw_client.cpp && \
  git commit -m "feat: lag compensation toggle and hit feedback in tw_client"
```

Expected: `client_selftest` passes. It exits 0 with a display, or 77 without one,
which CTest treats as a skip. Then one commit.

- [ ] **Task boundary:** `scripts/tw bash scripts/ci.sh`.

---

## Task 9: Mandatory security review

**Not optional.** `CLAUDE.md`: *"Any phase touching the network surface →
`security-review` skill / `security-reviewer` agent — **mandatory**."* P6 changes a
payload layout, adds a message type, and lets an attacker-supplied field
(`view_tick`) steer server-side computation. P1 through P5 each ran this review at
their boundary, and P6 does the same.

**Files:**
- Modify: `docs/project-history.md` (findings, under the P6 section)
- Plus whatever the findings require.

**Process:**

- [ ] Run the `security-reviewer` and `cpp-reviewer` agents **in parallel** over
  `src/server/rewind.{h,cpp}`, the P6 diffs to `src/server/server.h`,
  `src/server/session.{h,cpp}`, `src/client/client.h`,
  `src/net/{protocol,framing,snapshot_ring}.{h,cpp}`, `src/sim/{sim.h,world.h,world.cpp}`,
  and `apps/tw_{server,client}.cpp`.

- [ ] Threat model, unchanged from P1 through P5: an unauthenticated attacker controls
  every byte of every datagram, can send them at any rate, and can forge any
  source address the network permits.

- [ ] Questions this phase specifically raises. Answer each one explicitly in the
  write-up:
  - **The attacker picks the rewind.** `view_tick` is attacker-controlled. Within
    `[fire_tick − kMaxRewindTicks, acked]`, a cheater can choose whichever past
    moment is most favorable. That is inherent to lag compensation; Valve's
    `sv_maxunlag` exists for the same reason. Confirm that no path lets a view tick
    escape those bounds, that a refused rewind grants nothing an uncompensated shot
    does not, and record the bounded advantage as an accepted residual risk.
  - **CPU per shot.** `buildRewoundView` does a sample per live target, and each
    sample is a linear scan over 16 snapshots. Bound the worst case, all 32 sessions
    firing on the same tick, in operations, and state whether `kFireCooldownTicks`
    makes it attacker-amplifiable.
  - **`kHitConfirm` and amplification.** At most one per hit, only to the
    authorized shooter's endpoint, and rate-bounded by the fire cooldown. Quantify
    the bytes per second it adds to P2's recorded join/snapshot amplification
    finding for a spoofed-source session.
  - **Id reuse.** Verify both removal sites reset `hits_`/`shots_`. Verify
    `joined_tick` is assigned explicitly for every new session. Verify the
    bracket rule in `buildRewoundView` holds when a leave and a join of the same id
    happen on the same tick.
  - **Non-finite floats.** Rewound positions come from the server's own `history_`,
    so they are finite by construction. Confirm the only attacker floats reaching
    `sim::resolveHitscan` are `aim_x`/`aim_y`, which it already checks.
  - **Downgrade.** Confirm a version-2 packet is rejected before any payload
    decode, and that no code path accepts a 25-byte input.
  - **Client.** A forged `kHitConfirm` from a non-server source is dropped by the
    P3 guard. One forged from the server's spoofed address only moves HUD counters.
    Confirm nothing derived from them is ever sent or fed back into the simulation.
  - **Inherited from P4, render-only.** The client's `Interpolator` has no
    session-join knowledge, so after id reuse it can briefly lerp between two
    different occupants. The server no longer credits a hit on that ghost (Task 4
    Checkpoint 3). The on-screen artifact stays. Record it as observed, not fixed.
  - Are the three P2 CRITICAL findings still exactly as recorded? Re-check them
    against this phase's actual diff, not from memory.

- [ ] Fix every CRITICAL and HIGH before proceeding. Each fix gets its own RED→GREEN
  commit with a regression test, or an explicit note of why it can't be tested at
  runtime.

- [ ] Record every finding in `docs/project-history.md` under
  `### Task 9 boundary — mandatory security review findings`, following the
  structure P1–P5 established. That includes fixed findings, deferred ones, and
  ones verified closed.

```bash
scripts/tw bash scripts/ci.sh && \
  git add docs/project-history.md && \
  git commit -m "docs: record the P6 security review findings"
```

---

## Task 10: Writeup and final verification

**Files:**
- Modify: `README.md`, `docs/project-history.md`, `CLAUDE.md`
- Create: `journal/<date>_<HHMM>_ansh_p6-execution.md`

**Checkpoint 1: `README.md` tells the lag compensation story with its number**

- [ ] **Step 1: Write it**

Contract:
- "What works today" becomes "through P6". The wire protocol bullet says version 3,
  amended once at P2 and once at P6, and names what each amendment added. Add a lag
  compensation bullet.
- A "Lag compensation" subsection under "Measured results". It gives both arms'
  hit rates from Task 7's recorded lines verbatim, plus the measured max rewind
  depth. Provenance: `lagcomp_hitrate_test`, 100 ms + 10 ms jitter per leg, seed 7,
  pinned image. Add one sentence on why the uncompensated arm is not zero.
- "Run the demo" gains the lag compensation recipe. Start `tw_server`, then
  `tw_loadclient --players 1 --ticks 36000` as a moving target (it sweeps along x),
  then `tw_client --latency-ms 200`. Move your player a few units above or below
  the target's row, shoot at it, and press `L` to compare.
- "What's next" becomes P7.
- Per resolution doc § Q1, never say "lockstep" or "cross-platform". The numbers
  describe one machine and one image.

- [ ] **Step 2: Verify and commit**

```bash
scripts/tw bash scripts/ci.sh && \
  git add README.md && \
  git commit -m "docs: add lag compensation results and demo to the README"
```

**Checkpoint 2: history, conventions, and the journal**

- [ ] **Step 1: Write them**

Contract:
- `docs/project-history.md`: append to the existing P6 section, which already
  holds the planning-time wire-format decision. Do not overwrite it. Add:
  - the exact-reproduction design: shared `samplePlayerAt` over snapshot history,
    as opposed to per-tick authoritative history, and why;
  - `kMaxRewindTicks`'s derivation;
  - the refusal-falls-back-to-uncompensated decision;
  - the id-reuse bracket rule;
  - Task 7's measured numbers;
  - any robustness seed change from Task 1;
  - anything execution turned up that this plan did not anticipate.
- `CLAUDE.md`:
  - Add `src/server/rewind.{h,cpp}` to the `src/server/` file-structure row.
  - Add a "Verified constraints" entry for rewind symmetry, but only if execution
    confirmed it. The server's rewound positions and the client's rendered remote
    positions both come from `net::samplePlayerAt`, and a server-side
    reimplementation reintroduces cross-path divergence. Name the test that proves
    it.
- `journal/`: a session entry via the `journal` skill.

- [ ] **Step 2: Final full verification, then commit**

```bash
scripts/tw bash scripts/ci.sh && \
  scripts/tw bash -c "cmake -S . -B build/gui -DTW_BUILD_GUI=ON && cmake --build build/gui -j8 && ctest --test-dir build/gui -R client_selftest --output-on-failure" && \
  git add docs/project-history.md CLAUDE.md journal/ && \
  git commit -m "docs: journal the P6 execution session"
```

Expected: `ci.sh` green across plain/ASan/TSan plus the toolchain assertions, and
`client_selftest` passes. Then one commit.

**Outstanding for a human, not blocking:** the visual demo check. P2–P4 recorded
the same limitation: no session tool can watch a WSLg window. Run the README
recipe at 200 ms. Shots at the moving bot should register with `lagcomp=on` and
mostly not with `lagcomp=off`. Record the result in `docs/project-history.md`
afterward, the way P3 and P4 recorded theirs.

**The branch is now green and verified. Stop here.** `executing-plans` hands off
to `finishing-a-development-branch`, which owns the merge decision. Do not merge,
push, or open a PR from within this plan.

---

## Self-Review

**1. Spec coverage.** The design doc names lag compensation via server-side rewind
(Tasks 2–5) and the demo claim that "toggle lag compensation and shots that looked
like hits start registering". The toggle is Task 6 Checkpoint 1 and Task 8
Checkpoint 2. The "start registering" part is measured in Task 7 and made visible
by `kHitConfirm` (Task 1 Checkpoint 2, Task 5 Checkpoint 2, Task 6 Checkpoint 2,
Task 8 Checkpoint 2). P2 deferred client-visible hit feedback to P6, and it is
here. P3 left `hits_`-on-id-reuse as a follow-up, and Task 5 Checkpoint 3 closes
it. The mandatory network-surface security review is Task 9. P6 adds nothing to
the design doc's four headline benchmark rows. It adds its own measured row
(Task 7), because a netcode claim with no number is the one thing this project
refuses to ship.

**Two design calls worth knowing before executing:**
- **Exact reproduction over per-tick history.** Valve's reference design records
  authoritative positions every tick and interpolates between them. This plan
  instead samples the same 20 Hz snapshots the client rendered, through the same
  function. The client never saw per-tick positions: it saw a lerp between
  snapshots. So rewinding to authoritative per-tick state would place a target
  where it *truly was*, not where it was *drawn*. Those differ whenever velocity
  changed inside a 3-tick snapshot interval. Sampling the snapshots makes the
  server's answer bit-identical to the client's whenever both hold the same
  bracketing snapshots, and it needs no new storage.
  **The residual gap:** if the client lost or dropped the bracketing snapshot
  (under loss, or reordering, since the client discards a late snapshot), it lerped
  across a wider gap than the server does. Task 7's jitter exercises exactly this.
- **A refused rewind falls back to uncompensated resolution rather than voiding the
  shot.** It is the least surprising outcome, and it gives an attacker nothing an
  honest client with compensation off does not already get.

**2. Placeholder scan.** No "TBD", no "handle edge cases", no "similar to Task N".
Every checkpoint names exact inputs and expected values: the fixture tables in
Tasks 2–4, `view_tick == 102/104/0`, the `02 00 00 00 D2 04 00 00` golden bytes,
`{1, 93, 139}` refused versus `{1, 93, 138}` accepted, and `hits(1) == 1`,
`rewoundShots() == 1`. The moving-target scenario is specified once and reused by
name within its task.

**3. Type consistency.** Checked across tasks:
- `samplePlayerAt(ring, player_id, tick, x, y)` is spelled identically in Tasks 3,
  4 and 10.
- `buildRewoundView(live, history, sessions, req, out)` with
  `RewindRequest{shooter, view_tick, fire_tick}` is the same in Tasks 4 and 5.
- `shots`/`rewoundShots`/`rewindsRejected` are the same in Tasks 5, 7 and 8, and
  the `tw_server` output keys `rewound_shots=`/`rewinds_rejected=` match Task 8's
  regex.
- `HitConfirm{target_id, fire_tick}` is the same in Tasks 1, 5 and 6.
- `hitsConfirmed`/`lastHitTarget`/`lastHitFireTick` are the same in Tasks 6 and 8.
- `kMaxRewindTicks` appears in Tasks 4, 5 (the `static_assert`), 7 and 9.

**4. Checkpoint falsifiability.** Every checkpoint names an assertion at a public
surface that fails before and passes after, with three deliberate, flagged
exceptions:
- **Task 7** is a measurement task that is expected to pass. It says so in place,
  as P5's Task 11 did.
- **Task 8 Checkpoint 2** is GUI drawing with no test surface, per the P2–P4
  precedent.
- **Tasks 2 and 3** are refactors, but each introduces a new public function whose
  absence is the compile-error RED. Their behavior-preservation proof is the
  pre-existing suites (`WorldTest.Hitscan*`, `InterpolatorTest.*`) passing
  unmodified, which each contract forbids editing.

Two checkpoints were restructured while writing, so an executor will not have to
discover the problem:
- A separate "refused rewind falls back" checkpoint in Task 5 would have passed
  immediately, because Task 4 already refuses. It was folded into Task 5
  Checkpoint 1 as a third arm.
- Task 4 Checkpoint 1's fixture sets `noteSnapshotAck(kEp1, 93)` from the start.
  Otherwise Checkpoint 2's acknowledged-tick rule would turn Checkpoint 1's
  already-committed test red.

---

## Execution Handoff

The plan is written spec-driven for inline execution: behaviors are specified
precisely and the executor writes the code. Everything a fresh session needs is in
this document plus the auto-loaded `CLAUDE.md`, and the Global Constraints carry
the build commands, container rules, language limits, the verified
designated-initializer warning, and test scoping verbatim. If tasks are delegated
to cold subagents instead (`delegating-plan-tasks`), switch those tasks to the
code-heavy template first, as the `writing-plans` skill requires.
