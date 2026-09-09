# P3 — Client Prediction, Server Reconciliation, and Clock Sync Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use the `executing-plans` skill to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the local player respond to the keyboard instantly at 200 ms of
simulated latency, while the server stays authoritative — a client that runs its
own `sim::World` ahead of the server, sends inputs stamped for the server tick
they are meant to drive, and reconciles against every snapshot by adopting the
authoritative state and replaying whatever the server has not yet acknowledged.

**Architecture:** Three pieces, in dependency order. (1) The **server consumes
one input per player per tick**, dequeued from a per-session tick-keyed
`InputBuffer` at the tick the input was stamped for — replacing P2's
apply-on-arrival latch, which integrated an input for a jitter-dependent number
of ticks the client could never reproduce. (2) The **client's clock runs ahead**
of the server by a controlled margin, driven by a feedback signal already on the
wire: `ack_tick − tick` on an incoming snapshot *is* the depth of that session's
server-side input buffer, so a pure `ClockSync` controller drives it to a target
without ever measuring RTT explicitly. (3) The **client predicts and
reconciles**: it holds a `sim::World` containing only the local player, applies
each input locally the moment it is sent, and on every snapshot overwrites the
local player with the authoritative state and replays the pending inputs stamped
after the snapshot's tick.

**The wire format does not change.** P1 reserved `tick`, `send_time_ms` and
`ack_tick`; P2 populated them; P3 is the phase that reads them back. That is the
whole point of having reserved them, and it is verified below in "What P3 does
not change."

**Tech Stack:** C++20 (GCC 10.5.0), CMake 3.28.4, GoogleTest, raylib via
`FetchContent` (GUI build only), `std::span`, concepts.

**Spec:**
- [`docs/specs/2026-09-04-tickwire-design.md`](../specs/2026-09-04-tickwire-design.md) — the design; the P3 row, § Architecture (points 5–7), § `libsim`, § The demo, and the "P3 is the death phase" passage
- [`docs/specs/2026-09-04-architecture-resolution.md`](../specs/2026-09-04-architecture-resolution.md) — **authoritative**; Q1 (floats, and the determinism-test trigger for reverting to fixed-point), Q2 (`libsim` surface), Q3 (transport shape)
- [`docs/wire-format.md`](../wire-format.md) — the version 2 format. **P3 changes no byte layout**; Task 10 only rewrites the "reserved for P3" annotations to say what now consumes each field
- [`docs/project-history.md`](../project-history.md) — P0/P1/P2 decisions and findings, including the three CRITICAL security findings deferred at P2 and the reasoning for deferring them
- [`CLAUDE.md`](../../CLAUDE.md) — project conventions, auto-loaded

**References read before planning, per the design doc's instruction to read them
before P3 rather than during:** Gabriel Gambetta's *Client-Side Prediction and
Server Reconciliation* (the pending-input buffer, the acknowledged-sequence
discard, and the replay loop), Glenn Fiedler's *Snapshot Interpolation* (the
jitter-buffer argument, which is P4's, not P3's), and the standard
command-frame/tick-synchronization treatment of a client running ahead by
roughly half the RTT with the server feeding back whether it is early or late.
Valve's *Source Multiplayer Networking* page returned HTTP 403 and was not read;
nothing in this plan depends on it, and lag compensation — the part it is the
canonical reference for — is P6's scope, not P3's.

**Plan format:** spec-driven (inline execution), same as P1 and P2.

**Suggested branch:** `phase-3-prediction-reconciliation`

---

## Global Constraints

Everything in `CLAUDE.md` applies. Restated here because a cold executing window
must not have to infer any of it, plus the constraints P3 adds.

**Carried from `CLAUDE.md` (P0/P1/P2-verified):**

- **Never invoke the host `g++`, `cmake`, or `ctest`.** All build/test commands go through `scripts/tw`, which runs them inside the pinned `tickwire-dev:gcc10-cmake3.28.4-x11` image with the repo bind-mounted at `/work`.
- **TSan runs are wrapped in `setarch -R`**; the capability flags in `scripts/tw` and `setarch -R` are both required, neither alone is sufficient.
- **Float flags on every target linking `libsim`:** `-march=x86-64`, `-ffp-contract=off`. Never `-march=native`, never `-ffast-math`, never `-march=x86-64-v2` (absent in GCC 10). These propagate through `tickwire_sim_flags`; new source files need nothing added by hand as long as they join an existing target.
- **No `std::format`** (GCC 10 lacks it) and **no `std::bit_cast`** (libstdc++ ships it from GCC 11). Pun `float`↔`uint32_t` with `std::memcpy`. Format output with `std::ostream` or `std::fprintf`; do not add fmtlib.
- **`libsim` has no I/O, no wall-clock reads, and no allocation.** Only trivially-copyable POD crosses its boundary.
- **Protocol fields are explicitly little-endian**, written byte by byte through `net::ByteWriter`/`net::ByteReader`.
- **Decoders are strict:** reject rather than normalize or clamp.
- **`Server::tick(uint32_t now_ms)` and `Client::tick(uint32_t now_ms)` take the current monotonic millisecond as a parameter and never read a clock.** `server::monotonicMs()` stays the only `clock_gettime` in the project, called only from `apps/`.
- **Large objects are heap-allocated in tests** via `std::make_unique` — `Server` (~330 KB), `Client` (now larger, see below), `LoopbackTransport` (~310 KB), `SimulatedTransport` (~157 KB), `RecordingTransport` (~1.24 MB). Never a stack local.
- **`-Wall -Wextra -Werror`.**
- **Commits:** `type: description`. One commit per checkpoint, chained behind its test with `&&` — never `;`, never a separate line. `git add` names exact paths; **never** `git add -A` or `git add .`. Never `git commit --amend` — a follow-up commit instead.
- **Test scoping:** inside a checkpoint use `-R <regex>`; the full suite only at a task boundary.
- **Coverage:** 80% project-wide; **`libsim` is held to 100%** — Task 1 adds the phase's only `libsim` branches and must cover every one.
- **File size:** 200–400 lines typical, 800 hard maximum. `src/client/client.h` is 171 lines today and grows substantially this phase; if it passes ~400, split the prediction/reconciliation members into `src/client/prediction.h` rather than letting it sprawl.

**Added by P3:**

- **The wire format is frozen and P3 does not amend it.** No byte layout, no `kProtocolVersion` bump, no new `MsgType`. If a task appears to need one, that is a finding to record and raise — not a change to make quietly. See "What P3 does not change" below for the verification that nothing needs one.
- **One input per player per tick, consumed at the tick it was stamped for.** This is the invariant the whole phase rests on: it is what makes the client's replay reproduce the server's steps exactly. A server that consumes two inputs in one tick, or the same input twice, breaks reconciliation silently.
- **Underrun repeats, it does not zero.** When a session has no input stamped for the tick being simulated, the server applies nothing — `World::step()` then integrates the velocity already stored, which *is* repeat-last-input. The client's replay mirrors this exactly by skipping `applyInput` for a tick it holds no input for. These two behaviors must stay symmetrical; changing one without the other reintroduces divergence.
- **The client predicts the local player only.** Remote players are rendered straight from the snapshot. Predicting them would require predicting their inputs, which nothing can do; smoothing them is entity interpolation, which is P4's scope. `Client`'s prediction world therefore holds exactly one player.
- **Prediction is a toggle, off-path by default in tests that do not want it.** `Client::setPredictionEnabled(false)` restores P2's exact rendering behavior (draw the snapshot), which is what the demo's "before" state needs and what keeps the P2 client tests meaningful.
- **No benchmark harness.** P3 records prediction error and prints it; it does not build a sweep driver, emit CSV, or produce the headline numbers table. That is P5's scope, and it inherits this instrumentation.

---

## What P3 does not change

Stated explicitly because the temptation to reopen these is the main way this
phase grows past its scope.

| Not changed | Why it does not need to be |
|---|---|
| **The wire format** | Reconciliation needs an acknowledgment of "which inputs has the server consumed" — the snapshot's existing `tick` field is exactly that, because the server consumes inputs strictly in tick order (Task 3), so a snapshot at tick `S` has consumed every input stamped `≤ S`. Clock sync needs a feedback signal — `ack_tick − tick` on the same snapshot is the session's server-side input-buffer depth, from two fields P2 already populates. RTT needs an echo — `ack_tick` names an input the client still holds the send time for. Three needs, zero new fields. |
| **`sim::World::step()` / `applyInput()`** | The velocity-latch behavior that P2 built is precisely the underrun-repeat semantics P3 needs. `step()` reads `players_[i].vx/vy`, which persist until the next `applyInput`. Task 1 adds one new method and touches no existing one. |
| **`SessionTable`** | `touch()` already raises `last_input_tick` monotonically (`if (input_tick > e.last_input_tick)`), which is exactly the `ack_tick` semantics clock sync wants. Passing `0` as `input_tick` updates liveness without touching the ack — which is how Task 3 records a session as alive after an input its buffer rejected. No signature changes. |
| **`SessionTable`'s size** | The per-session `InputBuffer` (~450 B each, ~14 KB for 32) lives in `Server`, **not** in `SessionTable`. `SessionTable` is 900 B and `tests/server/session_test.cpp` stack-allocates it in 9 tests — a P2 review looked at that and judged it safe at 900 B. Growing it to ~15 KB would invalidate that judgment and force churn in all 9. `Server` is already heap-allocated everywhere. |
| **`PacketRing`, the epoll/timerfd loop, `TickTimer`, `PollSet`** | P3 changes what the server does with a packet, not how it receives one. |

**One finding to record, not fix:** `sim::World::latched_` is dead state. It is
written by `addPlayer` and `applyInput` and never read — `step()` integrates
`players_[i].vx/vy` instead. It is harmless, and removing it is a
behavior-preserving cleanup that belongs to `/refactor-clean`, not to the phase
the design doc calls the one most likely to kill the project. Record it in
`docs/project-history.md` at Task 10; do not delete it here. Its deadness is
also *why* Task 1's `setPlayerState` is sufficient for reconciliation: restoring
`vx`/`vy` from the snapshot fully restores the server's motion state, with no
hidden latch to also reconcile.

---

## File Structure

| File | Responsibility |
|---|---|
| `src/sim/world.h`, `.cpp` | Gains `setPlayerState` — the reconciliation primitive. The phase's only `libsim` change. |
| `src/server/input_buffer.h`, `.cpp` | `InputBuffer` — per-session tick-keyed input storage with an acceptance window. Pure; no I/O, no clock. |
| `src/server/server.h` | `Server<T>` buffers inputs on arrival and consumes one per session per tick, in two passes (apply, then resolve fires). |
| `src/client/clock_sync.h`, `.cpp` | `ClockSync` — the pure controller that turns an observed input-buffer depth into a per-tick clock correction. |
| `src/client/prediction.h`, `.cpp` | `PendingInputs` (the unacknowledged-input store, indexed by tick) and `PredictionStats` (the error reservoir). Both pure. |
| `src/client/client.h` | `Client<T>` gains the prediction world, the clock, reconciliation, the RTT estimate, and the prediction toggle. |
| `apps/tw_loadclient.cpp` | Prints RTT, clock lead, and prediction-error percentiles at exit. |
| `apps/tw_client.cpp` | Renders the local player from the predicted position; `P` toggles prediction; HUD gains RTT/lead/error. |
| `tests/sim/world_test.cpp` | `setPlayerState` — success and every rejection branch (100% floor). |
| `tests/server/input_buffer_test.cpp` | Store/retrieve, the acceptance window, the consumption floor. |
| `tests/server/server_test.cpp` | Tick-matched consumption, underrun repeat, two-pass fire ordering — plus the repaired P2 tests. |
| `tests/client/clock_sync_test.cpp` | Correction sign, the no-acknowledgment guard, the snap threshold. |
| `tests/client/prediction_test.cpp` | `PendingInputs` store/evict; `PredictionStats` percentiles. |
| `tests/client/client_test.cpp` | Clock seeding and correction, local prediction, reconciliation replay, RTT, error recording. |
| `tests/client/convergence_test.cpp` | Client-vs-server convergence over loopback and over `SimulatedTransport` with latency and jitter. |

