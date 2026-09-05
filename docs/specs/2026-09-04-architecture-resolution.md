# Tickwire — Architecture Resolution (`/impl-plan` output)

**Status:** proposed, awaiting confirmation · **Date:** 2026-09-04
**Resolves:** the five open questions in
[`2026-09-04-tickwire-design.md`](2026-09-04-tickwire-design.md) § Open questions
**Consumed by:** P0's `writing-plans` window. Everything needed to execute is
written here; no conversation history is required.

Every environment claim below was **measured on this machine**, not assumed.
Where a check failed to prove what it looked like it proved, that is said plainly.

---

## Q1 — Fixed-point vs floats

**Resolved: floats.** Fixed-point stays the documented fallback.

What changes from the design doc is the **revisit trigger**. "Revisit if P3
reconciliation proves unstable" is not actionable — the doc itself says P3's
failures are diffuse ("it jitters sometimes"), so the symptom cannot be
attributed to float behaviour without already suspecting it.

**The trigger is the cross-binary determinism test going red**, and that test
exists from P0. A red run that is not explained by a flag mismatch *is* the
fixed-point signal. Binary, attributable, and it fires before P3 rather than
during it.

**Scope the claim honestly.** What the pinned flags buy is bit-identical
behaviour **between two binaries built by the same pinned compiler for the same
pinned architecture**. That is all the project needs — client and server both
run locally. It is *not* cross-platform determinism, and the README must never
say lockstep or cross-platform. (Same discipline as the design doc's rejection
of the fabricated "top 1% saturation" claim.)

## Q2 — `libsim` API surface and link model

### Link model

`libsim` is a CMake **STATIC** target with zero dependencies. The
"linked identically into both binaries" requirement is enforced by the **build
graph, not by discipline**:

```cmake
add_library(tickwire_sim_flags INTERFACE)
target_compile_options(tickwire_sim_flags INTERFACE
    -march=x86-64             # pinned baseline; NEVER -march=native
    -ffp-contract=off)        # defense in depth
target_compile_features(tickwire_sim_flags INTERFACE cxx_std_20)

add_library(libsim STATIC ...)
target_link_libraries(libsim PUBLIC tickwire_sim_flags)   # PUBLIC => propagates
```

Because the flags are `PUBLIC` on `libsim`, every consumer inherits them
automatically. A binary cannot link `libsim` with different float flags without
deliberately fighting the build system.

### Surface

Only trivially-copyable POD crosses the boundary — no `std::string`, no
allocating containers. This is what keeps "no allocation" honest and makes
serialization a memcpy of a fixed layout.

```cpp
namespace sim {
inline constexpr float    kTickDt     = 1.0f / 60.0f;  // never derived from a clock
inline constexpr uint32_t kMaxPlayers = 32;            // see MTU derivation below

struct PlayerState  { uint32_t id; float x, y, vx, vy; float radius; };   // 24 B
struct InputCommand { uint32_t player_id, tick; float move_x, move_y; bool fire; };

struct WorldSnapshot {                       // fixed capacity - no vector
  uint32_t tick;
  uint32_t count;
  std::array<PlayerState, kMaxPlayers> players;
};

class World {
 public:
  void applyInput(const InputCommand&);
  void step();                                    // advances exactly kTickDt, no args
  void writeSnapshot(WorldSnapshot& out) const;   // caller-owned buffer, no return-by-value
  std::optional<uint32_t> resolveHitscan(uint32_t shooter, float aim_x, float aim_y) const;
};
}  // namespace sim
```

`step()` deliberately **takes no delta-time argument** — that is the enforcement
mechanism for "no wall-clock inside the simulation." A caller wanting variable
framerate must accumulate and call `step()` N times; it cannot scale a timestep.

### `kMaxPlayers = 32` is derived, not chosen

It falls out of the design doc's own ~1200-byte MTU limit:

