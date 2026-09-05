# P0 — Toolchain & Skeleton Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use the `executing-plans` skill to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Stand up a pinned, reproducible C++20 build in which all three sanitizers are provably active and two separately-built binaries provably agree bit-for-bit — before any netcode exists.

**Architecture:** Everything builds and runs inside a pinned Docker image (`gcc:10` + CMake 3.28.4), driven through a single wrapper script so the required capability flags can never be forgotten. A `tickwire_sim_flags` INTERFACE target carries the float-determinism flags and propagates them `PUBLIC` through `libsim`, making "identical flags in both binaries" a build-graph property rather than discipline. Sanitizer configurations are validated by deliberate faults, so a misconfigured sanitizer fails loudly instead of looking clean.

**Tech Stack:** C++20 (GCC 10.5.0), CMake 3.28.4, GoogleTest (FetchContent, `release-1.12.1`), Docker, ASan/UBSan/TSan, CTest.

**Spec:**
- [`docs/specs/2026-09-04-tickwire-design.md`](../specs/2026-09-04-tickwire-design.md) — the design
- [`docs/specs/2026-09-04-architecture-resolution.md`](../specs/2026-09-04-architecture-resolution.md) — **authoritative for every decision below**

**Plan format:** spec-driven (inline execution). Exception: Dockerfile, CMake, and
CI files are given verbatim — they *are* the deliverable, not code derived from a
contract, and their exact content is what was verified.

## Global Constraints

No `CLAUDE.md` exists yet (it is written at the end of this phase), so everything
required is stated here.

- **Never invoke the host `g++`, `cmake`, or `ctest`.** The host has GCC 9.4 and CMake 3.16 and cannot build this project. All build/test commands go through `scripts/tw`.
- **Container invocation is fixed:** `--cap-add SYS_PTRACE --security-opt seccomp=unconfined --user $(id -u):$(id -g)`. Without the caps, TSan aborts with `unexpected memory mapping`. Without `--user`, the bind mount writes **root-owned files onto the host**.
- **TSan test runs are wrapped in `setarch -R`.** Neither the caps nor `setarch` alone is sufficient; both are required.
- **CMake floor is 3.21+ (image pins 3.28.4).** On Debian's CMake 3.18, `ctest --test-dir` runs **zero tests and exits 0** — a silent green. Never install CMake from apt in the image.
- **Float flags, on every target that links `libsim`:** `-march=x86-64` and `-ffp-contract=off`. **Never** `-march=native` (it enables FMA contraction). **Never** `-ffast-math`. `-march=x86-64-v2` **does not exist in GCC 10** — do not use it.
- **No `std::format`** — GCC 10 lacks it. Use fmtlib if formatting is needed (not needed in P0).
- **`libsim` has no I/O, no wall-clock reads, and no allocation.** Only trivially-copyable POD crosses its boundary.
- **Forbidden interface shapes** (from the resolution doc): returning `optional<vector<byte>>` from a receive call; `push(T)`/`pop() -> optional<T>` on the ring; a `WorldSnapshot` holding a `vector`. Hand off by index or `span` into a preallocated slot.
- **Warnings are errors:** `-Wall -Wextra -Werror` from the first commit.
- **Commits:** `type: description` (feat, fix, refactor, docs, test, chore, perf, ci). One commit per checkpoint, chained behind its test with `&&` — never `;`, never a separate line. `git add` names exact paths; **never** `git add -A` or `git add .`.
- **Test scoping:** inside a checkpoint use `-R <regex>`; run the full suite only at a task boundary. Never run the full suite inside a checkpoint.
- **Coverage:** 80% project-wide; `libsim` is held to 100%. P0's `libsim` is deliberately tiny — see Task 2.
- **File size:** 200–400 lines typical, 800 hard maximum.

---

## File Structure

| File | Responsibility |
|---|---|
| `Dockerfile` | Pinned toolchain image: GCC 10.5.0 + CMake 3.28.4 + `setarch` |
| `scripts/tw` | The only way commands enter the container; owns the capability flags |
| `scripts/verify-toolchain.sh` | Executable assertions about the toolchain — the regression pin for every finding in the resolution doc |
| `scripts/ci.sh` | Runs every configuration; CI is a thin caller of this |
| `scripts/compare-digests.sh` | Determinism comparator — exits non-zero on mismatch |
| `CMakeLists.txt` | Root: `tickwire_sim_flags`, `TW_SANITIZER` option, FetchContent, subdirs |
| `src/sim/sim.h`, `src/sim/sim.cpp` | `libsim` — minimal in P0, expanded at P2 |
| `tests/smoke_test.cpp` | Proves the GoogleTest harness runs |
| `tests/faults/{uaf,ovf,race}.cpp` | Deliberate faults proving each sanitizer is live |
| `tools/digest_dump.cpp` | Emits a digest of N simulation steps; built twice |
| `.github/workflows/ci.yml` | Thin caller of `scripts/ci.sh` |