**Two shippable halves, same structure P2 used.** Tasks 1–8 produce a green,
fully tested headless system. Tasks 9–10 add the apps and the docs. Task 9's GUI
half is the only part this project cannot verify automatically — the windows
render through WSLg onto the host desktop — so it is deliberately last, exactly
as P2 placed its display risk last.

---

## Task 1: `World::setPlayerState` — the reconciliation primitive

Reconciliation has to overwrite the local player with the server's authoritative
state, velocity included. `addPlayer` cannot do it (it zeroes velocity and
refuses an id already present), and remove-then-add would lose the velocity that
drives the very next step. This is the phase's only `libsim` change.

**Files:**
- Modify: `src/sim/world.h`, `src/sim/world.cpp`, `tests/sim/world_test.cpp`

**Interfaces:**
- Consumes: `sim::PlayerState`, `sim::kInvalidPlayerId`, `World::findSlot` (existing private helper).
- Produces, in `namespace sim` (`src/sim/world.h`), as a public member of `World`:

```cpp
// Overwrites an existing player's position, velocity and radius from `s`,
// keyed by s.id. False when s.id is kInvalidPlayerId, is not present, or
// any of x/y/vx/vy/radius is non-finite. The stored state is untouched
// on false -- assigned only once every check has passed.
bool setPlayerState(const PlayerState& s);
```

**Checkpoint 1: an overwrite takes effect, including velocity**

- [ ] **Step 1: Write the failing test, then run it**

Spec, in `tests/sim/world_test.cpp` as `TEST(WorldTest, SetPlayerStateOverwritesPositionAndVelocity)`:
`World w; w.addPlayer(1, 0.0f, 0.0f)`. Then
`w.setPlayerState(PlayerState{1, 10.0f, -10.0f, sim::kMoveSpeed, 0.0f, sim::kPlayerRadius})`
returns `true`. `writeSnapshot` then reports player 1 at `x == 10.0f`,
`y == -10.0f`, `vx == sim::kMoveSpeed`, `vy == 0.0f`. After one `w.step()`,
player 1 is at `x == 10.0f + sim::kMoveSpeed * sim::kTickDt` — proving the
velocity was restored, not just the position.

Run: `scripts/tw ctest --test-dir build/plain -R world_test --output-on-failure`
Expected: FAIL — compile error, `'class sim::World' has no member named 'setPlayerState'`.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: `bool World::setPlayerState(const PlayerState& s)` — locate the slot
via `findSlot(s.id)`; return `false` if it is negative. Return `false` if any of
`s.x`, `s.y`, `s.vx`, `s.vy`, `s.radius` fails `std::isfinite`. Otherwise assign
`players_[slot] = s` and return `true`. `findSlot` already rejects
`kInvalidPlayerId`, so no separate check for it is needed. Do not touch
`latched_`, `occupied_`, `count_` or `tick_`.

```bash
scripts/tw ctest --test-dir build/plain -R world_test --output-on-failure && \
  git add src/sim/world.h src/sim/world.cpp tests/sim/world_test.cpp && \
  git commit -m "feat: let World overwrite a player's authoritative state"
```

Expected: PASS, then one commit.

**Checkpoint 2: every rejection leaves the stored state untouched**

- [ ] **Step 1: Write the failing test, then run it**

Spec, as `TEST(WorldTest, SetPlayerStateRejectsUnknownIdsAndNonFiniteFields)`:
`World w; w.addPlayer(7, 3.0f, 4.0f)`. Assert each of the following returns
`false` **and** leaves player 7 at exactly `x == 3.0f`, `y == 4.0f`,
`vx == 0.0f`, `vy == 0.0f`:
- an absent id: `PlayerState{8, 1, 1, 1, 1, kPlayerRadius}`
- `kInvalidPlayerId`: `PlayerState{0, 1, 1, 1, 1, kPlayerRadius}`
- non-finite `x`: `PlayerState{7, NAN, 1, 1, 1, kPlayerRadius}`
- non-finite `y`: `PlayerState{7, 1, INFINITY, 1, 1, kPlayerRadius}`
- non-finite `vx`: `PlayerState{7, 1, 1, NAN, 1, kPlayerRadius}`
- non-finite `vy`: `PlayerState{7, 1, 1, 1, -INFINITY, kPlayerRadius}`
- non-finite `radius`: `PlayerState{7, 1, 1, 1, 1, NAN}`

Use `std::nanf("")` and `std::numeric_limits<float>::infinity()` rather than the
`NAN`/`INFINITY` macros, to stay consistent with the existing suite.

Run: `scripts/tw ctest --test-dir build/plain -R world_test --output-on-failure`
Expected: FAIL — **on the five non-finite cases only**. The absent-id and
`kInvalidPlayerId` cases already pass from Checkpoint 1's `findSlot` guard; that
is expected and is not a reason to alter the test. The finiteness branch does not
exist yet, so those five assertions see `true` returned and the stored state
overwritten with garbage.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: add the `std::isfinite` guard over all five float fields to
`setPlayerState`, evaluated *before* the assignment to `players_[slot]`, so a
rejected call cannot partially populate. `<cmath>` is already included by
`world.cpp`.

```bash
scripts/tw ctest --test-dir build/plain -R world_test --output-on-failure && \
  git add src/sim/world.cpp tests/sim/world_test.cpp && \
  git commit -m "fix: reject non-finite fields in World::setPlayerState"
```

Expected: PASS, then one commit.

**Task boundary:** run the full suite in the plain configuration, confirming
`libsim` still holds at 100% on its new branches.

```bash
scripts/tw ctest --test-dir build/plain --output-on-failure
```

---

## Task 2: `InputBuffer` — per-session, tick-keyed input storage

A fixed-capacity store that holds inputs against the tick they were stamped for
and hands back exactly the one belonging to the tick being simulated. Pure: no
I/O, no clock, no allocation. Indexed by `tick & kInputBufferMask`, so lookup is
O(1) and the acceptance window is what prevents two ticks aliasing onto one slot.

**Files:**
- Create: `src/server/input_buffer.h`, `src/server/input_buffer.cpp`, `tests/server/input_buffer_test.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `sim::InputCommand` (existing).
- Produces, in `namespace server` (`src/server/input_buffer.h`):

```cpp
inline constexpr size_t kInputBufferSlots = 16;  // power of two; 266 ms of input at 60 Hz
inline constexpr uint32_t kInputBufferMask = kInputBufferSlots - 1;

class InputBuffer {
 public:
  // Accepts `in` only when its tick falls in the open-closed acceptance
  // window (lastConsumed, lastConsumed + kInputBufferSlots]. Rejects a tick
  // of 0, a tick already simulated past, and a tick so far ahead it would
  // alias onto a slot the window still owns.
  bool push(const sim::InputCommand& in) noexcept;

  // Writes the input stamped exactly `tick` into `out` and returns true.
  // Returns false on an underrun -- no input is held for that tick. Either
  // way the consumption floor advances to `tick` and the slot is cleared,
  // so a client that goes silent does not freeze the acceptance window.
  bool takeFor(uint32_t tick, sim::InputCommand& out) noexcept;

  uint32_t highestTick() const noexcept;  // highest accepted tick; 0 when none
  uint32_t lastConsumed() const noexcept; // the consumption floor; 0 initially
  void reset() noexcept;                  // clears every slot and both counters

 private:
  std::array<sim::InputCommand, kInputBufferSlots> slots_{};
  std::array<bool, kInputBufferSlots> filled_{};
  uint32_t highest_tick_ = 0;
  uint32_t last_consumed_ = 0;
};
```

Add `src/server/input_buffer.cpp` to `libserver`'s source list in
`CMakeLists.txt`, and register the test with
`tw_add_test(input_buffer_test tests/server/input_buffer_test.cpp)` alongside the
other `tests/server/` entries.

**Checkpoint 1: an input comes back at the tick it was stamped for, and only then**

- [ ] **Step 1: Write the failing test, then run it**

Spec, in `tests/server/input_buffer_test.cpp` as
`TEST(InputBufferTest, TakeForReturnsTheInputStampedForThatTick)`:
`InputBuffer b;` push `InputCommand{.player_id = 1, .tick = 5, .move_x = 1.0f}`
— returns `true`. `sim::InputCommand out{};` then:
- `b.takeFor(4, out)` returns `false` (nothing stamped 4) and leaves `out` zeroed.
- `b.takeFor(5, out)` returns `true` and `out.tick == 5`, `out.move_x == 1.0f`, `out.player_id == 1`.
- a second `b.takeFor(5, out)` returns `false` — the slot was consumed.

Run: `scripts/tw ctest --test-dir build/plain -R input_buffer_test --output-on-failure`
Expected: FAIL — compile error, `input_buffer.h: No such file or directory`.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: `push` writes `slots_[in.tick & kInputBufferMask] = in` and sets the
matching `filled_` flag, returning `true` unconditionally for now (the window
lands in Checkpoint 3). `takeFor(tick, out)` computes `i = tick & kInputBufferMask`,
treats it as a hit only when `filled_[i] && slots_[i].tick == tick`, copies to
`out` on a hit, then clears `filled_[i]` and returns whether it hit. The
`slots_[i].tick == tick` comparison — not `filled_[i]` alone — is what makes a
stale aliased slot read as an underrun rather than as someone else's input.

```bash
scripts/tw ctest --test-dir build/plain -R input_buffer_test --output-on-failure && \
  git add src/server/input_buffer.h src/server/input_buffer.cpp \
          tests/server/input_buffer_test.cpp CMakeLists.txt && \
  git commit -m "feat: store inputs against the tick they are stamped for"
```

Expected: PASS, then one commit.

**Checkpoint 2: `highestTick` reports the newest input accepted**

This is the value the server hands to `SessionTable::touch`, which becomes the
`ack_tick` a snapshot carries — the client's entire clock-sync signal. It is
worth its own checkpoint because it is the one output of this class that leaves
the server.

- [ ] **Step 1: Write the failing test, then run it**

Spec, as `TEST(InputBufferTest, HighestTickTracksTheNewestAcceptedInput)`:
a fresh `InputBuffer` reports `highestTick() == 0`. After pushing ticks 3, then
7, then 5 (out of order, all inside the window), `highestTick() == 7` — it is a
running maximum, not "the last one pushed". Consuming tick 7 via `takeFor` leaves
`highestTick() == 7`; it records what has been *received*, not what remains.

Run: `scripts/tw ctest --test-dir build/plain -R input_buffer_test --output-on-failure`
Expected: FAIL — compile error, `'class server::InputBuffer' has no member named 'highestTick'`.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: `push` sets `highest_tick_ = in.tick` when `in.tick > highest_tick_`,
after the acceptance decision. `highestTick()` returns it. `takeFor` does not
touch it.

```bash
scripts/tw ctest --test-dir build/plain -R input_buffer_test --output-on-failure && \
  git add src/server/input_buffer.h src/server/input_buffer.cpp \
          tests/server/input_buffer_test.cpp && \
  git commit -m "feat: track the newest input tick an InputBuffer has accepted"
