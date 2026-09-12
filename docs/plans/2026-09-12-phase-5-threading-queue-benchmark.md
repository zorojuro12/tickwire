# Phase 5 — Threading & Queue Benchmark Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use the `executing-plans` skill to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Split the server's I/O and simulation onto two threads across the
existing `PacketRing` seam, introduce the lock-free `SpscRing` behind the same
four calls, and produce the project's headline numbers table from a script a
stranger can re-run.

**Architecture:** The seam already exists and is already exercised —
`Server::ingest()` is the producer (`acquireWrite`/`commitWrite`),
`Server::tick()` is the consumer (`acquireRead`/`commitRead`), and today both run
on one epoll loop in `apps/tw_server.cpp`. P5 changes *who calls them*, not what
they do: an I/O thread owns `ingest()`, a sim thread owns `tick()` plus egress,
and the ring between them becomes swappable so a mutex arm and a lock-free arm
can be measured against each other. `Server` gains a second template parameter
(defaulted, so every existing use compiles unchanged); the single-threaded loop
survives as a runtime-selectable baseline arm, because a benchmark with no
baseline has nothing to report.

**Tech Stack:** C++20 (GCC 10.5 pinned), CMake 3.28.4, GoogleTest/CTest,
`std::thread` + `std::atomic` + `std::mutex`, epoll/timerfd, raw POSIX UDP.
All inside the pinned `tickwire-dev:gcc10-cmake3.28.4-x11` image.

**Spec:**
- [`docs/specs/2026-09-04-tickwire-design.md`](../specs/2026-09-04-tickwire-design.md)
  — § "Egress", § "Packet handling", § "The headline artifact"
- [`docs/specs/2026-09-04-architecture-resolution.md`](../specs/2026-09-04-architecture-resolution.md)
  — **§ Q4 is authoritative for `SpscRing`'s ordering contract**
- [`docs/project-history.md`](../project-history.md) — P1's two deferrals to P5

---

## Global Constraints

Every task's requirements implicitly include this section. Values are copied
verbatim from `CLAUDE.md` and the specs — do not re-derive them.

**Build and test**

- **Never invoke the host `g++`, `cmake`, or `ctest`.** The host has GCC 9.4 and
  CMake 3.16 and cannot build this project. Everything goes through
  `scripts/tw <command...>`.
- **`ctest` does not build. Always `cmake --build` first.** A bare
  `ctest -R foo` runs the *previously built* binary (so a newly written test case
  is absent and the run is falsely green), and a regex matching no target at all
  **exits 0**. Every test command in this plan is therefore
  `cmake --build <dir> -j8 && ctest --test-dir <dir> -R <regex> --output-on-failure`.
  This is what makes each checkpoint's "expect FAIL" meaningful.
- **Scope every checkpoint with `-R <regex>`.** Run the full suite only at a task
  boundary. Never run the full suite inside a checkpoint.
- **TSan runs need `setarch -R` around `ctest`, not around the build.**
  `scripts/tw` already supplies `--cap-add SYS_PTRACE --security-opt
  seccomp=unconfined`; both are required together, neither alone is sufficient.
- Configurations: `build/plain`, `build/asan` (`-DTW_SANITIZER=address,undefined`),
  `build/tsan` (`-DTW_SANITIZER=thread`). `scripts/ci.sh` runs all three plus the
  toolchain assertions.

**Language**

- **No `std::format`** (GCC 10 lacks it). **No `std::bit_cast`** (libstdc++ ships
  it only from GCC 11) — pun `float`↔`uint32_t` with `std::memcpy`.
- `-Wall -Wextra -Werror` from the first commit — non-negotiable.
- Types `PascalCase`, constants `kPascalCase`, members `snake_case_`.

**Architecture invariants**

- **No wall-clock reads in testable code.** `Server::tick(uint32_t now_ms)` takes
  the current monotonic millisecond as a *parameter*. `src/server/clock.cpp` is
  the only `clock_gettime` call site in the project, and **only `apps/` calls
  it.** Anything in `src/` that needs time takes it injected. A test that needs
  time passes a number.
- **`apps/` are thin.** "Argument parsing, a clock, and a loop — no logic of
  their own." The threading therefore lives in `src/server/`, not in
  `apps/tw_server.cpp`.
- **`libsim` boundary:** no I/O, no wall-clock reads, no allocation; only
  trivially-copyable POD crosses it. P5 adds nothing to `libsim`.
- **Float determinism flags** propagate `PUBLIC` through `libsim` via
  `tickwire_sim_flags`. Never `-march=native`, never `-ffast-math`,
  `-march=x86-64-v2` does not exist in GCC 10.
- **No egress queue.** The design doc pre-decided this: *"Room threads call
  `sendmmsg` directly. Measure tick jitter; add an egress queue only if the
  numbers demand it."* Adding one speculatively is out of scope.
- **The wire format does not reopen.** `kProtocolVersion` stays 2,
  `sim::kMaxPlayers` stays 32. Any task that believes it needs a format change
  must **stop and record a finding**, not make the change.

**Test hygiene**

- **Heap-allocate large test objects** via `std::make_unique` —
  P1-established, re-confirmed by P2's and P3's security reviews. This bites
  hard in P5: `net::PacketSlot` is **1208 bytes**, so
  `PacketRing<net::PacketSlot, 256>` is **~309 KB** and `SpscRing` of the same
  shape is larger still with its `alignas(64)` padding. Never stack-allocate one.
- Coverage: 80% project-wide; `libsim` is held to 100% (unchanged by P5).
- File size 200–400 lines typical, 800 hard maximum.

**Commits**

- `type: description` (feat, fix, refactor, docs, test, chore, perf, ci).
- One commit per checkpoint, **chained behind its test with `&&`** — never `;`,
  never a separate line, so a red test makes the commit unreachable rather than
  merely inadvisable.
- `git add` names exact paths. **Never** `git add -A` or `git add .`.

**Benchmark honesty (this phase's deliverable is a measurement, so this is a
correctness constraint, not a style note)**

- **The expected result is that lock-free does not beat a mutex at Tickwire's
  real load.** 20 Hz × 32 players ≈ 640 packets/sec; the design doc puts socket
  contention "roughly three orders of magnitude above" that. A table reporting
  *"lock-free wins at saturation; at this project's actual load the two are
  indistinguishable"* is the **correct and intended outcome**, not a failure of
  the phase. Do not tune, re-scope, or re-pick a benchmark to manufacture a win
  for the lock-free arm.
- **One hypothesis per variant.** Never change the ring *and* the syscall
  strategy in the same measured arm — the delta stops being attributable.
- Every reported number carries its provenance: machine, image tag, build
  config, and the exact command that produced it.

---

## File Structure

**New files**

| Path | Responsibility |
|---|---|
| `src/server/mutex_ring.h` | `MutexRing<T,N>` — same four calls, guarded by one `std::mutex`. The benchmark's mutex arm. Header-only. |
| `src/server/spsc_ring.h` | `SpscRing<T,N>` — the lock-free arm, exactly per resolution doc § Q4. Header-only. |
| `src/server/jitter_stats.h` | `JitterStats<N>` — fixed-capacity sample recorder, no allocation, nearest-rank percentiles. Header-only. |
| `src/server/threaded_runner.h` | `ThreadedRunner<T, Ring, Clock>` — owns the I/O thread and the sim thread, injected clock, atomic stop. Header-only (templated). |
| `tests/server/mutex_ring_test.cpp` | Single- and multi-threaded correctness for `MutexRing`. |
| `tests/server/spsc_ring_test.cpp` | Ordering contract + multi-threaded correctness for `SpscRing`. |
| `tests/server/jitter_stats_test.cpp` | Percentile arithmetic, capacity saturation. |
| `tests/server/threaded_runner_test.cpp` | Runner drives a loopback transport end to end; both ring arms. |
| `tests/faults/relaxed_ring.cpp` | **Deliberately broken** ring, built only under TSan, asserted to make TSan report a race. Proves the TSan gate has teeth. |
| `tools/bench_queue.cpp` | Producer/consumer microbenchmark; emits ns/handoff for each ring arm. |
| `scripts/bench.sh` | Regenerates every row of the numbers table. The reproducibility claim. |
| `docs/benchmarks.md` | The numbers table plus its provenance and methodology. |

**Modified files**