---

## Task 1: Pinned toolchain container

**Files:**
- Create: `Dockerfile`, `scripts/tw`, `scripts/verify-toolchain.sh`
- Test: `scripts/verify-toolchain.sh` (it is both the test and the invariant)

**Interfaces:**
- Produces: `scripts/tw <command...>` — runs `<command...>` in the image with the repo bind-mounted at `/work`. Every later task's commands are prefixed with it.
- Produces: image tag `tickwire-dev:gcc10-cmake3.28.4`.

**Checkpoint 1: the toolchain script rejects a compiler that cannot build the project**

- [ ] **Step 1: Write the failing test, then run it**

Write `scripts/verify-toolchain.sh`. It must exit non-zero with a named reason if any of these is false, and print one `OK:` line per satisfied check:
- `g++ -dumpversion` major version is exactly `10`
- a C++20 probe compiles with `-std=c++20` and reports `__cpp_lib_jthread`, `__cpp_lib_span`, and `__cpp_concepts` all defined
- `cmake --version` is ≥ 3.21
- `command -v setarch` succeeds

Run: `bash scripts/verify-toolchain.sh`
Expected: FAIL — on the host this exits non-zero at the first check, reporting GCC major `9`, not `10`. (This failure is the point: it is the finding from the resolution doc turned into an executable assertion.)

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract — `Dockerfile`, verbatim (this exact content was verified):

```dockerfile
FROM gcc:10

# Debian 11 ships CMake 3.18, on which `ctest --test-dir` runs zero tests and
# exits 0 - a silent green. The version floor is a correctness requirement.
ARG CMAKE_VERSION=3.28.4
RUN apt-get update && apt-get install -y --no-install-recommends \
      ninja-build util-linux ca-certificates curl git \
    && curl -fsSL "https://github.com/Kitware/CMake/releases/download/v${CMAKE_VERSION}/cmake-${CMAKE_VERSION}-linux-x86_64.tar.gz" \
       | tar -xz --strip-components=1 -C /usr/local \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /work
```

Contract — `scripts/tw`, verbatim. Note this checkpoint deliberately ships it
**without** the TSan capability flags; Checkpoint 2 adds them and explains why.

```bash
#!/usr/bin/env bash
set -euo pipefail
IMAGE=tickwire-dev:gcc10-cmake3.28.4
docker image inspect "$IMAGE" >/dev/null 2>&1 || docker build -t "$IMAGE" "$(dirname "$0")/.."
exec docker run --rm \
  --user "$(id -u):$(id -g)" \
  -v "$PWD":/work -w /work \
  "$IMAGE" "$@"
```

Make both scripts executable (`chmod +x`).

```bash
scripts/tw bash scripts/verify-toolchain.sh && \
  git add Dockerfile scripts/tw scripts/verify-toolchain.sh && \
  git commit -m "chore: pin toolchain container and add toolchain assertions"
```

Expected: PASS — every check prints `OK:`, then one commit.

**Checkpoint 2: TSan runs, which requires capability flags the wrapper does not yet pass**

- [ ] **Step 1: Write the failing test, then run it**

Extend `scripts/verify-toolchain.sh` with a fourth check: compile a trivial
two-thread program with `-fsanitize=thread` and **run it under `setarch -R`**,
asserting exit status 0.

Run: `scripts/tw bash scripts/verify-toolchain.sh`
Expected: FAIL — `setarch: failed to set personality to (null): Operation not permitted`. The container's default seccomp profile blocks the `personality()` syscall.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: add `--cap-add SYS_PTRACE --security-opt seccomp=unconfined` to the
`docker run` line in `scripts/tw`, immediately above `--user`. Add a comment
naming both symptoms — that without the flags TSan aborts with
`unexpected memory mapping`, and that `setarch -R` alone fails with
`Operation not permitted`.

