# Tickwire — Project Conventions

## Project Overview

Tickwire is a C++20 authoritative multiplayer game server: raw UDP, a
fixed-tick deterministic simulation core (`libsim`), client-side prediction,
server reconciliation, and lag compensation. The deliverable is a measurable
numbers table (tick jitter percentiles, concurrent players before jitter
exceeds budget, queue handoff latency, packet throughput) plus a working
local demo — not just a running server. See the design doc (linked below) for
the full phase table and rationale; see `docs/project-history.md` for what's
changed or been discovered since.

## File Structure

| Path | Contents |
|---|---|
| `src/sim/` | `libsim` — deterministic simulation core (`sim::World`) and POD payload types. No I/O, no wall-clock reads, no allocation. |
| `src/net/` | `libnet` — wire protocol codecs, bounds-checked byte cursors, framing (`net::framePacket`), and the `Transport` implementations (UDP, loopback, simulated). |
| `src/server/` | `libserver` — `Server<T>`, `SessionTable` (endpoint↔player binding), `PacketRing` (I/O↔sim seam), the epoll/timerfd tick loop (`PollSet`/`TickTimer`), and the monotonic clock. |
| `src/client/` | `libclient` — `Client<T>` (join handshake, input send, snapshot store) and the pure world→screen view mapping used by the raylib renderer. |
| `apps/` | Thin executables: `tw_server`, `tw_loadclient` (headless load client), `tw_client` (raylib demo client). Argument parsing, a clock, and a loop — no logic of their own. |
| `scripts/` | `tw` (container invocation), `ci.sh`, `demo.sh`, `e2e-udp.sh`, toolchain/determinism verification scripts. |
| `tests/` | GoogleTest suites, mirroring `src/` by subdirectory (`tests/net/`, `tests/server/`, `tests/client/`, ...); `tests/support/` holds fixtures shared across suites. |
| `tools/` | Standalone executables used by tests (e.g. `digest_dump` for the determinism harness). |
| `docs/` | Specs, phase plans, project history, and frozen format references (e.g. `wire-format.md`). |

## Verified constraints

Every rule below was hit as a real failure during toolchain verification or
plan execution, not assumed.

- **The server consumes exactly one input per player per tick, at the tick it
  was stamped for — never on arrival.** `Server::tick()`'s pass 1 pulls each
  live session's input for the tick the call is about to simulate
  (`server::InputBuffer::takeFor`). A tick with no matching input is an
  *underrun*: it applies nothing, so `World::step()` integrates whatever
  velocity is already latched — repeating the previous input, not stopping or
  erroring. The client's reconciliation replay (`Client::reconcile`) must
  mirror this exactly: on a tick it holds no pending input for, it skips
  `applyInput` and calls only `World::step()`. Breaking this symmetry (e.g.
  zeroing velocity on a client-side miss, or applying a stale input on a
  server-side one) reintroduces the exact divergence P3 exists to close — see
  `docs/project-history.md`'s P3 pivot entry for why apply-on-arrival was
  replaced with this in the first place.

## Build and test

- **Never invoke the host `g++`, `cmake`, or `ctest`.** The host has GCC 9.4
  and CMake 3.16 and cannot build this project (CMake 3.16 in particular is
  below the 3.21 floor — see below). All build/test commands go through
  `scripts/tw <command...>`, which runs `<command...>` inside the pinned
  `tickwire-dev:gcc10-cmake3.28.4-x11` image with the repo bind-mounted at
  `/work`.
- **CMake floor is 3.21+ (the image pins 3.28.4).** On Debian's packaged
  CMake 3.18, `ctest --test-dir` runs **zero tests and exits 0** — a silent
  green. Never install CMake from apt in the image.
- Configure/build/test three configurations as needed: `build/plain`,
  `build/asan` (`-DTW_SANITIZER=address,undefined`), `build/tsan`
  (`-DTW_SANITIZER=thread`). `scripts/ci.sh` runs all three plus the
  toolchain assertions in one call — it is what CI calls, so it is always
  reproducible locally.
- **`-DTW_BUILD_GUI=ON` (default `OFF`) builds the raylib demo client**
  (`build/gui`), fetching raylib via `FetchContent`. Left off `ci.sh`'s three
  configurations deliberately — CI has no display and shouldn't pay for
  raylib's build three times. `tw_client --selftest` proves the display path
  works: exits `0` after opening and closing a real window when `DISPLAY` is
  set, exits `77` (CTest `SKIP_RETURN_CODE`, not a failure) when it isn't.
- **Changing the `Dockerfile` requires bumping the image tag in
  `scripts/tw`.** `scripts/tw` skips the build when a matching tag already
  exists locally, so editing the `Dockerfile` without bumping the tag
  silently reuses the stale image and the change appears not to work.

## Container invocation

`scripts/tw` fixes the container invocation:
`--cap-add SYS_PTRACE --security-opt seccomp=unconfined --user $(id -u):$(id -g)`.