| Path | Change |
|---|---|
| `src/net/udp.cpp:81-96` | Bound the oversized-datagram retry loop (P1's deferred finding). |
| `src/net/udp.h` | Declare the optional `receiveBatch` variant (Task 10). |
| `src/server/clock.h` / `clock.cpp` | Add `monotonicNs()`. Still the only `clock_gettime` site. |
| `src/server/server.h` | Second template parameter for the ring type, defaulted. Ingest drain cap. |
| `apps/tw_server.cpp` | `--threads`, `--ring`, `--sim-load-us`; jitter + throughput reporting. |
| `CMakeLists.txt` | `find_package(Threads)`, new test targets, `bench_queue`, fault binary. |
| `README.md` | The headline numbers table. |
| `docs/project-history.md` | P5 decisions, findings, security review. |

**Interface summary** (the exact names later tasks depend on — kept identical
across tasks by construction):

```cpp
// src/server/clock.h
namespace server { uint32_t monotonicMs() noexcept; uint64_t monotonicNs() noexcept; }

// Every ring: the same four calls, already the shape PacketRing uses.
template <typename T, size_t N> class MutexRing / SpscRing {
  T* acquireWrite() noexcept;  void commitWrite() noexcept;
  T* acquireRead() noexcept;   void commitRead() noexcept;
  size_t size() const noexcept;  static constexpr size_t capacity() noexcept;
};

// src/server/jitter_stats.h
template <size_t N> class JitterStats {
  void record(uint64_t interval_ns) noexcept;   // drops silently once full
  size_t count() const noexcept;
  size_t dropped() const noexcept;
  uint64_t percentileNs(double p) noexcept;     // nearest rank; sorts in place
  uint64_t maxNs() noexcept;
};
```

---

## Task 1: Bound the ingest drain loop

Closes P1's deferred finding, recorded in `docs/project-history.md`:
*"`UdpTransport::tryReceive()`'s oversized-datagram retry loop is not bounded
per call… An attacker flooding oversized datagrams could make a single
`tryReceive()` call do more work than the 'one call returns one packet' contract
implies."* P1 deferred it here because P5 owns the receiver thread. It is
independent of everything else in this phase, so it goes first.

**Files:**
- Modify: `src/net/udp.cpp:78-97` (`tryReceive`)
- Modify: `src/net/udp.h` (constant + accessor)
- Test: `tests/net/udp_test.cpp`

**Interfaces:**
- Produces: `net::kMaxOversizedSkipsPerCall` (a `size_t`, value `16`), and
  `UdpTransport::oversizedSkipped() const noexcept -> uint64_t`.

**Checkpoint 1: an oversized datagram is skipped, and the skip is counted**

- [ ] **Step 1: Write the failing test, then run it**

Spec: bind two `UdpTransport`s to `127.0.0.1` (heap-allocate both via
`std::make_unique`). From the sender, `::sendto` a **1400-byte** datagram
directly on the sender's `nativeHandle()` (bypassing `send()`, which refuses
payloads over `kMaxPacket`), then `send()` a normal 8-byte payload. On the
receiver, one `tryReceive(slot)` call returns `true` with `slot.len == 8` — the
oversized datagram was skipped, not returned — and
`receiver->oversizedSkipped() == 1`.

Run: `scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain -R udp_test --output-on-failure"`
Expected: FAIL — compile error, `oversizedSkipped` is not a member of
`net::UdpTransport`.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: add a `uint64_t oversized_skipped_ = 0;` member incremented at the
existing `if (static_cast<size_t>(n) > kMaxPacket) continue;` site, exposed by
`oversizedSkipped()`. Behavior otherwise unchanged.

```bash
scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain -R udp_test --output-on-failure" && \
  git add src/net/udp.h src/net/udp.cpp tests/net/udp_test.cpp && \
  git commit -m "feat: count oversized datagrams skipped during receive"
```

Expected: PASS, then one commit.

**Checkpoint 2: the retry loop is capped per call**

- [ ] **Step 2 prerequisite — Step 1: Write the failing test, then run it**

Spec: send **20** oversized (1400-byte) datagrams back to back and then *no*
well-formed packet. A single `tryReceive(slot)` returns `false` (nothing
deliverable) and `oversizedSkipped() == 16` exactly — the loop stopped at
`kMaxOversizedSkipsPerCall` rather than draining all 20. A second
`tryReceive(slot)` call then returns `false` with `oversizedSkipped() == 32`,
proving the cap is per call and the remaining datagrams are still drained by
subsequent calls rather than lost to a permanent wedge.

> Note on the assertion's robustness: the kernel's socket receive buffer must
> actually hold 20 × 1400 B. `SO_RCVBUF` defaults are far larger than 28 KB on
> Linux, so no `setsockopt` is needed — but if this assertion proves flaky in
> the container, assert `oversizedSkipped() == 16` after the first call and
> `>= 16` (not `== 32`) after the second, and record the deviation in the
> commit message. Do not weaken the first assertion; the cap is the behavior
> under test.

Run: `scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain -R udp_test --output-on-failure"`
Expected: FAIL — `oversizedSkipped()` reports 20, not 16; the loop is uncapped.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: add `inline constexpr size_t kMaxOversizedSkipsPerCall = 16;` to
`src/net/transport.h` beside `kMaxPacket`. In `tryReceive`, count skips within
the call and `return false` once the count reaches the cap. `EINTR` retries stay
uncapped — they are not attacker-driven.

```bash
scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain -R udp_test --output-on-failure" && \
  git add src/net/transport.h src/net/udp.cpp tests/net/udp_test.cpp && \
  git commit -m "fix: bound the oversized-datagram retry loop per receive call"
```

Expected: PASS, then one commit.

**Task boundary — full suite once:**

```bash
scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain --output-on-failure"
```

---

## Task 2: Measurement primitives — nanosecond clock and `JitterStats`

The table's first row is *"tick jitter — p50 / p99 / max deviation from
16.67 ms."* **`monotonicMs()` cannot measure this**: at millisecond resolution
every 16.67 ms interval reads as 16 or 17, so the deviation being measured is
smaller than the measuring unit. A nanosecond clock is a prerequisite for the
deliverable, not a nicety.

**Files:**
- Modify: `src/server/clock.h`, `src/server/clock.cpp`
- Create: `src/server/jitter_stats.h`
- Create: `tests/server/jitter_stats_test.cpp`
- Modify: `CMakeLists.txt` (register `jitter_stats_test` via `tw_add_test`)

**Interfaces:**
- Consumes: nothing.
- Produces: `server::monotonicNs() -> uint64_t` (nanoseconds,
  `CLOCK_MONOTONIC`), and `server::JitterStats<N>` with exactly the surface in
  the Interface summary above.

**Checkpoint 1: `monotonicNs` is monotonic and finer-grained than milliseconds**

- [ ] **Step 1: Write the failing test, then run it**

Spec: in `tests/server/timer_test.cpp`, two successive `monotonicNs()` calls
satisfy `second >= first`, and the pair straddles no more than one second
(`second - first < 1'000'000'000ull`) — a sanity bound proving the unit is
nanoseconds and not, say, microseconds mislabelled. Additionally: over 1000
successive calls, at least one adjacent pair differs by less than 1'000'000 ns,
which is impossible if the underlying source were millisecond-granular.

Run: `scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain -R timer_test --output-on-failure"`
Expected: FAIL — compile error, `monotonicNs` is not declared in `server`.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: `uint64_t monotonicNs() noexcept` in `src/server/clock.cpp`, using
`clock_gettime(CLOCK_MONOTONIC, ...)` and returning
`ts.tv_sec * 1'000'000'000ull + ts.tv_nsec`. This file remains the project's
only `clock_gettime` site.

```bash
scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain -R timer_test --output-on-failure" && \
  git add src/server/clock.h src/server/clock.cpp tests/server/timer_test.cpp && \
  git commit -m "feat: add a nanosecond monotonic clock for jitter measurement"
```

Expected: PASS, then one commit.

**Checkpoint 2: nearest-rank percentiles over a known sample set**

- [ ] **Step 1: Write the failing test, then run it**

Spec: `JitterStats<128>` fed `record(i)` for `i = 1..100` (100 samples, so
`count() == 100`, `dropped() == 0`) returns exactly:
`percentileNs(0.50) == 50`, `percentileNs(0.99) == 99`, `maxNs() == 100`.
Feed the samples in **descending** order in a second case (`i = 100..1`) and
assert the same three values — the recorder must not assume sorted input.

The percentile definition, stated so two implementers write the same code —
**nearest rank, no interpolation**: for `n` samples sorted ascending,
`percentileNs(p)` returns `sorted[idx]` where
`idx = clamp(ceil(p * n) - 1, 0, n - 1)`. Worked: `n = 100, p = 0.50` →
`ceil(50) - 1 = 49` → the 50th smallest → `50`. `n = 100, p = 0.99` →
`ceil(99) - 1 = 98` → the 99th smallest → `99`. An empty recorder
(`count() == 0`) returns `0` from both `percentileNs` and `maxNs`.

Run: `scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain -R jitter_stats_test --output-on-failure"`
Expected: FAIL — `tests/server/jitter_stats_test.cpp` does not compile;
`server/jitter_stats.h` does not exist.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: `template <size_t N> class JitterStats` holding
`std::array<uint64_t, N> samples_`, a `size_t count_`, and a `size_t dropped_`.
`record` appends while `count_ < N`. `percentileNs` sorts `samples_[0..count_)`
in place (`std::sort`) on first use and is therefore non-`const` — permitted
because it runs after a run ends, never on the hot path. No allocation anywhere.
Register the target with `tw_add_test(jitter_stats_test tests/server/jitter_stats_test.cpp)`.