```

Expected: PASS, then one commit.

**Checkpoint 3: the acceptance window rejects late and far-future inputs**

- [ ] **Step 1: Write the failing test, then run it**

Spec, as `TEST(InputBufferTest, PushRejectsOutsideTheAcceptanceWindow)`.
On a fresh `InputBuffer` (floor 0), assert:
- `push` with `tick == 0` returns `false` — 0 is `World`'s pre-first-step value and never names a simulated tick — and leaves `highestTick() == 0`.
- `push` with `tick == kInputBufferSlots` (16) returns `true` — the window's inclusive upper edge.
- `push` with `tick == kInputBufferSlots + 1` (17) returns `false`, and leaves `highestTick() == 16` — a rejected push must not advance the high-water mark, or the server would advertise an `ack_tick` for an input it discarded.

Then drive the floor forward: `sim::InputCommand out{};` call `b.takeFor(10, out)`.
`lastConsumed() == 10`. Assert:
- `push` with `tick == 10` returns `false` (at the floor, already simulated).
- `push` with `tick == 9` returns `false` (below the floor).
- `push` with `tick == 11` returns `true`.

Finally, prove the floor advances on an underrun too — a client that goes silent
must not freeze the window: on a fresh `InputBuffer`, call `b.takeFor(t, out)`
for `t = 1..40` (every one an underrun, all returning `false`), then
`push` with `tick == 45` returns `true`. Without a floor that advances on a miss,
45 would sit outside a window still anchored at 0 and be rejected.

Run: `scripts/tw ctest --test-dir build/plain -R input_buffer_test --output-on-failure`
Expected: FAIL — `push` currently returns `true` unconditionally, so every
`EXPECT_FALSE` on a `push` fails, as does the `highestTick() == 16` assertion.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: `push` returns `false` unless
`in.tick > last_consumed_ && in.tick <= last_consumed_ + kInputBufferSlots`,
evaluated before it writes anything — a rejected push touches neither `slots_`,
`filled_`, nor `highest_tick_`. The window's width equals the ring's capacity,
which is what makes aliasing impossible by construction rather than by
arithmetic care at the call site. `takeFor` sets `last_consumed_ = tick`
unconditionally, hit or miss. `lastConsumed()` returns it. `reset()` zeroes
`filled_`, `highest_tick_` and `last_consumed_`.

```bash
scripts/tw ctest --test-dir build/plain -R input_buffer_test --output-on-failure && \
  git add src/server/input_buffer.h src/server/input_buffer.cpp \
          tests/server/input_buffer_test.cpp && \
  git commit -m "feat: bound InputBuffer's acceptance window to the ring capacity"
```

Expected: PASS, then one commit.

**Task boundary:** full suite, plain configuration.

```bash
scripts/tw ctest --test-dir build/plain --output-on-failure
```

---

## Task 3: the server consumes one input per player per tick

The behavioral heart of the phase, and the task that touches shipped P2 code.
`handleInput` stops applying and starts buffering; `tick()` gains a drain that
runs before `step()`.

**The tick a `Server::tick()` call simulates is `world_.tick() + 1`.** Routing
happens while the world still reads the previous tick, then `step()` advances it.
This is the same convention P2 already established for join replies
(`out_h.tick = world_.tick() + 1`, with the comment explaining why), so nothing
new is being invented — it is being applied to inputs as well.

**Two passes, deliberately.** Pass one applies every session's input for the
tick; pass two resolves every fire. A single pass that applied and fired per
session would resolve player 1's hitscan against player 2's *pre-input* position
for that tick, making hit detection depend on session iteration order. P2 had
the same ambiguity implicitly (fires resolved in packet arrival order); making
the passes explicit removes it.

**Files:**
- Modify: `src/server/server.h`, `tests/server/server_test.cpp`

**Interfaces:**
- Consumes: `server::InputBuffer` (Task 2), `SessionTable::touch/authorize/count/playerAt` (existing, unchanged), `sim::World::applyInput/step/resolveHitscan` (existing, unchanged).
- Produces, on `Server<T>`:

```cpp
uint64_t inputUnderruns() const noexcept;  // ticks a live session had no input for
uint64_t lateInputs() const noexcept;      // inputs rejected by InputBuffer::push
```

New private member: `std::array<InputBuffer, sim::kMaxPlayers> inputs_{};`
indexed by `player_id - 1`. This lives on `Server`, not `SessionTable` — see
"What P3 does not change" for why.

**Checkpoint 1: an input applies at the tick it was stamped for, not on arrival**

- [ ] **Step 1: Write the failing test, then run it**

Spec, in `tests/server/server_test.cpp` as
`TEST(ServerTest, InputAppliesAtItsStampedTickNotOnArrival)`:
heap-allocate a `RecordingTransport` and a `Server<RecordingTransport>`.
`injectJoin(*tp, kEpA, 1); srv->ingest(); srv->tick(0);` — the world is now at
tick 1 and player 1 sits at its spawn, `x == -35.0f`.

Inject an input from `kEpA` claiming player 1 with `move_x = 1.0f` **stamped for
tick 3** (two ticks in the future), then `srv->ingest()`.

- `srv->tick(16)` simulates tick 2. Player 1 must be **unmoved**: `x == -35.0f` and `vx == 0.0f`. `inputUnderruns()` is 1 — the session had no input for tick 2.
- `srv->tick(32)` simulates tick 3. Now `vx == sim::kMoveSpeed` and `x == -35.0f + sim::kMoveSpeed * sim::kTickDt`.

Run: `scripts/tw ctest --test-dir build/plain -R server_test --output-on-failure`
Expected: FAIL — under P2's apply-on-arrival the input takes effect during the
`tick(16)` call, so `EXPECT_EQ(p1->x, -35.0f)` fails with
`-35 + 8 * 0.0166667` and `EXPECT_EQ(p1->vx, 0.0f)` fails with `8`.
`inputUnderruns()` also does not compile yet.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract, in `src/server/server.h`:

`handleInput(from, r)` — decode, then `authorize`, both exactly as today
(**the authorization gate does not move and is not weakened**; it is what closes
the P1-deferred spoofing finding). On success:
`sessions_.touch(from, world_.tick(), 0)` to record liveness, then
`inputs_[in.player_id - 1].push(in)`. If the push succeeds, call
`sessions_.touch(from, world_.tick(), in.tick)` to raise `last_input_tick`; if it
fails, `++late_inputs_`. Passing `0` as `input_tick` is a no-op for
`last_input_tick` because `touch` only raises it on a strict increase — so a
rejected input keeps the session alive without advertising an `ack_tick` the
server cannot honor. **Remove the fire/hitscan block from `handleInput`
entirely**; it moves to `tick()`.

`tick(now_ms)` — after the ring drain and before `world_.step()`:

```
const uint32_t next = world_.tick() + 1;
// Pass 1: apply every live session's input for `next`.
for i in [0, sessions_.count()):
    id = sessions_.playerAt(i)
    if inputs_[id - 1].takeFor(next, in):
        world_.applyInput(in)
        remember (id, in) as a fire candidate when in.fire
    else:
        ++input_underruns_
// Pass 2: resolve fires against the fully-updated pre-step positions.
for each remembered (id, in):
    if sessions_.tryFire(id, next) and world_.resolveHitscan(id, in.aim_x, in.aim_y):
        ++hits_[id - 1]
world_.step();
```

Hold the fire candidates in a `std::array<sim::InputCommand, sim::kMaxPlayers>`
plus a count, declared inside `tick()` — no allocation, and `Server` is already
heap-allocated at every call site. `tryFire` now takes `next` rather than
`world_.tick()`, so the cooldown is measured in simulated ticks.

`inputUnderruns()`/`lateInputs()` return the new counters.

**This checkpoint's commit must also repair the P2 tests that assumed
apply-on-arrival.** `injectInput` already takes a `tick` parameter defaulting to
`0`; every call site must now pass the tick the assertion expects the input to
land on, which is `world tick at the time of the following tick() call, plus 1`.
Affected tests: `InputsMoveOnlyThePlayerTheSenderOwns`,
`FiringHitsTheNearestPlayerAlongTheAimAndIsRateLimited`,
`SnapshotsGoOutAt20HzToEveryLiveSession`, and
`SilentSessionsTimeOutAndTheFreedIdIsReusable`. Change the stamped ticks and
nothing else — the *assertions* about spoof rejection, drop counts, snapshot
cadence and timeouts all still hold and must not be relaxed. Change
`injectInput`'s default from `tick = 0` to no default, so a missed call site is a
compile error rather than a silently-rejected input.

```bash
scripts/tw ctest --test-dir build/plain -R server_test --output-on-failure && \
  git add src/server/server.h tests/server/server_test.cpp && \
  git commit -m "feat: consume one input per player at the tick it was stamped for"
```

Expected: PASS, then one commit.

**Checkpoint 2: an underrun repeats the last velocity rather than stopping the player**

- [ ] **Step 1: Write the failing test, then run it**

Spec, as `TEST(ServerTest, MissingInputRepeatsTheLastVelocity)`:
join `kEpA`, `srv->tick(0)` (world at tick 1). Inject an input stamped tick 2
with `move_x = 1.0f`, ingest, `srv->tick(16)` — player 1 now has
`vx == sim::kMoveSpeed` at `x == -35.0f + sim::kMoveSpeed * sim::kTickDt`.

Now call `srv->tick(32)` and `srv->tick(48)` with no input injected. After both,
`vx` is still `sim::kMoveSpeed` and
`x == -35.0f + 3.0f * sim::kMoveSpeed * sim::kTickDt` — the player kept moving.
`inputUnderruns()` increased by exactly 2 across those two calls.

Then inject an input stamped tick 5 with `move_x = 0.0f`, ingest, `srv->tick(64)`:
`vx == 0.0f` and `x` unchanged from the previous assertion — an explicit stop is
honored, so "repeat" is genuinely the underrun path and not a stuck velocity.

Run: `scripts/tw ctest --test-dir build/plain -R server_test --output-on-failure`
Expected: FAIL — on the underrun-count assertions. **The velocity-persistence
assertions already pass**, because `World::step()` integrates stored velocity and
Checkpoint 1 already skips `applyInput` on a miss. That is the intended design,
not an accident; this checkpoint pins it against a future change that "fixes" the
underrun by zeroing velocity, and adds the counter that makes an underrun
observable at all. Expect the failure to be
`inputUnderruns()` differing from the expected delta of 2.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: no behavior change is needed beyond confirming `input_underruns_` is
incremented once per live session per tick with no matching input, and never for
a session that had one. If the assertion delta is wrong, the bug is in
Checkpoint 1's counter placement — fix it there, in this commit.

```bash
scripts/tw ctest --test-dir build/plain -R server_test --output-on-failure && \
  git add src/server/server.h tests/server/server_test.cpp && \
  git commit -m "test: pin underrun-repeats-last-velocity and its counter"