| Players | Snapshot bytes | + ~20 B header | Fits under 1200? |
|---|---|---|---|
| 32 | 768 | 788 | yes, with margin |
| 48 | 1152 | 1172 | yes, no margin |
| 64 | 1536 | 1556 | **no** |

So 48 is the hard ceiling for full snapshots and 32 is the working value.
Exceeding it is exactly what **P4's snapshot delta** buys — which means P4 has a
concrete numeric justification rather than being a technique applied because the
reference articles mention it. (`radius` is static per player and is the first
field to drop from a delta.)

## Q3 — Transport interface shape

```cpp
struct Endpoint { uint32_t addr_be; uint16_t port_be; };   // IPv4, network byte order
inline constexpr size_t kMaxPacket = 1200;

struct PacketSlot {
  Endpoint peer;
  uint16_t len;
  std::array<std::byte, kMaxPacket> data;
};

template <typename T>
concept Transport = requires(T t, const Endpoint& ep,
                             std::span<const std::byte> out, PacketSlot& slot) {
  { t.send(ep, out)     } -> std::same_as<bool>;   // false = would block / dropped
  { t.tryReceive(slot)  } -> std::same_as<bool>;   // fills caller-owned slot
};
```

`tryReceive(PacketSlot&)` fills a caller-owned slot. This is the shape that
satisfies the design doc's forbidden-shapes table — no `optional<vector<byte>>`,
no allocation, no copy.

**`Endpoint` commits to IPv4**, recorded as a decision. `_be` suffixes make byte
order part of the type name, since mixing host and network order silently is the
classic bug here.

### `SimulatedTransport` is a wrapper, not a sibling

The design doc asserts "one abstraction, two payoffs" without giving the
mechanism. The mechanism is **composition**:

```cpp
template <Transport Inner> class SimulatedTransport { /* satisfies Transport */ };
```

- Wrapping an **in-memory loopback** → fully deterministic netcode tests.
- Wrapping **`UdpTransport`** → the latency slider, adding artificial delay on
  top of real localhost traffic.

One implementation serves both, which is what makes the claim true.

**Determinism requires two things the doc names and one it does not:**

1. Seeded `std::mt19937_64` — never `random_device`.
2. An injected tick source.
3. **Latency and jitter are converted from milliseconds to ticks at construction
   time**, and the delivery schedule is integer tick arithmetic. `SimConfig`
   takes ms because the slider is human-facing, but if ms survive into the
   schedule, wall-clock dependence re-enters through the back door — the exact
   thing `libsim` is built to exclude.

## Q4 — `SpscRing` ordering contract

```cpp
template <typename T, size_t N>
class SpscRing {
  static_assert((N & (N - 1)) == 0, "N must be a power of two");
  alignas(64) std::atomic<uint64_t> write_{0};
  alignas(64) std::atomic<uint64_t> read_{0};
  alignas(64) std::array<T, N> buf_;
 public:
  T*   acquireWrite();   // producer: slot pointer, or nullptr if full
  void commitWrite();    // producer: write_.store(w + 1, release)
  T*   acquireRead();    // consumer: slot pointer, or nullptr if empty
  void commitRead();     // consumer: read_.store(r + 1, release)
};
```

The contract, stated rather than implied:

- **Capacity** is a power of two, `static_assert`ed; slot index is `idx & (N-1)`.
- **Indices are monotonic `uint64_t`, never wrapped.** Masking happens only at
  access time. This makes full (`write - read == N`) and empty (`== 0`)
  unambiguous without sacrificing a slot, and `uint64` cannot overflow in any
  realistic runtime.
- **Ordering:** producer loads `read_` *acquire*, stores `write_` *release*;
  consumer loads `write_` *acquire*, stores `read_` *release*. **No `seq_cst` on
  the hot path.** This is the pairing TSan verifies.
- **`acquire`/`commit` split instead of `push(T)` / `pop() -> T`** — writes land
  directly in the slot, so the ring is allocation-free *and* copy-free.