```bash
scripts/tw bash scripts/verify-toolchain.sh && \
  git add scripts/tw scripts/verify-toolchain.sh && \
  git commit -m "chore: grant the container the capabilities TSan requires"
```

Expected: PASS — all four checks green, then one commit.

**Task boundary:** `scripts/tw bash scripts/verify-toolchain.sh` — full script, green.

---

## Task 2: CMake skeleton, `libsim`, and GoogleTest

**Files:**
- Create: `CMakeLists.txt`, `src/sim/sim.h`, `src/sim/sim.cpp`, `tests/smoke_test.cpp`
- Modify: `.gitignore` (add `build/`)

**Interfaces:**
- Produces: CMake target `tickwire_sim_flags` (INTERFACE) carrying `-march=x86-64 -ffp-contract=off -Wall -Wextra -Werror` and `cxx_std_20`.
- Produces: CMake target `libsim` (STATIC), linking `tickwire_sim_flags` **PUBLIC** so consumers inherit the flags.
- Produces: CMake cache option `TW_SANITIZER` (string, default empty).
- Produces: `namespace sim { inline constexpr float kTickDt = 1.0f/60.0f; float advance(float pos, float vel); }` in `src/sim/sim.h`.

> `libsim` is deliberately minimal in P0. `World`, `PlayerState`, `WorldSnapshot`
> and the rest of the surface in the resolution doc arrive at P2. `advance()`
> exists because Task 4's determinism harness needs a deterministic computation
> to digest; it is the smallest honest one.

**Checkpoint 1: the build configures, compiles, and runs a test**

- [ ] **Step 1: Write the failing test, then run it**

Write `tests/smoke_test.cpp` with one GoogleTest case: `sim::advance(1.0f, 2.0f)`
called twice returns values that compare equal, and `sim::kTickDt` equals
`1.0f/60.0f`.

Run: `scripts/tw cmake -S . -B build/plain -DCMAKE_BUILD_TYPE=RelWithDebInfo`
Expected: FAIL — CMake errors with `does not appear to contain CMakeLists.txt` (no root `CMakeLists.txt` exists yet).

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract — root `CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.21)
project(tickwire CXX)

include(FetchContent)
FetchContent_Declare(googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG release-1.12.1)
FetchContent_MakeAvailable(googletest)

set(TW_SANITIZER "" CACHE STRING "address,undefined | thread | (empty)")

add_library(tickwire_sim_flags INTERFACE)
target_compile_features(tickwire_sim_flags INTERFACE cxx_std_20)
target_compile_options(tickwire_sim_flags INTERFACE
  -march=x86-64 -ffp-contract=off -Wall -Wextra -Werror)
if(TW_SANITIZER)
  target_compile_options(tickwire_sim_flags INTERFACE
    -fsanitize=${TW_SANITIZER} -g -fno-omit-frame-pointer)
  target_link_options(tickwire_sim_flags INTERFACE -fsanitize=${TW_SANITIZER})
endif()

add_library(libsim STATIC src/sim/sim.cpp)
target_include_directories(libsim PUBLIC src)
target_link_libraries(libsim PUBLIC tickwire_sim_flags)

enable_testing()
add_executable(smoke_test tests/smoke_test.cpp)
target_link_libraries(smoke_test PRIVATE libsim GTest::gtest_main)
add_test(NAME smoke_test COMMAND smoke_test)
```

Implement `sim::advance` as `pos + vel * kTickDt`. Add `build/` to `.gitignore`.

```bash
scripts/tw cmake -S . -B build/plain -DCMAKE_BUILD_TYPE=RelWithDebInfo && \
  scripts/tw cmake --build build/plain -j8 && \
  scripts/tw ctest --test-dir build/plain -R smoke_test --output-on-failure && \
  git add CMakeLists.txt src/sim/sim.h src/sim/sim.cpp tests/smoke_test.cpp .gitignore && \
  git commit -m "feat: add CMake skeleton, libsim, and GoogleTest harness"
```

Expected: PASS — 1 test passes, then one commit.

**Task boundary:** `scripts/tw ctest --test-dir build/plain --output-on-failure` — full suite, green.

---

## Task 3: Sanitizer configurations, proven live

A sanitizer that is configured but not actually enabled is indistinguishable
from a clean run. Each checkpoint therefore asserts that the sanitizer
**detects a deliberate fault**, not merely that the build succeeded.