```

Expected: PASS, then one commit.

**Checkpoint 3: fires resolve against positions after every input for the tick is applied**

- [ ] **Step 1: Write the failing test, then run it**

Spec, as `TEST(ServerTest, FireResolvesAfterEveryInputForThatTickIsApplied)`.
Construct the case so the shot lands only if the *target's* input for the same
tick has already been applied.

Join `kEpA` (player 1, spawns at `(-35, -35)`) and `kEpB` (player 2, spawns at
`(-25, -35)`), then `srv->tick(0)`; the world is at tick 1. Both spawn on the
same row, so player 1 aiming along `+x` already hits player 2 — that is not a
discriminating case. Instead, first move player 2 off the row and let it settle:
inject an input for player 2 stamped tick 2 with `move_y = 1.0f`, ingest,
`srv->tick(16)`. Player 2 is now at
`y == -35.0f + sim::kMoveSpeed * sim::kTickDt` (about 0.133 above the row) —
still within `kPlayerRadius` (0.5) of player 1's aim line along `+x`, so a shot
would connect.

Now, for tick 3, inject **both** in the same batch: player 2 stamped tick 3 with
`move_y = 1.0f` (continuing to climb), and player 1 stamped tick 3 with
`move_x = 0.0f`, `aim_x = 1.0f`, `aim_y = 0.0f`, `fire = true`. Ingest, then
`srv->tick(32)`.

Player 2's position when the fire resolves is `y == -35.0f + 2.0f * sim::kMoveSpeed * sim::kTickDt`
(about 0.267) — still inside the 0.5 radius, so `srv->hits(1) == 1`.

Make it discriminating by repeating the pattern until pass ordering changes the
answer: continue driving player 2 upward one tick at a time and firing on each
tick, and assert that the hit stops registering on the **first tick at which
player 2's post-input `y` exceeds `kPlayerRadius`** — that is, `hits(1)` stops
incrementing exactly one tick earlier than it would if fires resolved against
pre-input positions. Concretely: drive ticks 3 through 6 with player 2 climbing
and player 1 firing every tick (`kFireCooldownTicks` is 12, so only the tick-3
shot is not rate-limited — reset the comparison by firing once, at tick 3, then
again at tick 15 and 27, spacing shots past the cooldown while player 2 keeps
climbing). Assert `hits(1)` against the post-input geometry at each firing tick.

If constructing this proves fiddly at execution time, the reduced form is
sufficient and must still be written: **one firing tick, chosen so the target is
inside the radius after its input for that tick and outside it before** —
`player 2 at y == kPlayerRadius - epsilon` after the tick's input, having been at
`y > kPlayerRadius` before it, with player 1 firing on the same tick. Under a
single-pass implementation the shot misses; under two passes it hits. Record in
the journal which form was used.

Run: `scripts/tw ctest --test-dir build/plain -R server_test --output-on-failure`
Expected: FAIL if Checkpoint 1 was implemented as a single fused pass. If
Checkpoint 1 already implemented the two passes as specified, this test passes on
first write — in which case **do not modify it to force a failure**. Note that in
the journal, commit it as a regression pin with `test:` rather than `feat:`, and
move on. The ordering is a real invariant worth pinning either way; what it is
not is a second chance to write the implementation.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: `tick()` resolves every fire only after pass one has applied every live
session's input for `next`. If the test failed, split the fused loop as specified
in Checkpoint 1's contract.

```bash
scripts/tw ctest --test-dir build/plain -R server_test --output-on-failure && \
  git add src/server/server.h tests/server/server_test.cpp && \
  git commit -m "test: pin that fires resolve after all inputs for the tick"
```

Expected: PASS, then one commit.

**Task boundary:** the full suite in all three configurations — this task changed
shipped server behavior and is the most likely place in the phase to have broken
something under a sanitizer.

```bash
scripts/tw ctest --test-dir build/plain --output-on-failure && \
  scripts/tw ctest --test-dir build/asan --output-on-failure && \
  scripts/tw setarch -R ctest --test-dir build/tsan --output-on-failure
```

---

## Task 4: `ClockSync` — turning buffer depth into a clock correction

A pure controller. It never sees a packet, a clock, or a transport; it takes two
integers off an arriving snapshot and returns how many extra ticks the client
should advance (or skip) this frame.

**The signal.** On a snapshot, `ack_tick` is the highest input tick the server
has accepted from this session and `tick` is the tick it has just simulated. So
`ack_tick − tick` is how many ticks of input the server holds *ahead* of its
simulation. Drive that to `kTargetLeadTicks` and the client is running ahead by
exactly the right amount: enough that its input for tick `T` arrives before the
server simulates `T`, not so much that inputs sit stale in the buffer. No RTT
measurement enters the loop — the delay is already baked into the observation.

**Files:**
- Create: `src/client/clock_sync.h`, `src/client/clock_sync.cpp`, `tests/client/clock_sync_test.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: nothing but `<cstdint>`.
- Produces, in `namespace client` (`src/client/clock_sync.h`):

```cpp
inline constexpr int32_t kTargetLeadTicks = 3;      // input ticks the server should hold
inline constexpr int32_t kSnapErrorTicks = 12;      // |error| at or beyond this: correct at once
inline constexpr int32_t kMaxCorrectionTicks = 30;  // hard clamp on a single correction

class ClockSync {
 public:
  // Feed one accepted snapshot. `ack_tick == 0` means the server has
  // acknowledged no input from this session yet and is ignored entirely.
  void observe(uint32_t server_tick, uint32_t ack_tick) noexcept;

  // The correction to apply to the client's tick this frame, then cleared.
  // Nominal is 0; +1 nudges the clock forward, -1 holds it back, and a
  // larger magnitude is a snap. A second call before the next observe()
  // returns 0.
  int32_t takeCorrection() noexcept;

  bool haveEstimate() const noexcept;  // has any snapshot been observed?
  int32_t lead() const noexcept;       // last observed ack_tick - server_tick
  uint32_t snaps() const noexcept;     // corrections that crossed kSnapErrorTicks
};
```

Add `src/client/clock_sync.cpp` to `libclient` in `CMakeLists.txt` and register
`tw_add_test(clock_sync_test tests/client/clock_sync_test.cpp)`.

**Checkpoint 1: the correction's sign follows the lead error, and clears on read**

- [ ] **Step 1: Write the failing test, then run it**

Spec, in `tests/client/clock_sync_test.cpp` as
`TEST(ClockSyncTest, CorrectionFollowsTheLeadErrorAndClearsOnRead)`:
`ClockSync c;` — `c.haveEstimate()` is `false` and `c.takeCorrection() == 0`.

- `c.observe(100, 103)` — lead 3, exactly `kTargetLeadTicks`. `c.haveEstimate()` is `true`, `c.lead() == 3`, `c.takeCorrection() == 0`.
- `c.observe(100, 101)` — lead 1, two short. `c.takeCorrection() == 1` (advance an extra tick to get further ahead), and an immediate second `c.takeCorrection() == 0`.
- `c.observe(100, 105)` — lead 5, two long. `c.takeCorrection() == -1` (hold back a tick).

Run: `scripts/tw ctest --test-dir build/plain -R clock_sync_test --output-on-failure`
Expected: FAIL — compile error, `clock_sync.h: No such file or directory`.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: `observe` computes
`lead = int64(ack_tick) - int64(server_tick)` and
`error = lead - kTargetLeadTicks`, stores `lead_`, sets `have_ = true`, and sets
`pending_` to `0` when `error == 0`, `-1` when `error > 0`, `+1` when
`error < 0`. Compute in `int64_t` and narrow once — `ack_tick` and `server_tick`
are unsigned and their difference is routinely negative. `takeCorrection()`
returns `pending_` and assigns `pending_ = 0` before returning. Snapshots arrive
at 20 Hz, so a ±1 nudge per snapshot gives ±20 ticks per second of correction
authority — far more than clock drift needs, which is why no separate rate limit
is required.

```bash
scripts/tw ctest --test-dir build/plain -R clock_sync_test --output-on-failure && \
  git add src/client/clock_sync.h src/client/clock_sync.cpp \
          tests/client/clock_sync_test.cpp CMakeLists.txt && \
  git commit -m "feat: derive a clock correction from the server's input-buffer depth"
```

Expected: PASS, then one commit.

**Checkpoint 2: a snapshot acknowledging no input is ignored**

Without this guard, the first snapshot after a join — which carries
`ack_tick == 0` because no input has been accepted yet — reads as a lead of
`−server_tick`, i.e. thousands of ticks behind, and the client snaps its clock
into the far future on the strength of a non-observation.

- [ ] **Step 1: Write the failing test, then run it**

Spec, as `TEST(ClockSyncTest, SnapshotWithNoAcknowledgedInputIsIgnored)`:
`ClockSync c; c.observe(500, 0);` — `c.haveEstimate()` is **`false`**,
`c.takeCorrection() == 0`, `c.lead() == 0`, `c.snaps() == 0`. A following
`c.observe(500, 503)` then behaves normally: `haveEstimate()` is `true`,
`lead() == 3`, `takeCorrection() == 0`.

Run: `scripts/tw ctest --test-dir build/plain -R clock_sync_test --output-on-failure`
Expected: FAIL — `haveEstimate()` returns `true` and `lead()` returns `-500`,
and `takeCorrection()` returns `1` instead of `0`.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: `observe` returns immediately, changing nothing, when `ack_tick == 0`.

```bash
scripts/tw ctest --test-dir build/plain -R clock_sync_test --output-on-failure && \
  git add src/client/clock_sync.cpp tests/client/clock_sync_test.cpp && \
  git commit -m "fix: ignore a snapshot that acknowledges no input yet"
```

Expected: PASS, then one commit.

**Checkpoint 3: a large error corrects in one step, clamped**

- [ ] **Step 1: Write the failing test, then run it**

Spec, as `TEST(ClockSyncTest, LargeErrorsSnapAndAreClamped)`:
- `ClockSync c; c.observe(100, 120);` — lead 20, error +17, at or beyond `kSnapErrorTicks` (12). `c.takeCorrection() == -17` (the whole error at once) and `c.snaps() == 1`.
- `c.observe(100, 80);` — `ack_tick` below `server_tick`: lead −20, error −23. `c.takeCorrection() == 23` and `c.snaps() == 2`.
- `c.observe(1000, 900);` — lead −100, error −103, beyond the clamp. `c.takeCorrection() == kMaxCorrectionTicks` (30), not 103. `c.snaps() == 3`.
- `c.observe(100, 111);` — lead 11, error +8, below the snap threshold. `c.takeCorrection() == -1` and `c.snaps()` stays 3.

Run: `scripts/tw ctest --test-dir build/plain -R clock_sync_test --output-on-failure`
Expected: FAIL — every snap case returns `-1` or `+1` from Checkpoint 1's
sign-only logic, and `snaps()` does not compile.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: in `observe`, when `error >= kSnapErrorTicks || error <= -kSnapErrorTicks`,
set `pending_` to `-error` clamped to `[-kMaxCorrectionTicks, +kMaxCorrectionTicks]`
and `++snaps_`. Otherwise fall through to the sign-only nudge from Checkpoint 1.
The clamp matters: `ack_tick` and `server_tick` are attacker-influenced values off
the wire, and an unclamped correction would let a forged snapshot throw the
client's clock arbitrarily far. Bounding it means the worst a hostile snapshot
can do is cost 30 ticks, recovered within a snapshot or two.