- **`alignas(64)` on both indices and the buffer.** Without padding, the two
  indices share a cache line and the benchmark measures cache-line ping-pong
  rather than synchronization cost — the same class of error as the `memcpy`
  problem the design doc already caught, and the benchmark **is** the deliverable.

## Q5 — Pinned dev container

**Resolved: yes, and it replaces `sudo apt install g++-10` as P0 task zero.**

Verified on this machine: Docker 28.4.0, daemon reachable, **runs without sudo**.
`apt install` needs a password that is not available to an automated session;
`docker run` does not. The container therefore removes the blocker rather than
working around it.

It also closes the CI-pinning hole with the same artifact: CI runs the same
image, so local and CI are *provably* identical rather than approximately so.
This matters because pinning `-march` only guarantees determinism if the
**compiler** is pinned too — host g++-10 plus CI's newer GCC is still two
compilers.

### Verified inside `gcc:10`

| Check | Result |
|---|---|
| `g++ --version` | GCC **10.5.0** |
| `std::jthread` / `std::span` / concepts | **YES** (all three) |
| `std::format` | NO — expected; use **fmtlib** |
| ASan + UBSan | **OK** |
| `cmake` | **absent from the image** — the derived Dockerfile must add it, and must **pin ≥ 3.21**; see below |

### CMake ≥ 3.21 is a correctness requirement, not polish

Debian 11 ships **CMake 3.18.4**. On 3.18, `ctest --test-dir <dir>` — the exact
scoped command the dev-workflow guide § 6 prescribes for every checkpoint —
**does not exist**, and ctest does not error on it. It runs **zero tests, prints
`No tests were found!!!`, and exits 0.**

Measured. Every TDD checkpoint would report green having executed nothing.

This reverses the design doc's "presets are polish" conclusion, which was
correct *for the host* (CMake 3.16, unupgradable without sudo) and wrong for a
container we control. **Pin CMake 3.28.4 in the image** via the Kitware release
tarball (`gcc:10` has no `pip3`). Verified: `cmake 3.28.4`, `--test-dir`
supported. This also restores `CMakePresets.json` (3.21+) as genuinely available.

**The workflow guide's test command is therefore valid — but only because the
image pins CMake.** The two are coupled; changing the image's CMake floor breaks
the documented convention silently.

### TSan needs a specific invocation — record it or it will look broken

TSan **compiles and links but aborts at runtime** in the container:
`FATAL: ThreadSanitizer: unexpected memory mapping`. Measured combinations:

| Invocation | TSan |
|---|---|
| plain `docker run` | **fails** |
| `--cap-add SYS_PTRACE --security-opt seccomp=unconfined` | **fails** |
| `setarch -R` alone | fails — `personality()` blocked by seccomp |
| **both together** | **works** |

```
docker run --rm --cap-add SYS_PTRACE --security-opt seccomp=unconfined \
  --user $(id -u):$(id -g) -v "$PWD":/w -w /w tickwire-dev \
  setarch -R ctest --preset tsan
```

`setarch -R` (disable ASLR) **also works on the host**, unprivileged — the
container's seccomp profile is what blocks it there. So **wrap the TSan test
command in `setarch -R` regardless of route**; it is not a container artifact.

**Bake this into the CTest/CMake configuration, not into a human's memory.**
Discovering it a third time costs another session.

### Residual risks, not papered over

1. **`--user $(id -u):$(id -g)` is mandatory.** Without it the bind mount
   produces **root-owned build artifacts on the host** — observed, not predicted.
2. **The raylib GUI client is the open question.** Everything
   determinism-critical (server, `libsim`, tests, CI) runs in the container
   cleanly. The client also links `libsim`, so it is determinism-critical too —
   but it needs a display. This is WSL2 with WSLg (`DISPLAY=:0`,
   `WAYLAND_DISPLAY=wayland-0`, `/tmp/.X11-unix` present), so X11 passthrough is
   plausible. **Validate it at P2, do not assume it.** Fallback: build the
   client on the host against the same pinned flags and accept a weaker
   guarantee for the renderer only.