**Files:**
- Create: `tests/faults/uaf.cpp`, `tests/faults/ovf.cpp`, `tests/faults/race.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `TW_SANITIZER` from Task 2.
- Produces: CTest tests `asan_detects_uaf`, `ubsan_detects_overflow`, `tsan_detects_race`, each registered only when the matching sanitizer is active, each using `PASS_REGULAR_EXPRESSION` so a sanitizer *report* is the pass condition.

**Checkpoint 1: ASan detects a heap-use-after-free**

- [ ] **Step 1: Write the failing test, then run it**

Write `tests/faults/uaf.cpp`: allocate one `int` with `malloc`, `free` it, then
read it and return the value. In `CMakeLists.txt`, when `TW_SANITIZER` contains
`address`, build target `fault_uaf` and register
`add_test(NAME asan_detects_uaf COMMAND fault_uaf)` with
`set_tests_properties(asan_detects_uaf PROPERTIES PASS_REGULAR_EXPRESSION "heap-use-after-free")`.

Run:
```bash
scripts/tw cmake -S . -B build/asan -DCMAKE_BUILD_TYPE=RelWithDebInfo -DTW_SANITIZER=address,undefined && \
scripts/tw cmake --build build/asan -j8 && \
scripts/tw ctest --test-dir build/asan -R asan_detects_uaf --output-on-failure
```
Expected: FAIL — the CMake block does not exist yet, so `ctest -R asan_detects_uaf` matches no tests and reports `No tests were found`.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: add the guarded block described above to `CMakeLists.txt`. The fault
target links `tickwire_sim_flags` so it inherits the sanitizer flags.

```bash
scripts/tw cmake -S . -B build/asan -DCMAKE_BUILD_TYPE=RelWithDebInfo -DTW_SANITIZER=address,undefined && \
  scripts/tw cmake --build build/asan -j8 && \
  scripts/tw ctest --test-dir build/asan -R asan_detects_uaf --output-on-failure && \
  git add CMakeLists.txt tests/faults/uaf.cpp && \
  git commit -m "test: prove ASan is active by detecting a deliberate use-after-free"
```

Expected: PASS — ASan reports `heap-use-after-free`, CTest treats the match as a pass, then one commit.

**Checkpoint 2: UBSan detects signed integer overflow**

- [ ] **Step 1: Write the failing test, then run it**

Write `tests/faults/ovf.cpp`: set `int x = INT_MAX;` then `x += 1;` and return `x`.
Register `ubsan_detects_overflow` under the same `address,undefined` guard, with
`PASS_REGULAR_EXPRESSION "signed integer overflow"`.

Run: `scripts/tw ctest --test-dir build/asan -R ubsan_detects_overflow --output-on-failure`
Expected: FAIL — `No tests were found` (the test is not yet registered).

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: add the `fault_ovf` target and its test registration alongside Checkpoint 1's.

```bash
scripts/tw cmake -S . -B build/asan -DCMAKE_BUILD_TYPE=RelWithDebInfo -DTW_SANITIZER=address,undefined && \
  scripts/tw cmake --build build/asan -j8 && \
  scripts/tw ctest --test-dir build/asan -R ubsan_detects_overflow --output-on-failure && \
  git add CMakeLists.txt tests/faults/ovf.cpp && \
  git commit -m "test: prove UBSan is active by detecting a deliberate signed overflow"
```

Expected: PASS, then one commit.

**Checkpoint 3: TSan detects a data race**

- [ ] **Step 1: Write the failing test, then run it**

Write `tests/faults/race.cpp`: a global `int`, two `std::thread`s each
incrementing it unguarded, both joined. Register `tsan_detects_race` only when
`TW_SANITIZER` contains `thread`, with `PASS_REGULAR_EXPRESSION "data race"`.

Run:
```bash
scripts/tw cmake -S . -B build/tsan -DCMAKE_BUILD_TYPE=RelWithDebInfo -DTW_SANITIZER=thread && \
scripts/tw cmake --build build/tsan -j8 && \
scripts/tw setarch -R ctest --test-dir build/tsan -R tsan_detects_race --output-on-failure
```
Expected: FAIL — `No tests were found`.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: add the `fault_race` target under the `thread` guard.

```bash
scripts/tw cmake -S . -B build/tsan -DCMAKE_BUILD_TYPE=RelWithDebInfo -DTW_SANITIZER=thread && \
  scripts/tw cmake --build build/tsan -j8 && \
  scripts/tw setarch -R ctest --test-dir build/tsan -R tsan_detects_race --output-on-failure && \
  git add CMakeLists.txt tests/faults/race.cpp && \
  git commit -m "test: prove TSan is active by detecting a deliberate data race"