```bash
scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain -R jitter_stats_test --output-on-failure" && \
  git add src/server/jitter_stats.h tests/server/jitter_stats_test.cpp CMakeLists.txt && \
  git commit -m "feat: add JitterStats with nearest-rank percentiles"
```

Expected: PASS, then one commit.

**Checkpoint 3: samples past capacity are dropped and counted, never overwritten**

- [ ] **Step 1: Write the failing test, then run it**

Spec: `JitterStats<4>` fed `record(10), record(20), record(30), record(40),
record(999), record(999)` yields `count() == 4`, `dropped() == 2`,
`maxNs() == 40`. The overflow samples must **not** displace earlier ones —
overwriting would silently bias percentiles toward the end of a run, which is
the specific measurement error this assertion exists to prevent.

Run: `scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain -R jitter_stats_test --output-on-failure"`
Expected: FAIL — `dropped()` returns 0 (Checkpoint 2's implementation has no
saturation accounting).

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: `record` increments `dropped_` and returns without storing once
`count_ == N`. `maxNs()` reflects only recorded samples.

```bash
scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain -R jitter_stats_test --output-on-failure" && \
  git add src/server/jitter_stats.h tests/server/jitter_stats_test.cpp && \
  git commit -m "feat: drop and count JitterStats samples past capacity"
```

Expected: PASS, then one commit.

**Task boundary — full suite once:**

```bash
scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain --output-on-failure"
```

---

## Task 3: `MutexRing` — the benchmark's mutex arm

Built before `SpscRing` deliberately: it is the simpler of the two, it is the
baseline the lock-free arm must be measured against, and getting the shared
four-call contract right once makes the second implementation a drop-in.

**Files:**
- Create: `src/server/mutex_ring.h`
- Create: `tests/server/mutex_ring_test.cpp`
- Modify: `CMakeLists.txt` (`find_package(Threads REQUIRED)`, new test target)

**Interfaces:**
- Consumes: nothing.
- Produces: `server::MutexRing<T, N>` with `acquireWrite`, `commitWrite`,
  `acquireRead`, `commitRead`, `size`, `capacity` — signatures byte-identical to
  `server::PacketRing<T, N>` (`src/server/packet_ring.h`), so the two are
  substitutable at Task 6's template parameter.

**Checkpoint 1: single-threaded behavior matches `PacketRing` exactly**

- [ ] **Step 1: Write the failing test, then run it**

Spec: mirror `tests/server/packet_ring_test.cpp`'s
`CommittedWriteBecomesReadableInOrder` against `MutexRing<uint32_t, 4>` — a new
ring has `size() == 0`, `capacity() == 4`, `acquireRead() == nullptr`; an
uncommitted write is invisible to both `size()` and `acquireRead()`; two
`acquireWrite()` calls without an intervening `commitWrite()` return the **same**
pointer; after `commitWrite()`, `size() == 1` and `acquireRead()` yields the
written value. Fill to capacity (4 committed writes) and assert
`acquireWrite() == nullptr`; `commitRead()` once and assert `acquireWrite()`
becomes non-null again.

Run: `scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain -R mutex_ring_test --output-on-failure"`
Expected: FAIL — `server/mutex_ring.h` does not exist.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: `template <typename T, size_t N> class MutexRing`, `static_assert`ing
`N` is a power of two, holding `std::array<T, N> buf_`, monotonic
`uint64_t write_`/`read_` (never wrapped; masked only at access), and one
`std::mutex mu_`. Every one of the four calls takes `mu_` for its whole body.
Full is `write_ - read_ == N`, empty is `write_ == read_`. Add
`find_package(Threads REQUIRED)` to `CMakeLists.txt` and link
`Threads::Threads` into `libserver`; register the test with `tw_add_test`.

```bash
scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain -R mutex_ring_test --output-on-failure" && \
  git add src/server/mutex_ring.h tests/server/mutex_ring_test.cpp CMakeLists.txt && \
  git commit -m "feat: add MutexRing behind the PacketRing four-call contract"
```

Expected: PASS, then one commit.

**Checkpoint 2: every item crosses a real thread boundary exactly once, in order**

- [ ] **Step 1: Write the failing test, then run it**

Spec: heap-allocate a `MutexRing<uint64_t, 64>` via `std::make_unique`. A
producer thread writes `1..100000` (spinning with `std::this_thread::yield()`
whenever `acquireWrite()` returns `nullptr`); a consumer thread reads until it
has taken 100000 items. Assert: the consumer observed the values in **strictly
ascending order with no gaps and no duplicates** (track only the previous value
and a count — no container needed), the final `size() == 0`, and the count is
exactly 100000. This is the assertion that must hold under TSan, which is where
a wrong contract actually shows up.

Run: `scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain -R mutex_ring_test --output-on-failure"`
Expected: FAIL — compile error, the new `TEST` references
`<thread>` facilities the file does not yet include; once compiling, it fails on
the ordering assertion only if the implementation is wrong.

> If this checkpoint passes the instant it is written (because Checkpoint 1's
> implementation is already correct under concurrency), that is the expected and
> acceptable case for a *correctness pin* over an existing contract — record it
> in the commit message. It is still worth its own commit: it is the only
> assertion in the file that runs two threads, and Task 4's `SpscRing` test is a
> direct copy of it.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: no production change expected — `MutexRing` as built in Checkpoint 1
should already satisfy this. If it does not, the defect is in Checkpoint 1's
locking and is fixed here.

```bash
scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain -R mutex_ring_test --output-on-failure" && \
  scripts/tw bash -c "cmake --build build/tsan -j8 && setarch -R ctest --test-dir build/tsan -R mutex_ring_test --output-on-failure" && \
  git add src/server/mutex_ring.h tests/server/mutex_ring_test.cpp && \
  git commit -m "test: pin MutexRing's cross-thread ordering under TSan"
```

Expected: PASS in both configurations, then one commit.

**Task boundary — full suite, plain and TSan:**

```bash
scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain --output-on-failure"
scripts/tw bash -c "cmake --build build/tsan -j8 && setarch -R ctest --test-dir build/tsan --output-on-failure"
```

---

## Task 4: `SpscRing` — the lock-free arm, per resolution doc § Q4

**§ Q4 is authoritative and already fixed at P0** precisely so this task
implements a decided contract rather than inventing one under benchmark
pressure. Do not deviate from it; if it appears wrong, that is a finding to
record, not a change to make.

The contract, quoted:

- Capacity is a power of two, `static_assert`ed; slot index is `idx & (N-1)`.
- **Indices are monotonic `uint64_t`, never wrapped.** Masking happens only at
  access time.
- **Ordering:** producer loads `read_` *acquire*, stores `write_` *release*;
  consumer loads `write_` *acquire*, stores `read_` *release*. **No `seq_cst` on
  the hot path.**
- `acquire`/`commit` split instead of `push(T)`/`pop() -> T` — writes land
  directly in the slot, so the ring is allocation-free *and* copy-free.
- **`alignas(64)` on both indices and the buffer.** Without padding the two
  indices share a cache line and the benchmark measures cache-line ping-pong
  rather than synchronization cost.

**Files:**
- Create: `src/server/spsc_ring.h`
- Create: `tests/server/spsc_ring_test.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: nothing.
- Produces: `server::SpscRing<T, N>`, four calls signature-identical to
  `MutexRing<T, N>` and `PacketRing<T, N>`.

**Checkpoint 1: single-threaded behavior matches the shared contract**

- [ ] **Step 1: Write the failing test, then run it**

Spec: the same assertions as Task 3 Checkpoint 1, against
`SpscRing<uint32_t, 4>`: empty/full detection, uncommitted writes invisible,
repeated `acquireWrite()` returning the same pointer, capacity exhaustion at 4
committed writes, and recovery after one `commitRead()`.

Run: `scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain -R spsc_ring_test --output-on-failure"`
Expected: FAIL — `server/spsc_ring.h` does not exist.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: exactly § Q4's declaration —
`alignas(64) std::atomic<uint64_t> write_{0};`,
`alignas(64) std::atomic<uint64_t> read_{0};`,
`alignas(64) std::array<T, N> buf_{};`. `acquireWrite` loads `read_` with
`memory_order_acquire` and `write_` with `memory_order_relaxed` (the producer
owns `write_`, so it needs no acquire on its own index); returns `nullptr` when
`write_ - read_ == N`. `commitWrite` stores `write_ + 1` with
`memory_order_release`. `acquireRead`/`commitRead` are the mirror image. No
`seq_cst` anywhere. Register with `tw_add_test`.

```bash
scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain -R spsc_ring_test --output-on-failure" && \
  git add src/server/spsc_ring.h tests/server/spsc_ring_test.cpp CMakeLists.txt && \
  git commit -m "feat: add the lock-free SpscRing per resolution doc Q4"
```

Expected: PASS, then one commit.

**Checkpoint 2: the `alignas(64)` separation is real, not aspirational**

- [ ] **Step 1: Write the failing test, then run it**

