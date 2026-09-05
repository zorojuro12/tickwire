# Tickwire — Project Conventions

Verified constraints from P0. Every rule here was hit as a real failure during
toolchain verification or plan execution, not assumed.

## Build and test

- **Never invoke the host `g++`, `cmake`, or `ctest`.** The host has GCC 9.4
  and CMake 3.16 and cannot build this project (CMake 3.16 in particular is
  below the 3.21 floor — see below). All build/test commands go through
  `scripts/tw <command...>`, which runs `<command...>` inside the pinned
  `tickwire-dev:gcc10-cmake3.28.4` image with the repo bind-mounted at
  `/work`.
- **CMake floor is 3.21+ (the image pins 3.28.4).** On Debian's packaged
  CMake 3.18, `ctest --test-dir` runs **zero tests and exits 0** — a silent
  green. Never install CMake from apt in the image.
- Configure/build/test three configurations as needed: `build/plain`,
  `build/asan` (`-DTW_SANITIZER=address,undefined`), `build/tsan`
  (`-DTW_SANITIZER=thread`). `scripts/ci.sh` runs all three plus the
  toolchain assertions in one call — it is what CI calls, so it is always
  reproducible locally.

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

## Specs

- [`docs/specs/2026-09-04-tickwire-design.md`](docs/specs/2026-09-04-tickwire-design.md) — the design
- [`docs/specs/2026-09-04-architecture-resolution.md`](docs/specs/2026-09-04-architecture-resolution.md) — authoritative for every architectural decision