```

Expected: PASS — TSan reports `data race`, then one commit.

**Task boundary:** all three configurations, full suites:
```bash
scripts/tw ctest --test-dir build/plain --output-on-failure && \
scripts/tw ctest --test-dir build/asan --output-on-failure && \
scripts/tw setarch -R ctest --test-dir build/tsan --output-on-failure
```

---

## Task 4: Two-binary determinism harness

The design doc's original phrasing — two builds of `libsim` inside one test
binary — is an ODR violation. This is two **executables** plus a comparator.

**Files:**
- Create: `tools/digest_dump.cpp`, `scripts/compare-digests.sh`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `sim::advance`, `sim::kTickDt` from Task 2.
- Produces: executables `digest_dump_a` and `digest_dump_b`, built from identical sources through `libsim`, each printing one line: the `%a` (hex float) representation of the state after 10,000 iterations of `advance`, starting from `pos=1.0f, vel=2.0f`.
- Produces: `scripts/compare-digests.sh <fileA> <fileB>` — exits 0 if byte-identical, non-zero with a diff otherwise.
- Produces: CTest test `determinism_two_binaries`.

**Checkpoint 1: the harness proves agreement and can prove disagreement**

Both assertions belong to one RED→GREEN cycle: a comparator that cannot fail is
the specific defect being guarded against, so proving it *can* fail is part of
the same deliverable, not a separate behavior.

- [ ] **Step 1: Write the failing test, then run it**

Register a CTest test `determinism_two_binaries` that runs
`scripts/compare-digests.sh` over the outputs of both executables. Separately,
the comparator must be exercised negatively: `scripts/compare-digests.sh` given
two files with different content must exit non-zero.

Write the test as a shell driver invoked by CTest that asserts both:
1. `digest_dump_a` and `digest_dump_b` outputs are byte-identical (exit 0).
2. The comparator exits non-zero when handed two deliberately different files.

Run: `scripts/tw ctest --test-dir build/plain -R determinism_two_binaries --output-on-failure`
Expected: FAIL — `No tests were found` (neither the executables nor the test are registered yet).

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract:
- `tools/digest_dump.cpp` — loop `advance` 10,000 times from `pos=1.0f, vel=2.0f`, print the result with `printf("%a\n", pos)`. No allocation, no clock reads.
- `CMakeLists.txt` — two executables from the same source, both linking `libsim` (so both inherit `tickwire_sim_flags`); register the CTest test.
- `scripts/compare-digests.sh` — `cmp -s "$1" "$2"`; on mismatch print both values and exit 1.

```bash
scripts/tw cmake -S . -B build/plain -DCMAKE_BUILD_TYPE=RelWithDebInfo && \
  scripts/tw cmake --build build/plain -j8 && \
  scripts/tw ctest --test-dir build/plain -R determinism_two_binaries --output-on-failure && \
  git add CMakeLists.txt tools/digest_dump.cpp scripts/compare-digests.sh && \
  git commit -m "test: add two-binary determinism harness with a falsifiable comparator"
```

Expected: PASS — digests match and the comparator is shown to reject a mismatch, then one commit.

**Task boundary:** `scripts/tw ctest --test-dir build/plain --output-on-failure` — full suite, green.

---

## Task 5: CI as a thin caller of a local script

CI must run the same commands that run locally. Making the workflow a caller of
`scripts/ci.sh` means CI correctness is verifiable on this machine.

**Files:**
- Create: `scripts/ci.sh`, `.github/workflows/ci.yml`

**Interfaces:**
- Produces: `scripts/ci.sh` — configures, builds, and tests all three configurations, exits non-zero if any fails.

**Checkpoint 1: one command runs every configuration green**

- [ ] **Step 1: Write the failing test, then run it**

Run: `scripts/tw bash scripts/ci.sh`
Expected: FAIL — `bash: scripts/ci.sh: No such file or directory`.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract — `scripts/ci.sh` runs, with `set -euo pipefail` and **`set -o pipefail` semantics preserved** (a piped `tail` masking a failing exit code is how a broken build was mistaken for a passing one during planning):
1. `plain` — configure, build, `ctest`
2. `asan` — configure with `TW_SANITIZER=address,undefined`, build, `ctest`
3. `tsan` — configure with `TW_SANITIZER=thread`, build, `setarch -R ctest`
4. `bash scripts/verify-toolchain.sh`

`.github/workflows/ci.yml` — a job that checks out the repo and runs
`scripts/tw bash scripts/ci.sh`. Pin `runs-on: ubuntu-22.04` (not `ubuntu-latest`);
the runner's own compiler is never used, but pinning keeps the Docker and
checkout environment stable.

```bash
scripts/tw bash scripts/ci.sh && \
  git add scripts/ci.sh .github/workflows/ci.yml && \
  git commit -m "ci: run every build configuration through one script"