- Without the capability flags, TSan aborts with `unexpected memory mapping`.
- Without `--user`, the bind mount writes root-owned files onto the host.
- **TSan test runs must additionally be wrapped in `setarch -R`.** Neither
  the capability flags nor `setarch` alone is sufficient — `setarch -R`
  without the flags fails with `Operation not permitted`; the flags without
  `setarch -R` still aborts. Both are required together.

## Float determinism flags

Every target that links `libsim` must carry, via the `tickwire_sim_flags`
INTERFACE target: `-march=x86-64` and `-ffp-contract=off`.

- **Never `-march=native`** — it enables FMA contraction, which breaks
  bit-for-bit determinism across builds.
- **Never `-ffast-math`.**
- **`-march=x86-64-v2` does not exist in GCC 10** — do not use it.

`tickwire_sim_flags` propagates these `PUBLIC` through `libsim`, so "identical
flags in both binaries" is a build-graph property, not something to remember
by hand.

## Language

- **No `std::format`** — GCC 10 lacks it. Use fmtlib if formatting is needed.

## Wire protocol (frozen at P1, amended once at P2)

- **The format is at version 2** (`kProtocolVersion = 2`) —
  [`docs/wire-format.md`](docs/wire-format.md) is authoritative and current;
  a version-1 header is rejected outright, no cross-version compatibility.
- **Protocol fields are explicitly little-endian**, encoded/decoded byte by
  byte through `net::ByteWriter`/`net::ByteReader` — never `memcpy` a struct
  onto the wire, never `reinterpret_cast` a buffer to a struct. `_be`
  suffixes (`Endpoint::addr_be`, `Endpoint::port_be`) are reserved for values
  the *kernel* requires in network order; everything else is little-endian.
- **No `std::bit_cast`** — GCC 10's libstdc++ ships it only from GCC 11. Pun
  `float`↔`uint32_t` with `std::memcpy`, which is well-defined regardless.

## No wall-clock reads in testable code

`Server::tick(uint32_t now_ms)` and `Client::tick(uint32_t now_ms)` take the
current monotonic millisecond as a **parameter**, never read it themselves.
`server::monotonicMs()` (`src/server/clock.cpp`) is the only `clock_gettime`
call in the project, and only `apps/` calls it. A test that needs time passes
a number.

## `libsim` boundary

`libsim` has no I/O, no wall-clock reads, and no allocation. Only
trivially-copyable POD crosses its boundary.

**Forbidden interface shapes** (see the architecture-resolution doc for
rationale):
- Returning `optional<vector<byte>>` from a receive call.
- `push(T)` / `pop() -> optional<T>` on the ring buffer.
- A `WorldSnapshot` holding a `vector`.

Hand off by index or `span` into a preallocated slot instead.

## Warnings

`-Wall -Wextra -Werror` from the first commit — this is non-negotiable.

## Commits

`type: description` (feat, fix, refactor, docs, test, chore, perf, ci). One
commit per checkpoint, chained behind its test with `&&` — never `;`, never a
separate line. `git add` names exact paths; **never** `git add -A` or
`git add .`.

## Testing discipline

- Inside a checkpoint, scope tests with `-R <regex>`; run the full suite only
  at a task boundary. Never run the full suite inside a checkpoint.
- Coverage: 80% project-wide; `libsim` is held to 100%.

## File size

200–400 lines typical, 800 hard maximum.

## Workflow

Full detail lives in [`docs/dev-workflow-guide.md`](docs/dev-workflow-guide.md)
— this is the quick-reference version.

| Situation | Use |
|---|---|
| Resolve open architectural questions before writing a phase plan | `/impl-plan` |
| Break an approved phase into committable tasks | `writing-plans` skill → `docs/plans/` |
| Execute a phase plan task-by-task | `executing-plans` skill |
| Build/compile errors | `/build-fix`, or the `cpp-build-resolver` agent |
| Fix a bug — reproduce as a failing test first | `/orch-fix-defect` |
| Behavior-preserving refactor | `/orch-refine-code` |
| General code review | `/code-review` command / `code-reviewer` agent |
| C++ idiom, RAII, lifetime, move-semantics review | `cpp-reviewer` agent |
| **Any phase touching the network surface** | `security-review` skill / `security-reviewer` agent — **mandatory**, not optional |
| Remove dead code | `/refactor-clean` |
| Scan for leaked secrets before pushing | `/security-scan` |
| Finish a phase branch | `finishing-a-development-branch` skill |
| Log a session | `journal` skill |
| Record a decision, pivot, or finding | `docs/project-history.md` |

## Specs

- [`docs/specs/2026-09-04-tickwire-design.md`](docs/specs/2026-09-04-tickwire-design.md) — the design
- [`docs/specs/2026-09-04-architecture-resolution.md`](docs/specs/2026-09-04-architecture-resolution.md) — authoritative for every architectural decision
- [`docs/wire-format.md`](docs/wire-format.md) — the wire format, frozen at P1 and amended once at P2 (version 2)
- [`docs/project-history.md`](docs/project-history.md) — cross-phase decisions, pivots, and findings
- [`docs/dev-workflow-guide.md`](docs/dev-workflow-guide.md) — full tool/skill/agent reference by situation