Spec: a compile-time assertion that the two indices do not share a cache line —
`static_assert` inside the test that
`offsetof(SpscRing<uint32_t,4>, read_) - offsetof(SpscRing<uint32_t,4>, write_) >= 64`.
Because `offsetof` on a class with private members is not available, expose this
instead as a `static constexpr size_t indexStride() noexcept` on the ring
returning
`reinterpret_cast<const std::byte*>(&read_) - reinterpret_cast<const std::byte*>(&write_)`
— which cannot be `constexpr`. **Resolved:** make it a runtime accessor
`size_t indexByteSeparation() const noexcept` and assert
`ring->indexByteSeparation() >= 64` in the test.

> This checkpoint exists because § Q4 names the padding as the thing that
> decides whether the benchmark measures synchronization or false sharing. An
> `alignas` that a future refactor silently drops would make every number in
> `docs/benchmarks.md` wrong while every other test stayed green — there is no
> other assertion in this plan that would catch it.

Run: `scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain -R spsc_ring_test --output-on-failure"`
Expected: FAIL — compile error, `indexByteSeparation` is not a member.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: add `size_t indexByteSeparation() const noexcept` returning the byte
distance between the two atomic members, computed with `reinterpret_cast<const
std::byte*>` on their addresses. Diagnostic only; never called on the hot path.

```bash
scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain -R spsc_ring_test --output-on-failure" && \
  git add src/server/spsc_ring.h tests/server/spsc_ring_test.cpp && \
  git commit -m "test: pin SpscRing's cache-line separation between indices"
```

Expected: PASS, then one commit.

**Checkpoint 3: 100000 items cross a real thread boundary in order, clean under TSan**

- [ ] **Step 1: Write the failing test, then run it**

Spec: byte-for-byte the same test body as Task 3 Checkpoint 2, with
`SpscRing<uint64_t, 64>` substituted for `MutexRing<uint64_t, 64>` — producer
writes `1..100000`, consumer asserts strictly ascending with no gaps or
duplicates, final `size() == 0`, exactly 100000 items observed. **This is the
assertion TSan verifies the acquire/release pairing against**, and it is the
reason § Q4 calls that pairing "the pairing TSan verifies."

Run (both configurations — TSan is the one that matters here):
```bash
scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain -R spsc_ring_test --output-on-failure"
scripts/tw bash -c "cmake --build build/tsan -j8 && setarch -R ctest --test-dir build/tsan -R spsc_ring_test --output-on-failure"
```
Expected: FAIL — compile error on the missing `<thread>` include and the new
`TEST` body.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: no production change expected if Checkpoint 1 implemented § Q4's
ordering correctly. If TSan reports a race, the defect is in the ordering and is
fixed here — **by correcting the ordering to match § Q4, never by relaxing the
test or adding `seq_cst` to silence it.**

```bash
scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain -R spsc_ring_test --output-on-failure" && \
  scripts/tw bash -c "cmake --build build/tsan -j8 && setarch -R ctest --test-dir build/tsan -R spsc_ring_test --output-on-failure" && \
  git add src/server/spsc_ring.h tests/server/spsc_ring_test.cpp && \
  git commit -m "test: verify SpscRing's acquire/release pairing under TSan"
```

Expected: PASS in both configurations, then one commit.

**Task boundary — full suite, all three configurations:**

```bash
scripts/tw bash scripts/ci.sh
```

---

## Task 5: Prove the TSan gate has teeth

Task 4 Checkpoint 3 passing under TSan is only evidence if TSan would have
**failed** on a wrong ordering. Without this task, "our lock-free ring is
TSan-clean" is an untested claim about the test, and the phase's central
technical assertion rests on it. The project already has exactly this pattern:
`tests/faults/race.cpp` is built only under TSan and asserted via
`PASS_REGULAR_EXPRESSION "data race"` (`CMakeLists.txt:90-95`).

**Files:**
- Create: `tests/faults/relaxed_ring.cpp`
- Modify: `CMakeLists.txt` (inside the existing `if(TW_SANITIZER MATCHES "thread")` block)

**Interfaces:**
- Consumes: nothing — the fault binary deliberately contains its own broken ring
  rather than including `spsc_ring.h`, so no production header can be edited
  into passing this.

**Checkpoint 1: TSan reports a data race on a deliberately mis-ordered ring**

- [ ] **Step 1: Write the failing test, then run it**

Spec: `tests/faults/relaxed_ring.cpp` contains a **local, deliberately broken**
copy of the ring — same structure as `SpscRing` but with every atomic operation
using `memory_order_relaxed`, and **without** the `alignas(64)` padding. A
producer thread and a consumer thread pass 10000 `uint64_t` values through it.
Register as a CTest test named `tsan_detects_relaxed_ring` with
`PASS_REGULAR_EXPRESSION "data race"`, inside the existing
`if(TW_SANITIZER MATCHES "thread")` block. The test **passes when TSan reports a
race**, exactly as `tsan_detects_race` already does.

> **Fallback, decided now rather than mid-execution.** TSan models atomic
> orderings, so relaxed indices should leave the `buf_` accesses unsynchronized
> and produce a report. If it does not fire reliably across repeated runs, make
> the fault ring's indices **plain non-atomic `uint64_t`** (structurally what
> `server::PacketRing` already is) — which races unambiguously. Prefer relaxed
> atomics because they prove the stronger claim (TSan catches *wrong ordering*,
> not merely *absent atomics*); take the fallback only after observing the
> relaxed version fail to report across at least 5 runs, and record which
> version shipped in the commit message and in `docs/project-history.md`.

Run: `scripts/tw bash -c "cmake --build build/tsan -j8 && setarch -R ctest --test-dir build/tsan -R tsan_detects_relaxed_ring --output-on-failure"`
Expected: FAIL — `ctest` matches no test named `tsan_detects_relaxed_ring`.
**Note: a no-match `ctest` run exits 0, so read the output for
`No tests were found`** rather than trusting the exit code here; this is the
plan's one checkpoint where the red signal is textual rather than an exit code.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: the fault source plus its `add_executable` / `add_test` /
`set_tests_properties` registration, mirroring `fault_race`'s three lines
exactly. It must not be built in `plain` or `asan` — a deliberate race under
ASan is undefined behavior with no assertion protecting it.

```bash
scripts/tw bash -c "cmake --build build/tsan -j8 && setarch -R ctest --test-dir build/tsan -R 'tsan_detects' --output-on-failure" && \
  git add tests/faults/relaxed_ring.cpp CMakeLists.txt && \
  git commit -m "test: prove TSan catches a relaxed-ordering ring"
```

Expected: PASS (both `tsan_detects_race` and `tsan_detects_relaxed_ring`), then
one commit.

**Task boundary — TSan suite once:**

```bash
scripts/tw bash -c "cmake --build build/tsan -j8 && setarch -R ctest --test-dir build/tsan --output-on-failure"
```

---

## Task 6: Make `Server`'s ring type a template parameter

**Files:**
- Modify: `src/server/server.h:24-27` (class template header), `:329` (member)
- Test: `tests/server/server_test.cpp`

**Interfaces:**
- Consumes: `server::MutexRing<T,N>` (Task 3), `server::SpscRing<T,N>` (Task 4).
- Produces:
  `template <net::Transport T, typename Ring = PacketRing<net::PacketSlot, kIngestCapacity>> class Server`.
  The default keeps every existing `Server<net::UdpTransport>` and
  `Server<net::LoopbackTransport>` spelling valid with no edit — verified by the
  existing suite compiling untouched.

**Checkpoint 1: `Server` runs on an explicitly-named ring type**

- [ ] **Step 1: Write the failing test, then run it**

Spec: in `tests/server/server_test.cpp`, a test instantiating
`Server<net::LoopbackTransport, SpscRing<net::PacketSlot, kIngestCapacity>>`
(heap-allocated — the ring alone is ~309 KB before `alignas` padding) and
driving the existing join-then-input path against it: one `JoinRequest`
produces a session, one `Input` stamped for the next tick is applied, and
`worldTick()` advances. Assert the same observable outcomes the existing
single-ring join/input test asserts, proving the substitution is behavior-
preserving rather than merely compiling.

Run: `scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain -R server_test --output-on-failure"`
Expected: FAIL — compile error: `Server` takes one template argument, not two.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: add the defaulted second template parameter and change the member
declaration at `src/server/server.h:329` from
`PacketRing<net::PacketSlot, kIngestCapacity> ring_;` to `Ring ring_;`. No call
site changes. The four `ring_.*` call sites in `ingest()` and `tick()` are
already written against the shared contract and need no edit.

```bash
scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain -R server_test --output-on-failure" && \
  git add src/server/server.h tests/server/server_test.cpp && \
  git commit -m "refactor: make Server's ingest ring a template parameter"
```

Expected: PASS, then one commit.

**Task boundary — full suite once** (this task touches a header every server and
client test includes, so the whole suite is the real signal):

```bash
scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain --output-on-failure"
```

---

## Task 7: `ThreadedRunner` — the two-thread split