## New findings from verification

- **raylib is not packaged for Ubuntu 20.04** — `apt-cache search raylib` returns
  nothing. It must come via `FetchContent` from source. Its X11/GL build
  dependencies (`libgl1-mesa-dev`, `libx11-dev`, `libxrandr-dev`, `libxi-dev`,
  `libxcursor-dev`, `libxinerama-dev`) are **already installed on the host**, but
  must be added to the Dockerfile if the client builds in the container.
- **`-march=x86-64-v2` does not compile on GCC 10** — micro-architecture levels
  (`v2`/`v3`/`v4`) were added in GCC 11. Caught by an actual build, not review.
  Valid values here: `x86-64` (chosen), or named micro-architectures such as
  `nehalem`, `haswell`.
- **`ctest --test-dir` is a silent false-green on Debian's CMake 3.18** — see the
  CMake floor section above. This is why the image pins 3.28.4.
- **`gcc:10` is Debian 11 with CMake 3.18.4 and git 2.30.2, and has no `pip3`** —
  so a newer CMake comes from the Kitware release tarball, not pip.
- **All three sanitizers actively detect their bug classes** in the pinned image,
  verified with deliberate faults: ASan caught a heap-use-after-free, UBSan a
  signed-integer overflow, TSan a data race (under `setarch -R`). This is what
  makes the "prove the sanitizer is live" tests in P0 worth writing — a
  configured-but-inactive sanitizer is otherwise indistinguishable from a clean run.
- **GitHub is reachable**, so `FetchContent` for GoogleTest/fmtlib/raylib works
  at configure time. Verified end to end: GoogleTest `release-1.12.1` fetched,
  built, and ran green inside the container.
- **`recvmmsg`/`sendmmsg` compile** against glibc 2.31.
- **16 cores available** — ample for room threads plus affinity pinning.

## Revised P0 task zero

Replaces "`sudo apt install g++-10`":

1. Write `Dockerfile` — `FROM gcc:10`, add **CMake 3.28.4 from the Kitware
   release tarball** (not apt — Debian's 3.18 is a silent false-green; no `pip3`
   in the image), `ninja-build`, `util-linux` (for `setarch`), and raylib's
   X11/GL deps.
2. Write the run wrapper carrying `--cap-add SYS_PTRACE`,
   `--security-opt seccomp=unconfined`, `--user $(id -u):$(id -g)`.
3. Prove the toolchain **inside** the container: C++20 compiles, and **all three
   sanitizers run green on an empty binary** — TSan under `setarch -R`.
4. Only then: CMake skeleton, GoogleTest via `FetchContent`, CI on the same image.
5. The two-binary determinism test plus its CTest fixture ships in P0, not later.

The host `sudo apt install g++-10` route stays available if a password is at
hand, but it is **unverified for TSan** and leaves CI pinning unsolved.

## Risks

| Risk | Likelihood | Mitigation |
|---|---|---|
| raylib client cannot get a display from the container | Medium | Validate at P2; fallback is host build of the renderer only |
| TSan invocation gets lost and TSan looks broken again | Medium | Bake into CTest config in P0 — the reason this section exists |
| The pinned `-march` value is unsupported by the pinned compiler | Resolved | Was real: `x86-64-v2` **does not exist in GCC 10** (micro-arch levels landed in GCC 11) and failed the build. Now `-march=x86-64` — verified to compile, and baseline x86-64 has no FMA at all, so it closes the contraction hazard by itself |
| `kMaxPlayers = 32` proves too small for the demo | Low | P4's snapshot delta is the designed answer; 48 is the full-snapshot ceiling |
| Container adds friction that erodes the habit | Medium | Wrapper script from day one; never type raw `docker run` |

## Still open — deliberately not decided here

- **P7's WebSocket gateway** vs the README GIF. No information available until
  the demo exists; deciding now would be guessing.
*(The `-march` value is no longer open — see the risks table. `x86-64` is
decided and verified.)*