```bash
scripts/tw ctest --test-dir build/plain -R clock_sync_test --output-on-failure && \
  git add src/client/clock_sync.h src/client/clock_sync.cpp \
          tests/client/clock_sync_test.cpp && \
  git commit -m "feat: snap the client clock on a large lead error, bounded"
```

Expected: PASS, then one commit.

**Task boundary:** full suite, plain configuration.

```bash
scripts/tw ctest --test-dir build/plain --output-on-failure
```

---

## Task 5: the client's clock runs ahead of the server

Wire `ClockSync` into `Client<T>`, replacing the free-running counter that P2
used to stamp inputs. From here on `Client::tick_` means *the server tick this
client is currently simulating*, not "how many times tick() has been called."

**Files:**
- Modify: `src/client/client.h`, `tests/client/client_test.cpp`

**Interfaces:**
- Consumes: `client::ClockSync`, `client::kTargetLeadTicks` (Task 4).
- Produces, on `Client<T>`:

```cpp
uint32_t clientTick() const noexcept;  // the server tick this client is simulating
int32_t clockLead() const noexcept;    // last observed server-side buffer depth
uint32_t clockSnaps() const noexcept;
```

**Checkpoint 1: joining seeds the clock from the server's reported tick**

- [ ] **Step 1: Write the failing test, then run it**

Spec, in `tests/client/client_test.cpp` as
`TEST(ClientTest, JoinAcceptSeedsTheClockAheadOfTheServer)`:
heap-allocate a `RecordingTransport` and a `Client<RecordingTransport>`.
`c->beginJoin(0);` then inject a `kJoinAccept` with `ack_seq == kJoinSeq`,
`h.tick == 500`, and a player id of 1. `c->tick(16);`

After the accept is handled, `c->clientTick() == 500 + kTargetLeadTicks` (503) —
the client places its clock ahead of the tick the server reported, so its first
input is stamped for a tick the server has not yet simulated. `c->state()` is
`State::kJoined` and `c->playerId() == 1`, unchanged from P2.

Run: `scripts/tw ctest --test-dir build/plain -R client_test --output-on-failure`
Expected: FAIL — compile error, `'class client::Client<...>' has no member named 'clientTick'`.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: add `clientTick()`, `clockLead()` and `clockSnaps()` accessors and a
`ClockSync clock_` member. In `handleJoinAccept`, after adopting the player id,
set `tick_ = h.tick + static_cast<uint32_t>(kTargetLeadTicks)`.

Note the ordering hazard: `Client::tick()` does `++tick_` **before** draining the
transport, so a `JoinAccept` handled during that drain sets `tick_` after the
increment and the seed is not immediately clobbered. Keep that order.

```bash
scripts/tw ctest --test-dir build/plain -R client_test --output-on-failure && \
  git add src/client/client.h tests/client/client_test.cpp && \
  git commit -m "feat: seed the client clock ahead of the server on join"
```

Expected: PASS, then one commit.

**Checkpoint 2: an observed lead error corrects the clock on the next tick**

- [ ] **Step 1: Write the failing test, then run it**

Spec, as `TEST(ClientTest, SnapshotLeadErrorCorrectsTheClientClock)`:
join as in Checkpoint 1 so `clientTick() == 503`. Then, in three separate
`tick()` calls, inject a snapshot each time and assert the clock's advance:

- Inject a snapshot with `h.tick == 500`, `h.ack_tick == 503` (lead 3, on target) and a payload containing player 1. `c->tick(32)` — `clientTick() == 504`. A nominal tick advances by exactly 1, and `clockLead() == 3`.
- Inject a snapshot with `h.tick == 501`, `h.ack_tick == 502` (lead 1, two short). `c->tick(48)` — `clientTick() == 506`: one nominal advance plus a `+1` correction.
- Inject a snapshot with `h.tick == 502`, `h.ack_tick == 507` (lead 5, two long). `c->tick(64)` — `clientTick() == 506`: one nominal advance plus a `-1` correction, netting zero.

Each snapshot must carry a strictly increasing `h.tick`, or `handleSnapshot`'s
existing newest-wins guard discards it before the clock ever observes it.

Run: `scripts/tw ctest --test-dir build/plain -R client_test --output-on-failure`
Expected: FAIL — the clock advances by exactly 1 every call, so the second
assertion sees 505 rather than 506.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: in `handleSnapshot`, after the newest-wins guard has accepted the
snapshot, call `clock_.observe(h.tick, h.ack_tick)`. In `tick()`, replace
`++tick_` with an increment followed by applying `clock_.takeCorrection()`.
Apply the correction **after** the transport drain, so a correction observed this
frame takes effect this frame:

```
++tick_;
drain transport
tick_ = applyCorrection(tick_, clock_.takeCorrection());
retry-join logic, unchanged
```

`applyCorrection` is a private static: add a non-negative correction directly;
for a negative one, subtract only down to 0 (see Checkpoint 3).

Note this makes `next_retry_tick_`'s arithmetic depend on a clock that can jump.
The join retry path only runs while `state_ == State::kJoining`, and the clock is
only seeded or corrected once joined, so the two never overlap — but if the retry
logic is ever moved, that assumption goes with it.

```bash
scripts/tw ctest --test-dir build/plain -R client_test --output-on-failure && \
  git add src/client/client.h tests/client/client_test.cpp && \
  git commit -m "feat: correct the client clock from the observed buffer depth"
```

Expected: PASS, then one commit.

**Checkpoint 3: a correction can never underflow the clock**

`tick_` is a `uint32_t` and a snap correction reaches −30. A wrap here would put
the client roughly four billion ticks ahead, and every input it sent would be
rejected by the server's acceptance window until the session timed out.

- [ ] **Step 1: Write the failing test, then run it**

Spec, as `TEST(ClientTest, ClockCorrectionNeverUnderflows)`:
join with a `JoinAccept` carrying `h.tick == 0`, so `clientTick() == 3`. Inject a
snapshot with `h.tick == 1` and `h.ack_tick == 20` — lead 19, error +16, a snap
demanding a correction of −16 against a clock reading 4 after the nominal
increment. `c->tick(32)`; `clientTick() == 0`, not a value near `UINT32_MAX`.
Assert `c->clientTick() < 1000u` explicitly, so a wrap fails loudly rather than
reading as an enormous but plausible tick.

Run: `scripts/tw ctest --test-dir build/plain -R client_test --output-on-failure`
Expected: FAIL — `clientTick()` returns `4294967284` from the unsigned wrap.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: `applyCorrection(uint32_t t, int32_t c)` returns `t + c` when `c >= 0`;
when `c < 0` it returns `0` if `t < static_cast<uint32_t>(-c)`, else `t - (-c)`.
Compute the negation in `int64_t` so `INT32_MIN` cannot bite, even though
`kMaxCorrectionTicks` bounds the input well away from it.

```bash
scripts/tw ctest --test-dir build/plain -R client_test --output-on-failure && \
  git add src/client/client.h tests/client/client_test.cpp && \
  git commit -m "fix: saturate the client clock correction at zero"
```

Expected: PASS, then one commit.

**Task boundary:** full suite, plain configuration.

```bash
scripts/tw ctest --test-dir build/plain --output-on-failure
```

---

## Task 6: pending inputs, the prediction world, and the RTT estimate

The client starts simulating. It keeps every input it has sent, applies each one
to a local `sim::World` the moment it goes out, and reports the local player's
position from that world instead of from the snapshot.

**Files:**
- Create: `src/client/prediction.h`, `src/client/prediction.cpp`, `tests/client/prediction_test.cpp`
- Modify: `src/client/client.h`, `tests/client/client_test.cpp`, `CMakeLists.txt`

**Interfaces:**
- Consumes: `sim::InputCommand`, `sim::World`, `sim::PlayerState`.
- Produces, in `namespace client` (`src/client/prediction.h`):

```cpp
inline constexpr size_t kPendingInputSlots = 128;  // ~2.1 s of input at 60 Hz
inline constexpr uint32_t kPendingInputMask = kPendingInputSlots - 1;

struct PendingInput {
  sim::InputCommand cmd;
  uint32_t send_time_ms;
};

// Every input sent and not yet known to be consumed, indexed by tick so a
// replay can ask for one tick at a time. Older than kPendingInputSlots ticks
// is silently evicted -- a client that far behind has already snapped.
class PendingInputs {
 public:
  void record(const sim::InputCommand& in, uint32_t send_time_ms) noexcept;
  const PendingInput* find(uint32_t tick) const noexcept;  // nullptr when absent
};
```

- Produces, on `Client<T>` (`src/client/client.h`):

```cpp
void setPredictionEnabled(bool on) noexcept;
bool predictionEnabled() const noexcept;
// The local player's position: predicted when prediction is on and the
// prediction world has been seeded, otherwise straight from the newest
// snapshot. False when no snapshot has yet carried this player.
bool localPosition(float& x, float& y) const noexcept;
uint32_t rttMs() const noexcept;
```

Add `src/client/prediction.cpp` to `libclient` and register
`tw_add_test(prediction_test tests/client/prediction_test.cpp)`.

**Note on `Client`'s size.** `sim::World` embeds three `kMaxPlayers`-wide arrays
(~2.5 KB) and `PendingInputs` adds 128 × 32 B (~4 KB). `Client` stays well under
10 KB, but the project's heap-allocate-in-tests convention already covers it and
`client_test.cpp` already follows it — keep it that way.

**Checkpoint 1: `PendingInputs` stores by tick and evicts the oldest**

- [ ] **Step 1: Write the failing test, then run it**

Spec, in `tests/client/prediction_test.cpp` as
`TEST(PendingInputsTest, RecordsByTickAndEvictsBeyondCapacity)`:
`PendingInputs p;`
- `p.find(5) == nullptr` on a fresh instance.
- `p.record(InputCommand{.player_id = 1, .tick = 5, .move_x = 1.0f}, 250);` then `p.find(5)` is non-null with `->cmd.move_x == 1.0f` and `->send_time_ms == 250u`.
- `p.find(6) == nullptr` — a neighbouring tick is not a match.
- Record ticks `5 .. 5 + kPendingInputSlots` inclusive (that is one more than capacity). `p.find(5) == nullptr` — tick 5 aliased onto the same slot as tick `5 + kPendingInputSlots` and was overwritten — while `p.find(5 + kPendingInputSlots)` is non-null.

Run: `scripts/tw ctest --test-dir build/plain -R prediction_test --output-on-failure`
Expected: FAIL — compile error, `prediction.h: No such file or directory`.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: `record` writes `slots_[in.tick & kPendingInputMask] = {in, send_time_ms}`
and sets the matching `filled_` flag. `find(tick)` returns `&slots_[i]` only when
`filled_[i] && slots_[i].cmd.tick == tick`, else `nullptr` — the same
tick-equality guard `InputBuffer` uses, and for the same reason: an aliased slot
must read as absent, never as a different tick's input.

```bash
scripts/tw ctest --test-dir build/plain -R prediction_test --output-on-failure && \
  git add src/client/prediction.h src/client/prediction.cpp \
          tests/client/prediction_test.cpp CMakeLists.txt && \
  git commit -m "feat: keep unacknowledged client inputs indexed by tick"
```