The heart of the phase. Lives in `src/server/` because `apps/` carry no logic of
their own, and takes its clock injected because `src/` never reads a wall clock.

**What is shared between the two threads, established by reading the code
rather than assumed:**

- `ring_` — the only shared mutable state, and the reason `Ring` is swappable.
- `transport_` — **safe to share without a mutex.** `UdpTransport::send()` and
  `tryReceive()` both read `fd_` and mutate no member state (`src/net/udp.cpp:64-97`),
  and POSIX guarantees concurrent `sendto`/`recvfrom` on one socket fd. The I/O
  thread receives; the sim thread sends. **Do not add a transport mutex** — it
  would serialize the two halves and make the benchmark measure the wrong thing.
- Everything else in `Server` (`world_`, `sessions_`, `inputs_`, every counter)
  is touched **only** by `tick()`, i.e. only by the sim thread. `ingest()` writes
  only `ring_` and `ingest_overflows_`.
- **The counters are the one remaining hazard:** the main thread reads
  `droppedPackets()`, `ingestOverflows()`, etc. for the final report while the
  worker threads are still running. Resolved by **joining both threads before
  any counter is read** — not by making the counters atomic, which would add
  hot-path cost to satisfy a shutdown-time read.

**Files:**
- Create: `src/server/threaded_runner.h`
- Create: `tests/server/threaded_runner_test.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `Server<T, Ring>` (Task 6), `JitterStats<N>` (Task 2), `TickTimer`,
  `PollSet`.
- Produces:
```cpp
template <net::Transport T, typename Ring, size_t JitterSamples = 65536>
class ThreadedRunner {
 public:
  // clock_ns: injected nanosecond source. clock_ms: injected millisecond
  // source passed straight through to Server::tick.
  ThreadedRunner(Server<T, Ring>& srv, T& transport, uint32_t tick_hz,
                 std::function<uint64_t()> clock_ns,
                 std::function<uint32_t()> clock_ms) noexcept;
  bool run(uint32_t ticks_limit) noexcept;  // false if the timer is invalid
  void requestStop() noexcept;              // safe from a signal handler
  uint32_t ticksRun() const noexcept;       // valid after run() returns
  JitterStats<JitterSamples>& jitter() noexcept;  // valid after run() returns
  uint64_t packetsIngested() const noexcept;
};
```

**Checkpoint 1: the runner drives a full join/tick cycle across two threads**

- [ ] **Step 1: Write the failing test, then run it**

Spec: with a heap-allocated pair of connected `LoopbackTransport`s, a
`Server<net::LoopbackTransport, MutexRing<net::PacketSlot, kIngestCapacity>>`,
and a `ThreadedRunner` over them, `run(30)` returns `true`, `ticksRun() == 30`,
and the server's `worldTick() == 30`. Inject `clock_ns` as a counter advancing
16'666'667 ns per call and `clock_ms` as a counter advancing 16 ms per call — no
real clock, per the no-wall-clock rule. Use `tick_hz = sim::kTickHz`.

Run: `scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain -R threaded_runner_test --output-on-failure"`
Expected: FAIL — `server/threaded_runner.h` does not exist.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: `run()` spawns one I/O thread and runs the sim loop on the calling
thread (so `run()` is synchronous and needs no separate `join()` in the caller's
happy path), then joins the I/O thread before returning. The I/O thread owns its
own `PollSet` registered on `transport.nativeHandle()` and calls `srv.ingest()`
on readiness with a 50 ms wait timeout, looping until `stop_` is set. The sim
loop owns a `TickTimer(tick_hz)` and its own `PollSet`, calling
`srv.tick(clock_ms())` once per expiration. `stop_` is a
`std::atomic<bool>`; `requestStop()` stores `true` with
`memory_order_relaxed`. Both threads exit before `run()` returns, so every
counter is safe to read afterward.

> `LoopbackTransport` has no pollable fd. For the loopback case the I/O thread
> must therefore call `srv.ingest()` in a `yield()` loop rather than through
> `PollSet`. Select between the two at compile time with
> `if constexpr (requires { transport.nativeHandle(); })` — `LoopbackTransport`
> does not declare `nativeHandle()`, so this is a clean compile-time fork and
> not a runtime type check.

```bash
scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain -R threaded_runner_test --output-on-failure" && \
  git add src/server/threaded_runner.h tests/server/threaded_runner_test.cpp CMakeLists.txt && \
  git commit -m "feat: add ThreadedRunner splitting ingest from tick"
```

Expected: PASS, then one commit.

**Checkpoint 2: a packet sent from the peer reaches the world across the seam**

- [ ] **Step 1: Write the failing test, then run it**

Spec: the producer/consumer path must be proven to carry real traffic, not just
to spin. Before `run(30)`, encode a `JoinRequest` and `send()` it from the peer
transport. After `run(30)` returns, assert `srv->sessions().count() == 1` and
`runner->packetsIngested() >= 1`. This fails if the I/O thread never runs, if
`ingest()` is never called, or if the ring never hands the packet across.

Run: `scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain -R threaded_runner_test --output-on-failure"`
Expected: FAIL — `packetsIngested` is not a member of `ThreadedRunner`.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: accumulate `ingest()`'s return value into a plain `uint64_t` owned by
the I/O thread and exposed by `packetsIngested()` — read only after the threads
have joined, so no atomic is needed.

```bash
scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain -R threaded_runner_test --output-on-failure" && \
  git add src/server/threaded_runner.h tests/server/threaded_runner_test.cpp && \
  git commit -m "feat: count packets ingested across the threaded seam"
```

Expected: PASS, then one commit.

**Checkpoint 3: tick intervals are recorded as jitter samples**

- [ ] **Step 1: Write the failing test, then run it**

Spec: after `run(30)` with the synthetic `clock_ns` advancing exactly
16'666'667 ns per call, `runner->jitter().count() == 29` (one interval per tick
after the first — the first tick has no predecessor to measure against) and
`runner->jitter().maxNs() == 16'666'667`. An exactly-uniform injected clock must
produce exactly-uniform samples; any other value means the runner is measuring
something other than the inter-tick interval.

Run: `scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain -R threaded_runner_test --output-on-failure"`
Expected: FAIL — `jitter()` is not a member of `ThreadedRunner`.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: the sim loop calls `clock_ns()` once per tick, and from the second
tick onward records `now - prev` into the `JitterStats<JitterSamples>` member.
`jitter()` returns it by reference.

```bash
scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain -R threaded_runner_test --output-on-failure" && \
  git add src/server/threaded_runner.h tests/server/threaded_runner_test.cpp && \
  git commit -m "feat: record per-tick interval jitter in the sim loop"
```

Expected: PASS, then one commit.

**Checkpoint 4: the runner is clean under TSan on both ring arms**

- [ ] **Step 1: Write the failing test, then run it**

Spec: a parameterized (or simply duplicated) test running the Checkpoint 2 body
against **both** `MutexRing<net::PacketSlot, kIngestCapacity>` and
`SpscRing<net::PacketSlot, kIngestCapacity>`, so TSan inspects both arms of the
benchmark rather than only the one Checkpoint 1 happened to use. Same assertions:
`ticksRun() == 30`, one session joined, `packetsIngested() >= 1`.

Run: `scripts/tw bash -c "cmake --build build/tsan -j8 && setarch -R ctest --test-dir build/tsan -R threaded_runner_test --output-on-failure"`
Expected: FAIL — compile error, the `SpscRing` instantiation of the test does
not exist yet.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: test-only addition. If TSan reports a race in either arm, fix the
runner (or the ring) — **never** by loosening an assertion or adding a mutex
around the transport.

```bash
scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain -R threaded_runner_test --output-on-failure" && \
  scripts/tw bash -c "cmake --build build/tsan -j8 && setarch -R ctest --test-dir build/tsan -R threaded_runner_test --output-on-failure" && \
  git add tests/server/threaded_runner_test.cpp && \
  git commit -m "test: run the threaded runner under TSan on both ring arms"
```

Expected: PASS in both configurations, then one commit.

**Task boundary — all three configurations:**

```bash
scripts/tw bash scripts/ci.sh
```

---

## Task 8: `tw_server` gains threading, synthetic load, and reporting

**Files:**
- Modify: `apps/tw_server.cpp`
- Modify: `CMakeLists.txt` (a new CTest case for the threaded path)

**Interfaces:**
- Consumes: `ThreadedRunner` (Task 7), `monotonicNs` (Task 2), both rings.
- Produces: CLI flags `--threads <1|2>` (default `1`), `--ring <spsc|mutex>`
  (default `spsc`, meaningful only with `--threads 2`), `--sim-load-us <n>`
  (default `0`); and four new stdout lines, format frozen here because
  `scripts/bench.sh` parses them:

```
threads=<1|2> ring=<spsc|mutex|none> sim_load_us=<n>
jitter_ns p50=<n> p99=<n> max=<n> samples=<n> dropped=<n>
packets_ingested=<n> ingest_overflows=<n>
```

