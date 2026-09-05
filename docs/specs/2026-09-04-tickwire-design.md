# Tickwire — netcode you can see working

**Status:** picked, not started · **Captured:** 2026-09-04 · **Revised:** 2026-09-04 after a machine check
**Live spec.** The copy in `~/projects/project idea/` is the historical idea record; this is the one `/impl-plan` consumes.
**Category:** C++ systems — *not* part of the RAG idea set below, different resume track.
**Siblings (RAG track, shelved):** [spoiler-firewall.md](spoiler-firewall.md) ·
[filing-drift.md](filing-drift.md) · [comment-letters.md](comment-letters.md) ·
[need-to-know.md](need-to-know.md) · [idea-verification-checklist.md](idea-verification-checklist.md)

**Name:** **Tickwire** — fixed *tick* simulation over the *wire*. Both halves describe
what the project is, and it reads as infrastructure rather than as a game.

**Decided 2026-09-04 with the collision known and accepted.** Two GitHub repos already
carry the name, one of them a *"UDP multicast market-data feed handler in C++20"* — same
language, same transport. Judgment call: nobody searches GitHub for a portfolio project's
name, so the collision costs nothing in practice. Recorded here so the question doesn't
get reopened; it was asked and answered, not missed.

Alternatives that were verified clean if this is ever revisited: **Chronaut** (exact name
completely unclaimed — chrono + -naut, for a client that runs forward through simulation
time and a server that runs backward) and **Statecast** (8 repos, all <= 3 stars).

Names rejected outright — **the structural finding: every real English word with a good
metaphor is already owned by the domain that naturally uses that metaphor.**

| Name | Why rejected |
|---|---|
| **Snapline** | Game-cheat vocabulary — top hits are CS2/Unity ESP tools |
| **Backstep** | Owned by *backstepping*, a nonlinear control technique |
| **Parallax** | 44,569 repos, top 18,413 stars. It's a CSS scrolling effect |
| **Datum** | 2,606 repos, dominated by geodesy |
| **Ephemeris** | 1,598 repos, all astronomy/astrology |
| **Metronome** | 6,609 repos, all music apps |
| **Jiffy** | Crowded (Erlang JSON, Flutter dates); reads informal |
| **Arbiter** | 2,501 repos, top 746 stars |
| **Lockstep**, **Rollback** | Name real netcode architectures this project does *not* implement. Anyone who knows the field catches the mismatch |
| **Cadence** | Uber's workflow engine — known to exactly this audience |
| **Foreshadow** | A published CPU vulnerability (L1TF) |

---

## Why this project and not the RAG one

The RAG search stalled on **corpus** blockers — free, bulk, licensed prose with
non-circular ground truth barely exists outside institutional domains. Those
blockers are unfixable by effort. A systems project generates its own data, so
the entire A/B gate set from the checklist evaporates and only D0 (use case),
D3 (differentiation) and D4 (would you actually open this on a Saturday) bind.

The resume argument, from the three-track strategy doc:

- The C project it replaces is **Group Chat Server (CMPT 201)** — `select()`,
  pthreads, and a course code in the project name. It sits in **Track 2 slot 2**
  (the default track's second-most-read project) and **Track 1 slot 3**.
  Replacing a course project there is the biggest single slot upgrade in the bank.
- Track 2's dispatch row names **"C/C++"** and the bank contains **no C++ at all**.
  By the strategy doc's own per-application check ("a tool that appears only in
  the Skills block counts as absent"), C++ is currently absent everywhere. It is
  the one place a track claims something it cannot back.
- Serves **2 of 3 tracks**. The RAG project serves 1.
- **Demonware/Activision is already a named Track 1 target.** Direct hit.
- It is the only one of the two that is **de-risked** — zero data dependency,
  can start tomorrow.

## The idea

A C++20 **authoritative multiplayer game server**. Raw UDP, fixed-tick simulation,
native client. The game is deliberately trivial: top-down arena, circles, movement
plus one hitscan shot. No art, no sound, no menus.

The project is the **netcode**: client-side prediction, server reconciliation,
entity interpolation, and lag compensation via server-side rewind. All four are
named, recognised techniques — implementing the standard references, not inventing.

## Why it is not "a game project"

Two rules, held absolutely:

1. **If it needs an artist, it is out.**
2. **The game must be describable in one sentence.**

The moment this becomes a game instead of a server, it is lost. The renderer is
~30 lines of raylib on top of the headless load client that the benchmark needs
anyway.

## The demo

Two clients side by side and a **latency slider**.

Drag to 200 ms with prediction disabled — the character lurches a third of a
second behind the keys, visibly unplayable. Enable prediction — instantly smooth.
Toggle lag compensation and shots that *looked* like hits start registering.

Nobody needs that explained. It is the aha demo the RAG search never found.

The slider and the deterministic netcode tests are **the same mechanism** (see
the injectable transport below) — one abstraction, two payoffs.

## Hard stack requirements

| Requirement | How it is earned | Real? |
|---|---|---|
| **C++20** | `std::jthread` + `stop_token`; `std::span`/`string_view` for zero-copy parsing; concepts on the transport and queue interfaces. **Not `std::format`** — needs GCC 13, unavailable here; use **fmtlib** and say so | Real — the shutdown path can't be written this cleanly in C++11. **Requires g++-10; see Toolchain** |
| **CMake** | Target-based, `FetchContent` for GoogleTest, `CMakePresets.json`, sanitizer configs, `-Wall -Wextra -Werror` from commit one | Real if target-based |
| **POSIX sockets** | `socket`/`bind`, `recvmmsg`/`sendmmsg`, `O_NONBLOCK`, `EAGAIN`. **No Boost.Asio, no libevent** — they exist to hide exactly what is being demonstrated | Real, and an upgrade on `select()` |
| **pthreads / jthread** | `jthread` for room threads; `pthread_setaffinity_np` for CPU pinning, `pthread_setname_np` for readable `perf`/`gdb` output | Real — pinning genuinely cuts p99 variance |
| **GoogleTest** | Deterministic replay assertions, protocol edge cases, pure rewind logic; suite runs under every sanitizer config | Real, unusually strong |
| **Linux** | epoll (not POSIX), timerfd, TSan/ASan/UBSan, `perf`, affinity | Real |

**Deleting Group Chat Server deletes the `pthreads` keyword** — it is the only
entry in the bank carrying it. The affinity/naming calls above reclaim it honestly.

## Toolchain — verified on this machine 2026-09-04

**The default toolchain cannot build this project.** Verified, not assumed:

| Requirement | Default (GCC 9.4.0) | After `g++-10` |
|---|---|---|
| `-std=c++20` | ✗ rejected — only `-std=c++2a` | ✓ |
| `std::jthread` / `stop_token` | ✗ | ✓ |
| `std::span` | ✗ | ✓ |
| concepts | ✗ (`__cpp_concepts` undefined) | ✓ |
| `std::format` | ✗ | ✗ — needs GCC 13. **Use fmtlib** |
| **TSan** | ✗ `cannot find libtsan_preinit.o` | ✓ |
| ASan / UBSan | ✓ | ✓ |

**TSan is the serious one** — a clean TSan run is this project's central
correctness claim. Root cause: the installed `libtsan0` is the **GCC-10** build
(10.5.0) while the default compiler is GCC 9, so GCC 9's driver looks for a
runtime that was never installed for it.

**Fix — P0 task zero, before anything else:** `sudo apt install g++-10`
(in sources at `10.5.0-1ubuntu1~20.04`; `libtsan0`, `libasan6`, `libubsan1`
are already present at that version). CMake 3.16.3 does everything
load-bearing — `FetchContent` (3.11+), target-based linking, sanitizer
configs — so **`CMakePresets.json` is polish, not a requirement**; upgrade via
`pip3` or `snap` only if wanted (presets need 3.19+, v3 schema 3.21+).

**Pin the compiler version explicitly in CI.** GitHub Actions' `ubuntu-latest`
ships a far newer GCC than 10.5.0. Since this project's correctness rests on
bit-identical float behaviour between separately built binaries, letting CI
inherit the runner's compiler reintroduces the divergence class the `libsim`
flag rules exist to close — from a different direction. A pinned dev container
would make local and CI provably identical; decide at `/impl-plan` time.

## Architecture — settled

```
UDP socket ──> receiver thread (recvmmsg, parse header, route by room)
                    │
                    ├──> [ring] ──> room thread 0  (jthread, 60 Hz tick)
                    ├──> [ring] ──> room thread 1
                    └──> [ring] ──> room thread N
                                          │
              shared room registry ───────┘   ← the synchronized state TSan exercises
```

1. **Native raylib client, not browser.** Browsers cannot send raw UDP — the only
   options are WebSocket (TCP, kills the loss story), WebRTC DataChannel
   (ICE/DTLS/SCTP, and the library would hide the sockets) or WebTransport.
   A headless native load client is required for benchmarking regardless; nobody
   clones and builds a stranger's C++ project, so a README GIF does the sharing.
2. **Threading is designed in, not discovered.** The natural one-thread-per-room
   design has almost no shared state, so TSan would find nothing and the central
   correctness claim would evaporate. The shared room registry and the I/O↔sim
   handoff are what make a clean TSan run mean something.
3. **The queue is the benchmark:** mutex-guarded vs lock-free SPSC ring, measured.
4. **epoll multiplexes the UDP socket against a timerfd** for the tick.
5. **60 Hz simulation, 20 Hz snapshot send.** The mismatch is *what creates the
   need* for entity interpolation — the design motivates the technique.
6. **Determinism:** fixed timestep, injected clock, **no wall-clock time inside
   the simulation**. Same invariant as CallIt's `internal/domain` and the Redis
   `TIME`-inside-Lua rule. Floats with pinned flags (below); fixed-point is the
   fallback, not a purist's stretch goal.
7. **Reliability:** unreliable for inputs and snapshots (a stale input is
   worthless). Minimal seq/ack channel for join/leave only. No general-purpose
   reliability layer.

### `libsim` — the single most important structural decision

All simulation math lives in **one static library linked identically into both
the server and the client binaries**. No sockets, no I/O, no allocation.

Client and server must run bit-identical movement logic or reconciliation
oscillates. With `libsim`, a prediction divergence is *by construction* a network
or tick-sync bug, never a math discrepancy — which collapses the P3 debugging
surface, and P3 is the phase most likely to kill this project.

**Pin one explicit `-march` across every target and configuration, and never
use `-march=native`.** `-ffp-contract=off` on top, as defense in depth.

This is the corrected rule. Measured on this machine (objdump for `vfmadd`):

| Flags | FMA emitted |
|---|---|
| `-O2` | 0 |
| `-O2 -march=native` | **1** |
| `-O2 -march=native -ffp-contract=off` | 0 |

Contraction only happens when the target actually *has* FMA — the baseline
x86-64 target has no FMA instruction to contract into. So the real determinism
hazard is **`-march=native`**, exactly the flag someone adds to a Release
build for benchmark speed, and exactly the build whose numbers are the
deliverable. Client and server are separate binaries; they can diverge from
rounding alone while both are "correct."

*Caveat, deliberately not overstated: this demonstrates an instruction-selection
difference, not an observed numerical divergence. Attempts to construct
differing values did not expose one.*

There is no `-fno-fast-math` to add — fast-math is **off by default**. The rule
is simply never to add `-ffast-math`.

### Interface contracts — hand off by index, never by value

The zero-allocation rule below is an **interface** constraint, not just an
allocator choice. Three shapes are forbidden because each silently violates it:

| Forbidden | Why |
|---|---|
| `receive() -> optional<pair<Endpoint, vector<byte>>>` | allocates per packet |
| `SpscRing::push(T v)` / `try_pop() -> optional<T>` | copies ~1200 bytes per hop — **P5's mutex-vs-lock-free benchmark would measure `memcpy`, not synchronization**, and the benchmark is the deliverable |
| `WorldSnapshot { vector<PlayerState> }` | allocates inside `libsim`, which declares no allocation |

All three must hand off an **index or a `span` into a preallocated slot**.

**`SpscRing`'s contract must be stated, not implied:** fixed power-of-two
capacity, index masking, and whether indices are monotonic or wrapped — these
determine the ordering guarantee and cannot be left to the implementer.

**`Endpoint { uint32_t addr }` commits to IPv4.** Fine, but state it as a
decision rather than letting it be discovered.

### The determinism test needs two binaries, not two builds

"Two separately-compiled instances of `libsim` (Debug and Release) in one test
binary" is an **ODR violation** and will not build. It has to be **two
executables plus a CTest fixture** that runs both and compares output. A cold
executing window — which the workflow guide requires to run without
conversation history — hits this in minute one.

### Injectable transport — built at P1, not retrofitted

Transport is an interface: a real UDP implementation, and a deterministic
simulated one injecting configurable latency, jitter, loss and reordering.

You cannot unit-test packet loss against a real socket. This makes netcode tests
deterministic **and** powers the latency slider demo — one abstraction, both
payoffs, plus a legitimate C++20 concepts use.

**`SimulatedTransport` is only deterministic if it is specified to be.** It needs
a **seeded PRNG** (never `random_device`) and an **injected tick source** to
schedule delayed delivery. Without both, "deterministic simulated transport" is
a claim the implementation cannot honor.

### Packet handling

Fixed-size preallocated slots, **no per-packet heap allocation on the hot path**.
Allocate-on-receiver / free-on-room-thread is the pathological pattern for
arena-based allocators. This is free: a ring of fixed-size slots is already what
an SPSC ring buffer is.

Size slots under **~1200 bytes** — above that invites IP fragmentation, and a
fragmented UDP datagram is lost entirely if any single fragment drops.

### Egress

Room threads call `sendmmsg` directly. Measure tick jitter; add an egress queue
only if the numbers demand it. Socket send contention bites at hundreds of
thousands of packets/sec, roughly three orders of magnitude above 20 Hz × player
count. "I measured before optimising" is both better engineering and a better
interview answer than a preemptive queue layer.

> Note: `SO_REUSEPORT` is **not** the fix here. It shares one port across sockets
> and the kernel hashes by client 4-tuple, not by room — reuseport socket *i* will
> not hold room *i*'s players, so the routing step remains either way.

## Build order

Each phase independently shippable. Stopping after any of them still leaves a
complete project.

| Phase | Scope | Risk |
|---|---|---|
| **P0** | **Task zero: install and pin `g++-10`, verify TSan links.** Then skeleton: CMake, GoogleTest, CI, three sanitizer configs, on an empty binary | Low — but do it *first*, on purpose |
| **P1** | Wire protocol, serialization, **injectable transport**. **Header must carry tick / timestamp / ack fields** — see below | Medium — the wire format is frozen here |
| **P2** | Authoritative server, single-threaded. Visibly laggy, and that's correct. **First demoable thing** | Medium — scope creep |
| **P3** | Client prediction + reconciliation + clock-sync *logic* (fields already reserved at P1) | **HIGHEST — this is where it dies** |
| **P4** | Entity interpolation + snapshot delta | Medium |
| **P5** | Threading + queue benchmark | Medium — lock-free memory ordering is subtle |
| **P6** | Lag compensation (server rewind) | High, but localized and additive |
| **P7** | Stretch: io_uring backend, or WebSocket gateway | — |

**Build the I/O↔simulation seam at P2 even while single-threaded**, so P5 swaps
an implementation rather than restructuring — and yields a real before/after
measurement instead of an asserted one.

**P3 is the death phase**, not P6. Every determinism debt comes due at once, and
its failures are diffuse ("it jitters sometimes") while P6's are specific and
localized ("hit registered at the wrong position"). P3 also hides an unlisted
subsystem: **clock synchronization** — prediction requires the client to estimate
server tick and deliberately run *ahead*, meaning RTT estimation and drift
correction.

**But clock sync is a P1 decision, not a P3 one.** RTT estimation and drift
correction need **tick, timestamp and ack fields in the packet header**. If P1
freezes the wire format without them, P3 must reopen the protocol — the exact
"answered badly inside a phase plan" failure the `/impl-plan` layer exists to
prevent. Reserve the fields at P1; implement the logic at P3.

## The headline artifact

A numbers table, the same role the ablation table played in the RAG plan:

- tick jitter — p50 / p99 / max deviation from 16.67 ms
- concurrent players before jitter exceeds budget
- queue handoff latency: mutex vs lock-free SPSC
- packet throughput

Plus the latency-slider GIF in the README.

## Known blockers — all resolved or accepted

| Blocker | Resolution |
|---|---|
| Browsers can't send UDP | Native raylib client; README GIF for sharing |
| One-thread-per-room has no shared state → TSan finds nothing | Concurrency designed in deliberately (registry + I/O handoff) |
| Learning C++ while building; CMake is a time sink | P0 first, in isolation. No raw `new`/`delete`, value semantics by default |
| Lag comp is where these stall | It's last, and P1–P5 stand alone |
| Determinism can't be retrofitted | `libsim` + injected clock + `-ffp-contract=off`, from day one |
| Live UDP hosting unsupported on most PaaS | Accepted — local demo plus recorded video |

**None of these is fatal.** Contrast the RAG search, where the blockers were
corpus blockers: unfixable by effort, because the data either existed or didn't.
Every one of these is a design decision made early or paid for later.

## Differentiation — honest read

"C++ multiplayer game server" on GitHub is **saturated** — mostly SFML or
Boost.Asio toys with no tests and no netcode depth.

The differentiator is **not the idea, it is the rigor**: sanitizers in CI,
deterministic replay tests, a measured queue comparison, and `libsim` guaranteeing
client/server agreement. That has to be the pitch. Gemini claimed "top 1%
saturation" — that is a fabricated statistic and must never reach a README.

## Open questions — inputs for `/impl-plan`

These are architectural and span phases. `/impl-plan` resolves them **once**,
before P0's phase plan is written; answering them inside a phase plan is how
they get answered badly.

- **Fixed-point vs floats.** Starting with floats plus pinned `-march` and
  `-ffp-contract=off`; revisit if P3 reconciliation proves unstable. Fixed-point
  removes the whole hazard class by construction — the cost is vector
  normalization (sqrt).
- **`libsim`'s exact API surface and link model** — static library, and which
  types cross the boundary.
- **The transport interface's shape.** Built at P1 but constrains P3 and the
  demo.
- **`SpscRing`'s ordering contract** — capacity, masking, monotonic vs wrapped
  indices, and the memory-ordering guarantee.
- **Pinned dev container, yes or no.** The cheapest way to make local and CI
  provably identical given the toolchain findings above.
- Whether P7's WebSocket gateway is worth building for shareability, or whether
  the GIF is genuinely sufficient.

## Day one

**`sudo apt install g++-10`, then verify TSan links.** Nothing in this document
builds until that is done, and the project's central correctness claim is dead
until TSan runs.

**Then `/impl-plan`** — the phase table above is a table, not an implementation
plan, and the open questions above are its inputs.

**Then P0.** It is the least interesting phase and the one most likely to
derail the project if hit later while also debugging netcode.

Read the canonical references **before P3, not during**: Glenn Fiedler's
Gaffer On Games articles on snapshot interpolation and networked physics, and
Valve's Source Multiplayer Networking page for lag compensation. These are the
standard references every interviewer in this space knows.