Expected: PASS, then one commit.

**Checkpoint 2: sending an input moves the local player immediately**

- [ ] **Step 1: Write the failing test, then run it**

Spec, in `tests/client/client_test.cpp` as
`TEST(ClientTest, SendInputMovesTheLocalPlayerWithoutWaitingForASnapshot)`:
join so the client holds player id 1 with `clientTick() == 503` (as in Task 5).
Prediction is **on by default**.

Deliver one snapshot at `h.tick == 500`, `h.ack_tick == 503`, carrying player 1 at
`(10.0f, 20.0f)` with zero velocity, via `c->tick(32)`. `localPosition(x, y)`
now returns `true` with `x == 10.0f`, `y == 20.0f` — the prediction world has
been seeded from the authoritative state.

Now `c->sendInput(48, 1.0f, 0.0f, 0.0f, 0.0f, false)` returns `true`, and with
**no further snapshot**, `localPosition(x, y)` returns
`x == 10.0f + sim::kMoveSpeed * sim::kTickDt`, `y == 20.0f`. The keypress moved
the player before the server could possibly have heard about it — which is the
entire phase in one assertion.

Run: `scripts/tw ctest --test-dir build/plain -R client_test --output-on-failure`
Expected: FAIL — compile error, `'class client::Client<...>' has no member named 'localPosition'`.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: `Client` gains `sim::World predicted_`, `PendingInputs pending_`, and
`bool predicted_ready_ = false`.

In `handleSnapshot`, after the newest-wins guard accepts the snapshot: locate
`player_id_` in the decoded snapshot. If found and `!predicted_ready_`, call
`predicted_.addPlayer(player_id_, s.x, s.y)` followed by
`predicted_.setPlayerState(s)` — `addPlayer` creates the slot, `setPlayerState`
installs the velocity `addPlayer` zeroes — then set `predicted_ready_ = true`.
The client cannot seed earlier: spawn position is server policy
(`Server::spawnPosition`), deliberately not in `libsim`, so the first snapshot
carrying the player is the first moment the client knows where it is.

In `sendInput`, after `sendFramed` succeeds: `pending_.record(in, now_ms)`, and
when `predicted_ready_ && prediction_enabled_`, `predicted_.applyInput(in)` then
`predicted_.step()`.

`localPosition(x, y)` returns the predicted world's player when
`prediction_enabled_ && predicted_ready_`, otherwise scans `snapshot_` for
`player_id_`; `false` when neither has it. Read the predicted position via
`predicted_.writeSnapshot` into a member scratch `sim::WorldSnapshot` — `World`
exposes no per-player getter and adding one is out of scope. Since
`localPosition` is `const`, make the scratch `mutable` or build a local
`WorldSnapshot` on the stack; the latter is 2.5 KB per call, so prefer the
`mutable` member.

```bash
scripts/tw ctest --test-dir build/plain -R client_test --output-on-failure && \
  git add src/client/client.h tests/client/client_test.cpp && \
  git commit -m "feat: predict the local player's motion on the client"
```

Expected: PASS, then one commit.

**Checkpoint 3: prediction off restores P2's behavior exactly**

- [ ] **Step 1: Write the failing test, then run it**

Spec, as `TEST(ClientTest, PredictionOffRendersTheSnapshotPosition)`:
join, `c->setPredictionEnabled(false)`, `c->predictionEnabled() == false`.
Deliver a snapshot carrying player 1 at `(10.0f, 20.0f)`. `localPosition` returns
`(10.0f, 20.0f)`.

`c->sendInput(48, 1.0f, 0.0f, 0.0f, 0.0f, false)` returns `true`; `localPosition`
still returns exactly `(10.0f, 20.0f)` — the input went out but changed nothing
locally. Re-enabling with `c->setPredictionEnabled(true)` and sending another
input then moves it, proving the toggle is live rather than construction-time.

Run: `scripts/tw ctest --test-dir build/plain -R client_test --output-on-failure`
Expected: FAIL — compile error, `no member named 'setPredictionEnabled'`.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: `bool prediction_enabled_ = true;` with the two accessors.
`setPredictionEnabled(true)` after a period of being off must re-seed rather than
resume from a stale world: set `predicted_ready_ = false` whenever prediction is
switched **on**, so the next snapshot re-seeds from authority. Switching off
needs no bookkeeping.

```bash
scripts/tw ctest --test-dir build/plain -R client_test --output-on-failure && \
  git add src/client/client.h tests/client/client_test.cpp && \
  git commit -m "feat: make client-side prediction toggleable at runtime"
```

Expected: PASS, then one commit.

**Checkpoint 4: RTT from the acknowledged input's send time**

`ack_tick` names an input the client sent and still holds the send time for, so
the round trip is a subtraction — no new wire field, and no clock comparison
between two machines.

- [ ] **Step 1: Write the failing test, then run it**

Spec, as `TEST(ClientTest, RttComesFromTheAcknowledgedInputsSendTime)`:
join so `clientTick() == 503`. `c->rttMs() == 0` before any acknowledgment.
`c->sendInput(100, 1.0f, 0.0f, 0.0f, 0.0f, false)` — this input is stamped tick
503 and recorded with `send_time_ms == 100`.

Deliver a snapshot with `h.tick == 500`, `h.ack_tick == 503`, carrying player 1,
through `c->tick(180)`. `c->rttMs() == 80` — `180 − 100`.

Then deliver a snapshot with `h.ack_tick == 999`, a tick the client never sent.
`c->rttMs()` is **unchanged at 80** — an unmatched acknowledgment updates
nothing rather than producing a garbage sample.

Run: `scripts/tw ctest --test-dir build/plain -R client_test --output-on-failure`
Expected: FAIL — compile error, `no member named 'rttMs'`.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: thread `now_ms` from `tick()` through `handlePacket` into
`handleSnapshot`. There, after the newest-wins guard: if
`const PendingInput* p = pending_.find(h.ack_tick)` is non-null and
`now_ms >= p->send_time_ms`, set `rtt_ms_ = now_ms - p->send_time_ms`. Guard the
subtraction — `now_ms` comes from the caller and `send_time_ms` from a value
derived from an attacker-influenced `ack_tick`; an unguarded unsigned subtraction
would report a ~4-billion-ms round trip.

Use a single sample rather than a smoothed one for now; the first assertion above
pins an exact value, and smoothing would make it an approximation for no benefit
at this stage.

**Document the known bias where `rttMs()` is declared:** this measures
input-to-snapshot latency, not a pure network round trip. The server holds an
input in its buffer for up to `kTargetLeadTicks` ticks before simulating it, and
emits snapshots only every `kSnapshotIntervalTicks` (3) ticks, so the figure runs
roughly 50–100 ms above the wire round trip at 60 Hz. That is the number that
matches what a player actually feels, which is why it is the one worth showing —
but it must not be labelled "ping" in the HUD or the docs.

```bash
scripts/tw ctest --test-dir build/plain -R client_test --output-on-failure && \
  git add src/client/client.h tests/client/client_test.cpp && \
  git commit -m "feat: estimate round-trip latency from the acknowledged input"
```

Expected: PASS, then one commit.

**Task boundary:** full suite, plain and ASan.

```bash
scripts/tw ctest --test-dir build/plain --output-on-failure && \
  scripts/tw ctest --test-dir build/asan --output-on-failure
```

---

## Task 7: reconciliation and the prediction-error record

The client adopts the authoritative state on every snapshot and replays what the
server has not yet consumed. This is where a divergence becomes visible — and
where the number that proves the phase worked gets recorded.

**Files:**
- Modify: `src/client/prediction.h`, `src/client/prediction.cpp`, `src/client/client.h`, `tests/client/prediction_test.cpp`, `tests/client/client_test.cpp`

**Interfaces:**
- Produces, in `namespace client` (`src/client/prediction.h`):

```cpp
// The magnitude of each reconciliation correction, over a rolling window.
// Fixed storage, no allocation; percentiles sort a stack copy on query,
// which is a diagnostic path, never a per-tick one.
class PredictionStats {
 public:
  static constexpr size_t kWindow = 512;  // ~25 s of snapshots at 20 Hz

  void record(float error) noexcept;
  uint32_t samples() const noexcept;  // total recorded, not window-capped
  float p50() const noexcept;         // 0 when samples() == 0
  float p99() const noexcept;         // 0 when samples() == 0
  float worst() const noexcept;       // over all samples, not just the window
};
```

- Produces, on `Client<T>`:

```cpp
const PredictionStats& predictionError() const noexcept;
```

**Checkpoint 1: `PredictionStats` percentiles over a known sample set**

- [ ] **Step 1: Write the failing test, then run it**

Spec, in `tests/client/prediction_test.cpp` as
`TEST(PredictionStatsTest, ReportsPercentilesOverTheRollingWindow)`:
- A fresh instance: `samples() == 0`, `p50() == 0.0f`, `p99() == 0.0f`, `worst() == 0.0f`.
- Record the 100 values `1.0f .. 100.0f` in order. `samples() == 100`. With the nearest-rank convention below, `p50() == 50.0f`, `p99() == 99.0f`, `worst() == 100.0f`.
- Record `1000.0f`, then 600 further values of `1.0f`. `samples() == 701`; the window now holds only the last 512 (all `1.0f`), so `p50() == 1.0f` and `p99() == 1.0f` — but `worst() == 1000.0f`, because the peak is tracked across all samples and is not allowed to age out. A correction that large happened; a rolling window must not be able to hide it.

Run: `scripts/tw ctest --test-dir build/plain -R prediction_test --output-on-failure`
Expected: FAIL — compile error, `'PredictionStats' was not declared in this scope`.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: `record` stores into `window_[count_ % kWindow]`, `++count_`, and
raises `worst_` when the value exceeds it. Ignore a non-finite `error` entirely
— it cannot be a real correction magnitude, and one `NaN` would poison every
percentile through the sort.

Percentiles use the **nearest-rank** convention on the filled portion of the
window: copy the `n = min(count_, kWindow)` live entries into a
`std::array<float, kWindow>` local, `std::sort` the first `n`, and return
element `ceil(p/100 * n) - 1`. For `n = 100` that gives index 49 (`50.0f`) at p50
and index 98 (`99.0f`) at p99, matching the assertions above. State the
convention in a comment at the declaration; percentile conventions differ and a
future reader comparing against another tool needs to know which one this is.

The 2 KB stack copy is fine here — this is called from `apps/` at exit and from
tests, never per tick.

```bash
scripts/tw ctest --test-dir build/plain -R prediction_test --output-on-failure && \
  git add src/client/prediction.h src/client/prediction.cpp \
          tests/client/prediction_test.cpp && \
  git commit -m "feat: record prediction error percentiles over a rolling window"
```

Expected: PASS, then one commit.

**Checkpoint 2: reconciliation replays the unacknowledged inputs**

The gap case — a replayed tick the client holds no input for — is asserted inside
this test rather than being its own checkpoint. It is not separately falsifiable:
the natural implementation of the replay loop (`if (find) applyInput; step();`)
handles it correctly the moment it is written, so a checkpoint for it would
expect a FAIL it could never see. The behavior still matters — it is the client's
half of the underrun symmetry from Task 3 — so it is pinned here.