**Checkpoint 1: the single-threaded path is preserved as the baseline arm**

- [ ] **Step 1: Write the failing test, then run it**

Spec: a CTest case `server_app_threads1` running
`tw_server --port 0 --ticks 5 --threads 1` whose output matches
`PASS_REGULAR_EXPRESSION "threads=1"`. The existing `server_app_runs` case
(`--port 0 --ticks 5`, matching `ticks=5`) must continue to pass unchanged —
defaulting to `--threads 1` is what keeps the baseline arm the default and keeps
the existing e2e test meaningful.

Run: `scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain -R 'server_app' --output-on-failure"`
Expected: FAIL — `ctest` matches no test named `server_app_threads1`; read the
output for `No tests were found` rather than trusting the exit code.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: parse the three new flags, rejecting an unknown value for `--threads`
or `--ring` with the existing usage error path (exit non-zero, which
`server_app_bogus_arg` already relies on). With `--threads 1`, keep today's
epoll loop verbatim and print `ring=none`. Print the `threads=` line before the
existing `ticks=` line.

```bash
scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain -R 'server_app' --output-on-failure" && \
  git add apps/tw_server.cpp CMakeLists.txt && \
  git commit -m "feat: add a --threads flag keeping the single-threaded baseline"
```

Expected: PASS, then one commit.

**Checkpoint 2: `--threads 2` runs the threaded runner on the selected ring**

- [ ] **Step 1: Write the failing test, then run it**

Spec: two CTest cases —
`server_app_threads2_spsc` running `tw_server --port 0 --ticks 5 --threads 2
--ring spsc`, matching `PASS_REGULAR_EXPRESSION "threads=2 ring=spsc"`, and
`server_app_threads2_mutex` with `--ring mutex`, matching
`"threads=2 ring=mutex"`.

Run: `scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain -R 'server_app_threads2' --output-on-failure"`
Expected: FAIL — no such tests; the flags are parsed but `--threads 2` still
runs the single-threaded loop and prints `ring=none`.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: with `--threads 2`, construct the `Server` and `ThreadedRunner` on the
ring named by `--ring`, passing `server::monotonicNs` and
`server::monotonicMs` as the injected clocks — `apps/` is the only layer allowed
to name them. The two ring choices instantiate different template types, so
select with a small `if`/`else` over two fully-constructed paths rather than
attempting a runtime-polymorphic ring. `requestStop()` is wired to the existing
`SIGINT` handler via the existing `g_stop` flag.

```bash
scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain -R 'server_app' --output-on-failure" && \
  git add apps/tw_server.cpp CMakeLists.txt && \
  git commit -m "feat: run the threaded server on a selectable ring"
```

Expected: PASS, then one commit.

**Checkpoint 3: `--sim-load-us` burns a measurable amount of per-tick time**

- [ ] **Step 1: Write the failing test, then run it**

Spec: this is the knob that locates the jitter cliff, since real player count
never will (see Task 11). A CTest case `server_app_sim_load` running
`tw_server --port 0 --ticks 20 --threads 2 --ring spsc --sim-load-us 20000`
must report a `jitter_ns` line whose `p50` exceeds `16666667` — a 20 ms
synthetic load on a 16.67 ms budget *must* push the measured interval over
budget, or the knob is not actually costing anything. Assert with
`PASS_REGULAR_EXPRESSION "jitter_ns p50=(1[7-9]|[2-9][0-9])[0-9]{6}"`.

> The regex, spelled out: matches a `p50` of 8 or 9 digits beginning `17`–`19`
> or `20`–`99`, i.e. ≥ 17'000'000 ns. A load that failed to cost anything would
> print `p50=166…` and not match.

Run: `scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain -R server_app_sim_load --output-on-failure"`
Expected: FAIL — no such test, and no `jitter_ns` line is printed at all.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: print the `jitter_ns` and `packets_ingested` lines after the threads
join. Implement the load as a busy-wait on `monotonicNs()` inside a
`--sim-load-us`-microsecond deadline, applied once per tick on the sim thread.
A busy-wait on a real clock cannot be optimized away, unlike an arithmetic
dummy loop. With `--sim-load-us 0` (the default) the burn is skipped entirely so
the unloaded measurement pays nothing for the flag's existence.

```bash
scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain -R 'server_app' --output-on-failure" && \
  git add apps/tw_server.cpp CMakeLists.txt && \
  git commit -m "feat: report tick jitter and add a synthetic per-tick load knob"
```

Expected: PASS, then one commit.

**Task boundary — all three configurations:**

```bash
scripts/tw bash scripts/ci.sh
```

---

## Task 9: `bench_queue` — the handoff-latency microbenchmark

The row the phase exists for. A standalone tool rather than a test, because it
measures rather than asserts — but it lives in `tools/` beside `digest_dump`,
which the determinism harness already treats the same way.

**Files:**
- Create: `tools/bench_queue.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `MutexRing`, `SpscRing`, `monotonicNs`.
- Produces: a binary taking `--ring <spsc|mutex> --items <n> --rate <n>` (`--rate 0`
  meaning "as fast as possible", i.e. saturation) and printing one line whose
  format `scripts/bench.sh` parses:

```
ring=<spsc|mutex> items=<n> rate=<n> ns_per_handoff_p50=<n> ns_per_handoff_p99=<n> total_ms=<n>
```

**Checkpoint 1: the benchmark runs both arms and reports per-handoff latency**

- [ ] **Step 1: Write the failing test, then run it**

Spec: CTest cases `bench_queue_spsc` and `bench_queue_mutex` running
`bench_queue --ring spsc --items 10000 --rate 0` and the `mutex` equivalent,
each matching `PASS_REGULAR_EXPRESSION "ns_per_handoff_p50=[0-9]+"`. Registering
them as tests keeps the tool compiling and running in CI, so it cannot rot
between the day it is written and the day the table is generated — but the
**numbers** are produced by `scripts/bench.sh` (Task 11), not asserted here.

Run: `scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain -R bench_queue --output-on-failure"`
Expected: FAIL — no such tests; `tools/bench_queue.cpp` does not exist.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: producer thread and consumer thread over a heap-allocated ring of
**`net::PacketSlot`** (not `uint32_t`) — the 1208-byte slot is what the real
seam carries, and benchmarking a 4-byte payload would measure the wrong thing
and reproduce exactly the `memcpy`-instead-of-synchronization error the design
doc already warned about. The producer stamps `monotonicNs()` into the slot's
first 8 bytes before `commitWrite()`; the consumer reads it after
`acquireRead()` and records `now - stamped` into a `JitterStats<1<<20>`.
Percentiles come from `JitterStats`, reusing Task 2 rather than re-deriving.
`--rate n` (n > 0) paces the producer to n items/sec by busy-waiting on
`monotonicNs()`; `--rate 0` runs flat out.

```bash
scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain -R bench_queue --output-on-failure" && \
  git add tools/bench_queue.cpp CMakeLists.txt && \
  git commit -m "feat: add the queue handoff latency microbenchmark"
```

Expected: PASS, then one commit.

**Checkpoint 2: the benchmark is clean under TSan**

- [ ] **Step 1: Write the failing test, then run it**

Spec: the two `bench_queue` CTest cases above, run under the TSan
configuration, must pass with no race reported — the benchmark itself must not
be the source of a race, or its numbers are meaningless. Reduce to
`--items 10000` (already the registered value) so the TSan run stays fast.