```

Expected: PASS — all three configurations and the toolchain check green, then one commit.

**Task boundary:** `scripts/tw bash scripts/ci.sh` — the whole phase, green.

---

## Task 6: Project conventions

The dev-workflow guide § 3 defers `CLAUDE.md` until after P0 exists, so that it
documents verified reality rather than guesses. P0 now exists.

**Files:**
- Create: `CLAUDE.md`

**Checkpoint 1: `CLAUDE.md` records the constraints a cold session would otherwise violate**

- [ ] **Step 1: Write the failing test, then run it**

Run: `test -f CLAUDE.md && grep -q "scripts/tw" CLAUDE.md`
Expected: FAIL — exit 1, the file does not exist.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: `CLAUDE.md` states, at minimum — that all build/test commands go
through `scripts/tw` and the host toolchain must never be used; the float flag
rules including the `-march=native` and `x86-64-v2` prohibitions; the TSan
`setarch -R` requirement; the CMake 3.21 floor and why; the forbidden interface
shapes; and pointers to both spec documents. Derive it from this plan's Global
Constraints — do not restate anything not verified here.

```bash
test -f CLAUDE.md && grep -q "scripts/tw" CLAUDE.md && \
  scripts/tw bash scripts/ci.sh && \
  git add CLAUDE.md && \
  git commit -m "docs: add CLAUDE.md recording P0's verified constraints"
```

Expected: PASS, then one commit.

**Task boundary:** `scripts/tw bash scripts/ci.sh` — green. **The branch is now green and verified.**

Hand off to `finishing-a-development-branch` for the integration decision. Do not
merge or push from within this plan.

---

## Self-Review

**Spec coverage.** The design doc's P0 row asks for: task-zero toolchain (Task 1),
CMake (Task 2), GoogleTest (Task 2), CI (Task 5), three sanitizer configs
(Task 3). The resolution doc additionally requires the two-binary determinism
test in P0 (Task 4) and the `tickwire_sim_flags` PUBLIC-propagation model
(Task 2). `CLAUDE.md` timing comes from the workflow guide § 3 (Task 6).
No P0 requirement is unassigned.

**Deferred deliberately, and where to:** the full `libsim` surface, `Transport`,
`SpscRing`, and clock-sync header fields all belong to P1/P2 and are specified in
the resolution doc, not here. The raylib client — and the open question of
whether the container can reach WSLg's display — is a **P2** concern; P0 builds
no GUI, so the risk is not yet load-bearing.

**Type consistency.** `sim::advance(float, float)` and `sim::kTickDt` are
declared in Task 2's Interfaces and used unchanged in Task 4. `TW_SANITIZER` is
introduced in Task 2 and consumed in Tasks 3 and 5. `scripts/tw` is introduced in
Task 1 and used by every subsequent command.

**Checkpoint falsifiability.** Every checkpoint names a signal at the surface its
test calls: T1C1 exits non-zero on GCC major 9; T1C2 fails with
`Operation not permitted` until the caps are granted; T2C1 fails CMake configure
with no `CMakeLists.txt`; T3C1–C3 report `No tests were found` until each test is
registered; T4C1 likewise; T5C1 fails with `No such file or directory`; T6C1
fails `test -f`. None passes at the moment its test is written.

**One risk worth stating.** Tasks 3 and 4 both use "`ctest -R` matches nothing"
as their RED. That is a genuine failure, but a weak signal — it would also appear
if the regex were simply misspelled. The executor should confirm the *reason* is
an unregistered test, not a typo, before proceeding to Step 2.