- [ ] **Step 1: Write the failing test, then run it**

Spec, in `tests/client/client_test.cpp` as
`TEST(ClientTest, ReconciliationReplaysUnacknowledgedInputs)`:
join so `clientTick() == 503`. Seed prediction with a snapshot at `h.tick == 500`,
`h.ack_tick == 503`, carrying player 1 at `(0, 0)` with zero velocity, delivered
via `c->tick(32)`.

Send three inputs, all `move_x = 1.0f`, at successive client ticks — call
`c->sendInput(...)` then `c->tick(...)` between each so they are stamped 504, 505
and 506. `localPosition` now reads `x == 3.0f * sim::kMoveSpeed * sim::kTickDt`.

Now deliver a snapshot with `h.tick == 504` (the server has consumed the input
stamped 504 and no more), `h.ack_tick == 506`, carrying player 1 at a
**deliberately different** authoritative position — `(100.0f, 0.0f)` with
`vx == sim::kMoveSpeed`. After the `tick()` that delivers it, `localPosition`
returns `x == 100.0f + 2.0f * sim::kMoveSpeed * sim::kTickDt`: the authoritative
position, advanced by exactly the two inputs (505 and 506) the server had not yet
consumed. Neither three replays nor zero.

Then the gap assertion, in the same test: send an input stamped 508 without
sending one for 507 (advance the clock past 507 with a bare `c->tick()` call and
no `sendInput`). Deliver a snapshot at `h.tick == 506` carrying player 1 at
`(0.0f, 0.0f)` with `vx == sim::kMoveSpeed`. The replay covers ticks 507 and 508;
507 has no input, so the authoritative velocity carries — `localPosition` returns
`x == 2.0f * sim::kMoveSpeed * sim::kTickDt`, not `1.0f * ...` as it would if a
missing input zeroed the velocity.

Run: `scripts/tw ctest --test-dir build/plain -R client_test --output-on-failure`
Expected: FAIL — with no reconciliation, the snapshot only re-seeds nothing and
`localPosition` still reads `3.0f * sim::kMoveSpeed * sim::kTickDt` from the
locally-predicted world, missing the `100.0f` entirely.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: in `handleSnapshot`, when `predicted_ready_ && prediction_enabled_` and
the snapshot carries `player_id_`:

```
predicted_.setPlayerState(authoritative_state_from_snapshot);
for (uint32_t t = h.tick + 1; t <= tick_; ++t) {
    if (const PendingInput* p = pending_.find(t)) predicted_.applyInput(p->cmd);
    predicted_.step();
}
```

Skipping `applyInput` on a miss — rather than applying a zeroed command — is what
mirrors the server's underrun-repeat. The two must stay symmetrical; see the
Global Constraints.

Bound the loop: if `tick_ < h.tick` (the client is behind the server, which the
clock controller should prevent but a hostile or badly-reordered snapshot could
assert) the range is empty and nothing replays, which is correct. Also cap the
iteration count at `kPendingInputSlots` so a snapshot claiming a tick far in the
past cannot make the client replay millions of steps inside one packet handler —
that is a cheap denial-of-service from a single forged datagram otherwise. When
the cap trips, replay only the most recent `kPendingInputSlots` ticks.

```bash
scripts/tw ctest --test-dir build/plain -R client_test --output-on-failure && \
  git add src/client/client.h tests/client/client_test.cpp && \
  git commit -m "feat: reconcile by adopting authority and replaying pending inputs"
```

Expected: PASS, then one commit.

**Checkpoint 3: the pre-correction error is recorded**

- [ ] **Step 1: Write the failing test, then run it**

Spec, as `TEST(ClientTest, ReconciliationRecordsThePreCorrectionError)`:
join and seed prediction with player 1 at `(0, 0)`, zero velocity, from a
snapshot at `h.tick == 500`, `h.ack_tick == 503`. `c->predictionError().samples() == 0`
— seeding is not a correction and must not count as one.

Send no inputs, then deliver a snapshot at `h.tick == 501`, `h.ack_tick == 503`
placing player 1 at `(3.0f, 4.0f)` with zero velocity. The predicted position was
`(0, 0)`, so the correction magnitude is exactly `5.0f`.
`c->predictionError().samples() == 1` and `c->predictionError().worst() == 5.0f`.

Run: `scripts/tw ctest --test-dir build/plain -R client_test --output-on-failure`
Expected: FAIL — compile error, `no member named 'predictionError'`.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: add a `PredictionStats stats_` member and its accessor. In
`handleSnapshot`, immediately **before** `setPlayerState` overwrites the
predicted state, compute `std::hypot(pred.x - auth.x, pred.y - auth.y)` and pass
it to `stats_.record`. Do not record on the seeding path — there is no prediction
to have been wrong yet. Use `std::hypot` rather than a hand-rolled
`sqrt(dx*dx + dy*dy)`: it avoids the intermediate overflow that squaring two
large-but-finite floats produces, the same hazard the P2 security review found in
`World::resolveHitscan`.

```bash
scripts/tw ctest --test-dir build/plain -R client_test --output-on-failure && \
  git add src/client/client.h tests/client/client_test.cpp && \
  git commit -m "feat: record the magnitude of every reconciliation correction"
```

Expected: PASS, then one commit.

**Task boundary:** full suite, all three configurations.

```bash
scripts/tw ctest --test-dir build/plain --output-on-failure && \
  scripts/tw ctest --test-dir build/asan --output-on-failure && \
  scripts/tw setarch -R ctest --test-dir build/tsan --output-on-failure
```

---

## Task 8: convergence against a real server, and the security review

Everything so far tested the client against hand-built packets. This task runs a
real `Server` against a real `Client` and asserts the property the phase exists
to deliver — and closes the phase's mandatory security review.

**Files:**
- Create: `tests/client/convergence_test.cpp`
- Modify: `CMakeLists.txt`

Register with `tw_add_test(convergence_test tests/client/convergence_test.cpp)`.
Heap-allocate every transport, server and client in this file — `LoopbackTransport`
is ~310 KB and `SimulatedTransport` ~157 KB.

**Checkpoint 1: predicted position converges on the server's over loopback**

- [ ] **Step 1: Write the failing test, then run it**

Spec, as `TEST(ConvergenceTest, PredictedPositionMatchesTheServerOverLoopback)`:
build a `LoopbackTransport` pair, a `Server<LoopbackTransport>` on one end and a
`Client<LoopbackTransport>` on the other. Drive them in lockstep: for
`i = 0 .. 599`, with `now_ms = i * 16`, call `srv->ingest()`, `srv->tick(now_ms)`,
`c->tick(now_ms)`, and once joined `c->sendInput(now_ms, 1.0f, 0.0f, 0.0f, 0.0f, false)`.

After the loop:
- `c->state() == client::State::kJoined`.
- `c->clockLead()` is within 1 of `kTargetLeadTicks` — the controller settled rather than oscillating.
- `c->predictionError().p99() < 0.05f` — at zero latency the prediction is essentially exact; the tolerance covers accumulated float rounding across 600 ticks, nothing more.
- The client's `localPosition` is within `0.05f` of the server's authoritative position for that player, read via `srv->world().writeSnapshot(...)`.
- `srv->inputUnderruns()` is under 30 — a handful during the join ramp is expected, a steady stream means the clock never settled.

Run: `scripts/tw ctest --test-dir build/plain -R convergence_test --output-on-failure`
Expected: FAIL — compile error, `convergence_test.cpp: No such file or directory`.
This checkpoint's Step 2 writes no production code; if the assertions fail once
the file compiles, that is a genuine defect in Tasks 3–7 and it must be root-
caused there, not accommodated by loosening a tolerance here.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: no production change expected. Register the test target. If an
assertion fails, fix the cause in the owning task's file and include that fix in
this commit, noting it in the journal.

```bash
scripts/tw ctest --test-dir build/plain -R convergence_test --output-on-failure && \
  git add tests/client/convergence_test.cpp CMakeLists.txt && \
  git commit -m "test: converge client prediction against a real server"
```

Expected: PASS, then one commit.

**Checkpoint 2: prediction beats no-prediction under 200 ms round-trip latency**

The demo's claim, written as an assertion.

- [ ] **Step 1: Write the failing test, then run it**

Spec, as `TEST(ConvergenceTest, PredictionRemovesTheVisibleLagAt200ms)`:
wrap both ends in `net::SimulatedTransport` with `latency_ms = 100`,
`jitter_ms = 10`, `loss_permille = 0`, `seed = 7`. Per P1's recorded decision,
each end delays its own *inbound* traffic, so wrapping both ends at 100 ms
produces a 200 ms round trip — which is the design doc's headline number.

Run two clients against one server through the same driving loop as Checkpoint 1
for 900 ticks, one with `setPredictionEnabled(true)` and one with `(false)`,
each moving `move_x = 1.0f` every tick. Remember to call `advanceTick()` on each
`SimulatedTransport` once per loop iteration — its delivery schedule is driven by
an injected tick source, not a clock.

Assert:
- The predicting client's `localPosition` is within `0.5f` (one player radius) of the server's authoritative position for it.
- The non-predicting client's rendered position trails the server's by **more than `2.0f`** — at `kMoveSpeed` 8.0 units/s, a 200 ms round trip plus the 50 ms snapshot interval is ~2 units of lag. That gap is the thing the demo shows.
- The predicting client's `predictionError().p99()` is at least 4× smaller than the non-predicting client's rendered-position error over the same run. Record both figures in the journal — this is the number the phase is judged by.

If loss is later added to this config, note that the predicting client's error
will rise and the assertion tolerances are stated for `loss_permille = 0` only.

Run: `scripts/tw ctest --test-dir build/plain -R convergence_test --output-on-failure`
Expected: FAIL initially only if a defect exists; as in Checkpoint 1, a failure
here is a real bug in Tasks 3–7. If it passes on first write, that is the correct
outcome — commit it as the regression pin it is.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: no production change expected.

```bash
scripts/tw ctest --test-dir build/plain -R convergence_test --output-on-failure && \
  git add tests/client/convergence_test.cpp && \
  git commit -m "test: prove prediction removes the visible lag at 200 ms"
```

Expected: PASS, then one commit.

**Task boundary — full suite plus the mandatory security review.**

`CLAUDE.md` makes a security review non-optional for any phase touching the
network surface, and P3 changes how every received input is handled. Run it here,
at the last headless boundary, exactly as P2 did.

```bash
scripts/tw ctest --test-dir build/plain --output-on-failure && \
  scripts/tw ctest --test-dir build/asan --output-on-failure && \
  scripts/tw setarch -R ctest --test-dir build/tsan --output-on-failure
```

Then dispatch the `security-reviewer` and `cpp-reviewer` agents in parallel over
`src/server/`, `src/client/`, and `tests/`, with the threat model P1 and P2 both
used: an unauthenticated attacker controls every byte of every datagram, can send
them at any rate, and can forge any source address the network permits.

Specific P3 surfaces to put in front of them:

- **`InputBuffer::push`'s acceptance window** — can a forged input evict a legitimate one, or wedge the window so a real client's inputs are refused?
- **The `ack_tick`-driven clock controller** — `kMaxCorrectionTicks` bounds a single correction, but can a stream of forged snapshots walk the clock somewhere useless? (Only reachable by an attacker who can already forge snapshots from the server's address, which is the P2-deferred root cause, not a new defect — confirm that reading rather than assume it.)
- **The replay loop's iteration bound** in Task 7 Checkpoint 2 — verify the cap actually holds for every `h.tick`/`tick_` combination, including `h.tick == 0` and `h.tick > tick_`.
- **The `rttMs()` subtraction guard** and the `hypot` overflow avoidance.
- Whether the three CRITICAL findings deferred at P2 have changed shape. They should not have — P3 adds no authentication and removes none — but the input path was rewritten, so the "verified closed" P1 finding (endpoint↔player binding, one chokepoint at `handleInput`) must be **re-verified by grepping every call site of `World::applyInput`**, not assumed to have survived the rewrite. There is now a second caller: the client's replay loop, which is a client-local prediction world with no authority and no network input, and that distinction should be confirmed rather than waved at.

Record every finding in `docs/project-history.md` under a new P3 heading, in the
same shape P1 and P2 used: fixed, deferred with reasoning, or verified closed.
Fix anything CRITICAL or HIGH before Task 9. Commit the fixes individually,
then the history entry.

---

## Task 9: the apps

Two halves with very different verification stories, so they are two separate
steps rather than two checkpoints.

**Files:**
- Modify: `apps/tw_loadclient.cpp`, `apps/tw_client.cpp`, `scripts/e2e-udp.sh`

**Checkpoint 1: the load client reports the phase's numbers**

- [ ] **Step 1: Write the failing test, then run it**

Spec: `apps/tw_loadclient.cpp` prints, on exit, one line per client of the form

```
player=<id> rtt_ms=<n> lead=<n> pred_p50=<f.ff> pred_p99=<f.ff> pred_worst=<f.ff> underruns=<n>
```

`scripts/e2e-udp.sh` already drives a server and a load client over real UDP on
loopback, and CTest asserts `joined=4` against its output. Extend the script's
assertion — and the `PASS_REGULAR_EXPRESSION` on the `e2e_udp` test in
`CMakeLists.txt` — to also require `lead=` to appear, so the stats line being
absent or unprinted fails the test rather than passing silently.

Run: `scripts/tw ctest --test-dir build/plain -R e2e_udp --output-on-failure`
Expected: FAIL — the load client prints no `lead=` field, so the extended regex
does not match.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: after the driving loop and before exit, iterate the clients and
`std::fprintf` the line above using `client->playerId()`, `rttMs()`,
`clockLead()`, `predictionError().p50()/p99()/worst()`, and the client's own
input count. Keep the existing `joined=<n>` line — the e2e test still asserts on
it. `std::fprintf` with `%.2f`, not `std::format` (GCC 10).

Note: `tw_loadclient` currently constructs `Client<net::UdpTransport>` and sends
no inputs at all in its loop; add a fixed `move_x = 1.0f` input per tick per
client so prediction has something to predict and the percentiles are not all
zero.

```bash
scripts/tw ctest --test-dir build/plain -R e2e_udp --output-on-failure && \
  git add apps/tw_loadclient.cpp scripts/e2e-udp.sh CMakeLists.txt && \
  git commit -m "feat: report RTT, clock lead and prediction error from the load client"
```

Expected: PASS, then one commit.

**Step: wire the GUI, then build and smoke-test it**

**This is not a checkpoint and there is no RED to expect.** The raylib client has
no automated test beyond `--selftest`'s "a window opened and closed", which
already passes and is unaffected by this change. Do not write a test that expects
a failure here.

- [ ] Modify `apps/tw_client.cpp`:
  - Render the local player from `client->localPosition(x, y)` instead of scanning the snapshot for it. Remote players keep coming straight from `snap` — prediction is local-player-only, per the Global Constraints.
  - `IsKeyPressed(KEY_P)` toggles `client->setPredictionEnabled(!client->predictionEnabled())`.
  - Draw the local player in a different colour when prediction is off, so the toggle's state is visible without reading the HUD.
  - Extend the HUD line to `tick=%u latency=%ums players=%u pred=%s rtt=%ums lead=%d err_p99=%.2f`, sourced from the accessors added in Tasks 5–7. Label the RTT figure honestly — it is input-to-snapshot latency, not ping (see Task 6 Checkpoint 4).
  - Keep `aimFromCursor` fed from the *predicted* local position, not the snapshot's, so aiming tracks what the player sees.
- [ ] Build the GUI configuration and run the selftest:

```bash
scripts/tw cmake -S . -B build/gui -DTW_BUILD_GUI=ON && \
  scripts/tw cmake --build build/gui -j && \
  scripts/tw ctest --test-dir build/gui -R client_selftest --output-on-failure
```

Expected: PASS, or SKIP with return code 77 when `DISPLAY` is unset — 77 is
CTest's `SKIP_RETURN_CODE` here and is not a failure.

- [ ] Smoke-test the demo: run `scripts/tw bash scripts/demo.sh` with a server and
  two GUI clients for roughly 8 seconds. Confirm no crash and a steady frame rate.
  **The interactive behavior cannot be verified from this session** — the windows
  render through WSLg onto the host desktop and no tool here can capture or click
  into them. Record in the journal that a human still needs to drag the latency
  slider to 200 ms, press `P`, and confirm the difference is as visible as the
  design doc claims. This is the same limitation P2 recorded, and it is the one
  claim in this phase that testing cannot close.

```bash
scripts/tw cmake --build build/gui -j && \
  git add apps/tw_client.cpp && \
  git commit -m "feat: render the predicted local player and toggle prediction with P"
```

**Task boundary:** the three headless configurations plus the GUI build.

```bash
scripts/tw bash scripts/ci.sh
```

---

## Task 10: documentation

No RED, no checkpoints — a documentation pass, same as P2's Task 10.

**Files:**
- Modify: `docs/wire-format.md`, `docs/project-history.md`, `CLAUDE.md`, `README.md`

- [ ] **`docs/wire-format.md`** — **no layout changes, and `kProtocolVersion` stays 2.** Rewrite only the annotations:
  - `tick` — add that a `Snapshot`'s `tick` is the reconciliation acknowledgment: the server consumes inputs strictly in tick order, so a snapshot at tick `S` has consumed every input stamped `≤ S`, and the client replays only those stamped after it.
  - `send_time_ms` — no longer "reserved for P3". The client stores it per pending input and computes input-to-snapshot latency when a snapshot acknowledges that input. Document the bias explicitly (it includes the server's input-buffer hold and the snapshot interval; it is not ping).
  - `ack_tick` — no longer "reserved for P3". `ack_tick − tick` on a snapshot is the depth of that session's server-side input buffer, and is the sole feedback signal driving the client's clock. A snapshot with `ack_tick == 0` acknowledges nothing and is ignored by the controller.
  - Replace the "P2 populates every header field but doesn't yet consume most of them" paragraph with the P3 statement: every header field is now both populated and consumed.
  - Add a "Version history" note that P3 changed no byte layout, so v2 is still current — this is the record that the P1 reservation worked as intended.
- [ ] **`docs/project-history.md`** — a new `## P3 — Prediction, reconciliation, clock sync` section (the file already carries the placeholder comment for it). Cover:
  - **The pivot:** why apply-on-arrival had to become a tick-matched buffer, and the alternative that lost (Gambetta's model, rejected because the server integrates a latched input for a jitter-dependent number of ticks that the client cannot reproduce).
  - **The decision** to derive clock sync from `ack_tick − tick` rather than an explicit RTT round trip, and that this is why the wire format did not need reopening.
  - **The decision** that the client predicts the local player only.
  - **The decision** to resolve fires in a second pass, and the ordering ambiguity in P2 that it removes.
  - **The finding** that `sim::World::latched_` is dead state — written by `addPlayer`/`applyInput`, never read — and that this is *why* `setPlayerState` restoring `vx`/`vy` is sufficient for reconciliation. Flag it as a `/refactor-clean` candidate, deliberately not removed in this phase.
  - **The finding** that `rttMs()` measures input-to-snapshot latency, not ping, with the magnitude of the bias.
  - The Task 8 security review's findings, in P1/P2's shape: fixed, deferred with reasoning, or verified closed.
  - The measured convergence numbers from Task 8 Checkpoint 2.
- [ ] **`CLAUDE.md`** — add to "Verified constraints" that the server consumes exactly one input per player per tick at the tick it was stamped for, that an underrun repeats the previous velocity, and that the client's replay must mirror that by skipping `applyInput` on a tick it holds no input for. This is the invariant most likely to be broken by a well-meaning later change.
- [ ] **`README.md`** — document the demo: run `scripts/demo.sh`, `[`/`]` for latency, `P` for prediction, and what to look for at 200 ms. Note the headline convergence number from Task 8.
- [ ] Commit:

```bash
git add docs/wire-format.md docs/project-history.md CLAUDE.md README.md && \
  git commit -m "docs: record P3's prediction, reconciliation and clock-sync design"
```

- [ ] **Journal the phase** with the `journal` skill.

**Task boundary — the phase's final gate.** Run the full CI script, which covers
all three sanitizer configurations plus the toolchain assertions, and confirm the
GUI configuration still builds.

```bash
scripts/tw bash scripts/ci.sh && \
  scripts/tw cmake --build build/gui -j
```

The branch is now green and verified. Integration — merge, PR, or keep the branch
— is not this plan's decision; hand off to the `finishing-a-development-branch`
skill, which presents that menu.

---

## Self-review notes

Recorded because the next reader will otherwise re-derive them.

**Spec coverage.** The design doc's P3 row names client prediction, server
reconciliation, and clock-sync logic: Tasks 6, 7 and 4–5 respectively. Its
"unlisted subsystem" warning about clock synchronization is Task 4's entire
subject. Its demo requirement (latency slider, prediction toggle, visible
difference) is Task 9 plus the Task 8 Checkpoint 2 assertion. Its instruction to
read the canonical references before P3 was carried out at planning time and is
recorded in the header, including which reference could not be retrieved.

**Two checkpoints deliberately fold assertions rather than splitting.** Task 7
Checkpoint 2 carries the replay-gap case, and Task 3 Checkpoint 2 carries the
velocity-persistence case; in both, the natural implementation of the preceding
checkpoint already satisfies the behavior, so a separate checkpoint would expect
a FAIL it could never observe. Each is pinned as an assertion instead, with the
reason stated inline.

**Three checkpoints may pass on first write** — Task 3 Checkpoint 3 and both
Task 8 checkpoints — and each says so explicitly, with the instruction not to
manufacture a failure. Task 8's are convergence properties over code all of which
already exists by then; a failure there is a real defect in an earlier task, and
the plan says to fix it at its source rather than to relax the tolerance.

**Type consistency.** `kTargetLeadTicks` is `int32_t` and used in
`uint32_t` arithmetic in exactly one place (Task 5 Checkpoint 1's clock seed),
where the cast is written out. `InputBuffer::push` takes only the command and
derives its window from its own consumption floor rather than from a `now_tick`
parameter — that choice makes aliasing impossible by construction and is why no
call site passes a tick to it. `PendingInputs::find` and `InputBuffer::takeFor`
use the same tick-equality-plus-filled-flag guard against slot aliasing.

**The riskiest task is 3, not 7.** It is the only one that changes shipped,
green P2 behavior, and its repair of the four P2 server tests is the most likely
place for a silent weakening of an assertion. Its Step 2 names the four tests and
says to change stamped ticks and nothing else.