Run: `scripts/tw bash -c "cmake --build build/tsan -j8 && setarch -R ctest --test-dir build/tsan -R bench_queue --output-on-failure"`
Expected: this may PASS immediately if Checkpoint 1 was written correctly. **If
it does, that is the acceptable "correctness pin" case** — record it in the
commit message rather than inventing a failure. If TSan *does* report a race,
the defect is in the benchmark harness (most likely the timestamp stamping
racing the slot's reuse) and is fixed here.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: whatever fix the TSan run demands, or no production change if it is
already clean.

```bash
scripts/tw bash -c "cmake --build build/tsan -j8 && setarch -R ctest --test-dir build/tsan -R bench_queue --output-on-failure" && \
  git add tools/bench_queue.cpp && \
  git commit -m "test: verify the queue benchmark is race-free under TSan"
```

Expected: PASS, then one commit.

**Task boundary — all three configurations:**

```bash
scripts/tw bash scripts/ci.sh
```

---

## Task 10: `recvmmsg` as a measured variant

P1 deferred `recvmmsg`/`sendmmsg` batching to P5. It is built here as **one
variant with one hypothesis**, adopted only if it measurably wins — per the
design doc's *"I measured before optimising"* and the
`benchmark-optimization-loop` skill's promotion gate. **Egress stays
`sendto`-per-packet**: the design doc pre-decided that an egress queue waits for
the numbers to demand it, and nothing in this phase's measurements will.

**Files:**
- Modify: `src/net/udp.h`, `src/net/udp.cpp`
- Modify: `apps/tw_server.cpp` (a `--batch-ingest` flag)
- Test: `tests/net/udp_test.cpp`

**Interfaces:**
- Produces: `UdpTransport::receiveBatch(std::span<PacketSlot> slots) -> size_t`
  — fills up to `slots.size()` slots in one `recvmmsg` call, returns how many
  were filled, `0` when nothing is available. Same per-call oversized cap and
  same `oversizedSkipped()` accounting as `tryReceive`.

**Checkpoint 1: `receiveBatch` fills several slots in one call**

- [ ] **Step 1: Write the failing test, then run it**

Spec: with two bound `UdpTransport`s, `send()` five distinct 8-byte payloads
from the sender. On the receiver, one `receiveBatch(std::span(slots))` call over
a heap-allocated `std::array<net::PacketSlot, 8>` returns `5`, with
`slots[i].len == 8` for `i` in `0..4` and each `slots[i].peer` equal to the
sender's `localEndpoint()`. A second call on a drained socket returns `0`.

Run: `scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain -R udp_test --output-on-failure"`
Expected: FAIL — compile error, `receiveBatch` is not a member of
`net::UdpTransport`.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: `recvmmsg` with `MSG_DONTWAIT` over a stack `std::array<mmsghdr, 64>`
and matching `iovec` array, capped at `min(slots.size(), 64)`. Each returned
message's `msg_len` over `kMaxPacket` is skipped and counted exactly as
`tryReceive` does. `tryReceive` itself is left **unchanged** — both paths must
remain available, because the unbatched arm is the baseline this variant is
measured against.

```bash
scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain -R udp_test --output-on-failure" && \
  git add src/net/udp.h src/net/udp.cpp tests/net/udp_test.cpp && \
  git commit -m "feat: add a recvmmsg batch receive path alongside tryReceive"
```

Expected: PASS, then one commit.

**Checkpoint 2: `--batch-ingest` selects the batched path end to end**

- [ ] **Step 1: Write the failing test, then run it**

Spec: a CTest case `server_app_batch_ingest` running
`tw_server --port 0 --ticks 5 --threads 2 --ring spsc --batch-ingest`, matching
`PASS_REGULAR_EXPRESSION "batch_ingest=1"`. The `threads=` output line gains a
`batch_ingest=<0|1>` field, defaulting to `0`.

Run: `scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain -R server_app_batch_ingest --output-on-failure"`
Expected: FAIL — no such test; the flag is unrecognized and `tw_server` exits
non-zero on it.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: add `Server::ingestBatch()` alongside `ingest()`, acquiring up to 32
ring slots and filling them via `receiveBatch`. `ThreadedRunner` gains a
`bool batch` constructor argument selecting which the I/O thread calls. Guard
the whole path with `if constexpr (requires { transport.receiveBatch(...); })`
so `LoopbackTransport` (which has no batch path) still instantiates.

```bash
scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain -R 'server_app|threaded_runner' --output-on-failure" && \
  git add src/server/server.h src/server/threaded_runner.h apps/tw_server.cpp CMakeLists.txt && \
  git commit -m "feat: select batched ingest with --batch-ingest"
```

Expected: PASS, then one commit.

**Task boundary — all three configurations:**

```bash
scripts/tw bash scripts/ci.sh
```

---

## Task 11: `scripts/bench.sh` and the measured table

**This task produces the phase's deliverable.** Everything before it was
scaffolding.

**Files:**
- Create: `scripts/bench.sh`
- Create: `docs/benchmarks.md`

**Interfaces:**
- Consumes: `tw_server`'s frozen output lines (Task 8, Task 10), `bench_queue`'s
  frozen output line (Task 9), `tw_loadclient --players <n> --ticks <n>`.
- Produces: `docs/benchmarks.md`, regenerable by one command.

**Checkpoint 1: the script regenerates every row unattended**

- [ ] **Step 1: Write the failing test, then run it**

Spec: `scripts/bench.sh --smoke` completes with exit 0 and prints a line
matching `rows=` reporting how many table rows it produced. `--smoke` uses
short runs (`--ticks 60`, `--items 10000`) so it is usable as a CI-time check
that the harness still works; the real table comes from the full run. Register
as CTest case `bench_smoke` with no `PASS_REGULAR_EXPRESSION` — exit code is the
signal, following the deliberate precedent `e2e_udp` sets in `CMakeLists.txt`
(that property, once set, *replaces* the exit code as CTest's sole pass/fail
signal, which would mask a `set -e` failure).

Run: `scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain -R bench_smoke --output-on-failure"`
Expected: FAIL — no such test; `scripts/bench.sh` does not exist.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: `set -euo pipefail`. Four measurement groups, each parsing the frozen
output lines above:

1. **Tick jitter vs player count** — for `players` in `1 2 4 8 16 32`: start
   `tw_server --threads 2 --ring spsc --ticks 1200`, run
   `tw_loadclient --players <n> --ticks 1200` against it, parse the `jitter_ns`
   line. (32 is `sim::kMaxPlayers`; the wire format's own ceiling is 48 for full
   snapshots, but `kMaxPlayers` is frozen at 32 this phase and **must not be
   raised** — see the note in `docs/benchmarks.md` that Checkpoint 2 writes.)
2. **The jitter cliff** — for `sim_load_us` in `0 2000 8000 14000 16000 18000
   20000`: `tw_server --threads 2 --ring spsc --ticks 600 --sim-load-us <n>`,
   parse `jitter_ns`. Report the lowest load at which `p99 > 16666667`.
3. **Queue handoff latency** — `bench_queue` for each of `spsc` and `mutex`, at
   `--rate 0` (saturation) and at `--rate 640` (Tickwire's actual load:
   20 Hz × 32 players). Four numbers; the `--rate 640` pair is the one that
   answers "does this matter here?"
4. **Packet throughput** — `tw_server --threads 2 --ring spsc` with
   `--batch-ingest` on and off, parsing `packets_ingested` over a fixed tick
   count. This is `recvmmsg`'s promotion gate: adopt only if it wins here.

Each group prints TSV to stdout. `--smoke` shortens every run. The script must
record `uname -srm`, `nproc`, and the image tag into its output header, because
a number without its machine is not a measurement.

```bash
scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain -R bench_smoke --output-on-failure" && \
  git add scripts/bench.sh CMakeLists.txt && \
  git commit -m "feat: add scripts/bench.sh regenerating the numbers table"
```

Expected: PASS, then one commit.

**Checkpoint 2: the real table is measured and written down**

- [ ] **Step 1: Run the full benchmark**

Run: `scripts/tw bash scripts/bench.sh | tee /tmp/bench-p5.tsv`

There is no FAIL/PASS here — this step *produces data*. Record the output
verbatim.

- [ ] **Step 2: Write `docs/benchmarks.md`, then commit**

Contract: `docs/benchmarks.md` contains, in order:

1. **Provenance** — machine (`uname -srm`, `nproc`), image tag
   `tickwire-dev:gcc10-cmake3.28.4-x11`, build config `RelWithDebInfo`, the
   exact command (`scripts/tw bash scripts/bench.sh`), and the date.
2. **Methodology** — tick jitter is the *inter-tick interval* measured on the
   sim thread with `monotonicNs()`, reported as nearest-rank percentiles of the
   deviation from the nominal 16'666'667 ns; handoff latency is producer-stamp
   to consumer-read across a `PacketSlot`-sized ring; percentile definition
   stated as in Task 2.
3. **The four tables**, from the measured data.
4. **An explicit interpretation section.** Write what the numbers actually say,
   including — if it is what they show — that **player count never exceeds the
   jitter budget within `kMaxPlayers`**, that the cliff sits at N µs of added
   per-tick work rather than at any player count, and that **lock-free and mutex
   are indistinguishable at Tickwire's real 640 packets/sec while lock-free wins
   at saturation.** State the `recvmmsg` adopt/reject decision and its number.
   Per this plan's Global Constraints, that null result is the intended
   outcome — do not re-run the benchmark hunting for a more flattering one.

```bash
git add docs/benchmarks.md && \
  git commit -m "docs: record the P5 measured numbers table"
```

Expected: one commit. (No test command: this checkpoint's deliverable is a
document derived from the previous step's data, and the harness that produced it
is already gated by `bench_smoke`.)

**Task boundary — all three configurations:**

```bash
scripts/tw bash scripts/ci.sh
```

---

## Task 12: Mandatory security review

**Not optional.** `CLAUDE.md`: *"Any phase touching the network surface →
`security-review` skill / `security-reviewer` agent — **mandatory**, not
optional."* P5 changes the receive path (`recvmmsg`, the drain cap), adds
concurrency to packet handling, and adds attacker-reachable CLI surface. P1, P2,
P3 and P4 each ran this at their own boundary; P5 does the same.

**Files:**
- Modify: `docs/project-history.md` (the findings, under the P5 section)
- Plus whatever the findings require.

**Process:**

- [ ] Run the `security-reviewer` and `cpp-reviewer` agents **in parallel** over
  `src/server/spsc_ring.h`, `src/server/mutex_ring.h`,
  `src/server/threaded_runner.h`, `src/server/jitter_stats.h`, the P5 diffs to
  `src/net/udp.{h,cpp}` and `src/server/server.h`, `tools/bench_queue.cpp`, and
  `apps/tw_server.cpp`. Two independent agents converging on one finding is a
  much stronger signal than either alone — P4's review found its most serious
  issue exactly this way.

- [ ] Threat model, unchanged from P1/P2/P3/P4: an unauthenticated attacker
  controls every byte of every datagram, can send them at any rate, and can
  forge any source address the network permits.

- [ ] Questions this phase specifically raises, each to be answered explicitly
  in the write-up rather than left implied:
  - Can a flood make `ingestBatch` acquire ring slots it then fails to fill,
    wedging the ring or losing slots?
  - Does the per-call oversized cap (Task 1) hold on the `recvmmsg` path too, or
    only on `tryReceive`?
  - With the I/O thread now draining independently of the tick rate, can an
    attacker fill the ring faster than `tick()` drains it and starve legitimate
    traffic in a way the single-threaded loop did not allow? What does
    `ingest_overflows_` do under that load?
  - Are the three P2 CRITICAL findings (spoofable session authorization,
    join/snapshot amplification, session-table exhaustion) still exactly as
    recorded — **re-checked against this phase's actual diff, not assumed?**
  - Does `--sim-load-us` create a remotely-triggerable DoS? (It is operator-set,
    not attacker-set — confirm that and say so.)

- [ ] Fix every CRITICAL and HIGH before proceeding. Each fix gets its own
  RED→GREEN commit with a regression test, or an explicit note of why it is not
  runtime-testable (P4's `static_assert` fix is the precedent).

- [ ] Record every finding — fixed, deferred, and verified-closed — in
  `docs/project-history.md` under `### Task 12 boundary — mandatory security
  review findings`, matching the structure P1–P4 established.

```bash
scripts/tw bash scripts/ci.sh && \
  git add docs/project-history.md && \
  git commit -m "docs: record the P5 security review findings"
```

---

## Task 13: Writeup and final verification

The phase's value to a reader is concentrated here, and this is deliberately its
own task rather than a footnote on Task 11 — a measured table nobody can
interpret is not a deliverable.

**Files:**
- Modify: `README.md`
- Modify: `docs/project-history.md`
- Modify: `CLAUDE.md` (file-structure table rows for the new modules)
- Create: `journal/<date>_ansh_p5-execution.md`

**Checkpoint 1: `README.md` carries the headline table**

- [ ] **Step 1: Write it**

Contract: a "Measured results" section with the four-row summary table, each row
linking into `docs/benchmarks.md` for methodology. Include the honest
interpretation in one or two sentences — specifically the lock-free-vs-mutex
finding at real load. Per the resolution doc § Q1's scoping rule, **the README
must never say "lockstep" or "cross-platform"**; the same discipline applies to
every claim in this table, which describes one machine and one pinned image.

- [ ] **Step 2: Verify and commit**

```bash
scripts/tw bash scripts/ci.sh && \
  git add README.md docs/benchmarks.md && \
  git commit -m "docs: add the measured numbers table to the README"
```

**Checkpoint 2: history, conventions, and the journal**

- [ ] **Step 1: Write them**

Contract:
- `docs/project-history.md` — P5's decisions (the ring made a template parameter
  rather than a swap; the transport shared without a mutex, with the POSIX
  reasoning; no egress queue and why; the `recvmmsg` adopt/reject decision and
  its number), plus any findings execution turned up that this plan did not
  anticipate. The existing P5 section already holds the import-map finding from
  the planning session — append, do not overwrite.
- `CLAUDE.md` — add `src/server/{spsc_ring,mutex_ring,threaded_runner,jitter_stats}.h`
  and `tools/bench_queue.cpp` to the file-structure table; add a "Verified
  constraints" entry for the two-thread ownership split (which thread owns which
  state, and that the transport needs no mutex) if execution confirmed it.
- `journal/` — a session entry via the `journal` skill.

- [ ] **Step 2: Final full verification, then commit**

```bash
scripts/tw bash scripts/ci.sh && \
  scripts/tw bash -c "cmake -S . -B build/gui -DTW_BUILD_GUI=ON && cmake --build build/gui -j8 && ctest --test-dir build/gui -R client_selftest --output-on-failure" && \
  git add docs/project-history.md CLAUDE.md journal/ && \
  git commit -m "docs: journal the P5 execution session"
```

Expected: `ci.sh` green across plain/ASan/TSan plus the toolchain assertions;
`client_selftest` passes (exit 0 with a display, or 77 = CTest `SKIP_RETURN_CODE`
without one). Then one commit.

**The branch is now green and verified. Stop here** — `executing-plans` hands off
to `finishing-a-development-branch`, which owns the merge decision. Do not merge,
push, or open a PR from within this plan.

---

## Self-Review

**1. Spec coverage.** The design doc's headline artifact names four rows: tick
jitter percentiles (Task 2 primitives, Task 7 recording, Task 8 reporting,
Task 11 group 1), concurrent players before jitter exceeds budget (Task 11
groups 1–2 — and see the honest-result note below), queue handoff latency mutex
vs lock-free (Tasks 3, 4, 9, Task 11 group 3), packet throughput (Task 10,
Task 11 group 4). Resolution doc § Q4's five contract clauses are covered by
Task 4 Checkpoints 1–3 (capacity/masking, ordering, acquire-commit split) and
Checkpoint 2 specifically (the `alignas(64)` separation, which nothing else would
catch). The design doc's "no egress queue until the numbers demand it" is
honored as a Global Constraint. P1's two deferrals land in Task 1 and Task 10.
The mandatory network-surface security review is Task 12.

**One honest gap, stated rather than papered over:** the design doc's row
*"concurrent players before jitter exceeds budget"* likely has **no answer within
`sim::kMaxPlayers`** — 32 players at 60 Hz is microseconds of work against a
16'667 µs budget. Task 11 measures to 32 anyway and Task 11 group 2 locates the
real cliff via synthetic load, with Task 11 Checkpoint 2 required to say so
explicitly. Raising `kMaxPlayers` to manufacture a cliff was considered and
rejected: it reopens a wire format frozen at P2 for a third time, breaks the
1200-byte no-fragmentation rule the design doc set deliberately, and contradicts
P4's `static_assert` pinning `kMaxPlayers == 32`.

**2. Placeholder scan.** No "TBD", no "add error handling", no "similar to Task
N". Every checkpoint names exact inputs and exact expected values (sample sets
`1..100`, `dropped() == 2`, `oversizedSkipped() == 16`, `jitter().count() == 29`,
`maxNs() == 16'666'667`). The two regexes that could have been vague are spelled
out in prose beside themselves. The percentile definition is given with two
worked examples so two implementers write the same test.

**3. Type consistency.** Checked across tasks: all three rings expose exactly
`acquireWrite`/`commitWrite`/`acquireRead`/`commitRead`/`size`/`capacity`,
matching `PacketRing`'s existing spelling at `src/server/packet_ring.h:18-31` —
this is what makes Task 6's template substitution work. `JitterStats`'s surface
is named once in the Interface summary and used unchanged in Tasks 7, 9 and 11
(`percentileNs`, `maxNs`, `count`, `dropped` — never `p50()` or `pctl()`).
`monotonicNs` is spelled identically in Tasks 2, 7, 8 and 9. The `tw_server` and
`bench_queue` output lines are frozen in Tasks 8/9/10 and parsed in Task 11 by
the same field names.

**4. Checkpoint falsifiability.** Every checkpoint names an assertion at a
public surface that fails before and passes after. Three needed restructuring
during review and were fixed here rather than left to be discovered:

- **Task 4 Checkpoint 2** originally asserted the `alignas` via `offsetof` on
  private members, which does not compile. Resolved by adding
  `indexByteSeparation()` — an interface extension decided in the plan, which is
  the skill's prescribed way out of an unobservable distinction.
- **Task 3 Checkpoint 2** and **Task 9 Checkpoint 2** are correctness pins that
  may pass the moment they are written, because a correct earlier implementation
  already satisfies them. Rather than delete them (they are the only two-thread
  and only TSan assertions on their respective components) or pretend they will
  go red, each says so explicitly and instructs the executor to record the pass
  in the commit message. This is the one deviation from "every Step 1 expects
  FAIL", made deliberately and flagged in place so a cold executor does not halt
  or "fix" a correct test until it fails.
- **Task 5 Checkpoint 1** and **Task 8 Checkpoint 1** have a `ctest`-specific
  trap: a regex matching no test **exits 0**, so the red signal is the textual
  `No tests were found`, not the exit code. Both say so inline.

---

## Execution Handoff

Per `docs/dev-workflow-guide.md` § 2a, this plan was written in Opus and is
intended to execute in Sonnet, in a fresh session with no conversation history.
Everything that session needs is in this document plus the auto-loaded
`CLAUDE.md`: the Global Constraints section carries the build commands, the
container invocation, the language limits, and the test-scoping rules verbatim.
