# P6 Demo Follow-up Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use the `executing-plans` skill to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make P6's lag-compensation claim ("toggle lag compensation and shots
that looked like hits start registering") actually visible to a human in the
demo. That means fixing the bug that stops the README recipe from connecting
at all, fixing the demo's readability problems, and then re-running the human
check and recording it honestly.

**Architecture:** No simulation, protocol, or server-logic change. This covers
four surface fixes plus one committed measurement:
- a byte-order bug in `tw_server`'s bind call;
- a tunable sweep period on `tw_loadclient`'s bot;
- a zoomable camera in `client::view` (the pure world↔screen mapping), which
  `tw_client` uses to follow the local player at 4×;
- a HUD that fits the window;
- a `tools/lagcomp_probe` measurement that makes the numbers behind these
  choices regenerable.

**Tech Stack:** C++20 (GCC 10.5 pinned), CMake 3.28.4, GoogleTest/CTest, raylib
(GUI build only). All inside the pinned `tickwire-dev:gcc10-cmake3.28.4-x11` image.

**Spec:**
- [`docs/specs/2026-09-04-tickwire-design.md`](../specs/2026-09-04-tickwire-design.md)
  — § "The demo" (*"Toggle lag compensation and shots that looked like hits start
  registering"*)
- [`docs/plans/2026-09-12-phase-6-lag-compensation.md`](2026-09-12-phase-6-lag-compensation.md)
  — Task 8 (demo surface) and Task 10's "Outstanding for a human" item, which
  this plan closes
- [`docs/project-history.md`](../project-history.md) — P6 section; the P4
  "Interactive feel — human-verified" entry is the format the final task follows

**Branch:** cut `p6-demo-followup` from `dev`.

---

## Why this plan exists (findings from the 2026-09-13/14 human check)

Discovered while a human ran P6's outstanding visual check. None of it is
recorded anywhere yet. Task 5 records it.

1. **`tw_server --port N` binds the byte-swapped port.** `apps/tw_server.cpp:139`
   passes the host-order `port` straight to `UdpTransport::bind(uint32_t addr_be,
   uint16_t port_be)`. `--port 41234` (0xA112) binds 4769 (0x12A1), and
   `tw_server` itself prints `port=4769`. Every client sends to
   `htons(41234)` and never joins. It has been present since `3b166fd`
   (2026-09-08, P2). It went unnoticed because every script and test uses
   `--port 0`, and 0 byte-swapped is still 0. The P6 README recipe was the first
   fixed-port invocation, and no human had run it.
2. **The HUD overflows the 800 px window.** `lagcomp=` and `hits=` sit past the
   right edge. The human had to maximize the window to read them.
3. **With the port worked around (`--port 0`), on vs off was "very minimal, not
   very clear" by eye at 200 ms**, while `lagcomp_hitrate_test` measures 100%
   vs 11%. The toggle path itself was verified correct: `Client` stamps
   `view_tick = 0` when off (`client.h:223`), and `Server` rewinds only when
   `view_tick != 0` (`server.h:127`). The server printed `lagcomp rewound_shots=328
   rewinds_rejected=0`. A throwaway lockstep harness over the real `Client`/`Server`
   (the prototype of Task 4's tool) showed why:

   | topology | bot reverses every | axis | aim σ (world units) | on | off |
   |---|---|---|---|---|---|
   | test (100 ms ±10 each side) | 120 ticks | y | 0 | 1.000 | 0.100 |
   | demo (shooter 200 ms only) | 30 | x | 0 | 1.000 | 0.400 |
   | demo | 30 | x | 0.5 | 0.687 | 0.193 |
   | demo | 30 | x | 1.0 | 0.307 | 0.233 |
   | demo | 120 | x | 1.0 | 0.293 | 0.260 |
   | demo | 120 | x | 0.25 | 0.980 | 0.060 |

   At the demo's 8 px per world unit, a player is 8 px across, and a hit must
   pass within 4 px of its center. Human tracking error of about one dot-width
   (σ ≈ 1 unit) erases the gap on its own: most shots miss on aim in both modes,
   and random misaims land on the live target often enough to prop up "off". The
   fast 30-tick reversal also inflates "off" (40% even with perfect aim). **Zoom
   is the fix that matters.** At 4×, the same ~8 px hand error is σ ≈ 0.25
   units. Combined with a 120-tick sweep, that gives 98% vs 6%.

## Global Constraints

Every task's requirements implicitly include this section. Values are copied
verbatim from `CLAUDE.md`.

**Build and test**

- **Never invoke the host `g++`, `cmake`, or `ctest`.** Everything goes through
  `scripts/tw <command...>`.
- **`ctest` does not build. Always `cmake --build` first.** Every test command is
  `cmake --build <dir> -j8 && ctest --test-dir <dir> -R <regex> --output-on-failure`.
  A regex that matches nothing **exits 0**. At every RED step, check that the
  output shows the expected failure and not `No tests were found`.
- **A new `add_test` must be registered in the same Step 1 that expects it to
  fail.** Re-run `cmake -S . -B build/plain` in that command so the registration
  takes.
- **Scope every checkpoint with `-R <regex>`.** Run the full suite
  (`scripts/tw bash scripts/ci.sh`) only at a task boundary.
- `PASS_REGULAR_EXPRESSION` **replaces** the exit code as CTest's pass signal
  (see the `e2e_udp` comment in `CMakeLists.txt`). Use it only on tests whose
  pass condition really is an output line.
- Configurations: `build/plain`, `build/asan`, `build/tsan` (all via `ci.sh`), and
  `build/gui` (`-DTW_BUILD_GUI=ON`, for `tw_client` only; `ci.sh` skips it).

**Language**

- No `std::format`, no `std::bit_cast`. `-Wall -Wextra -Werror`.
- Types `PascalCase`, constants `kPascalCase`, members `snake_case_`.
- `_be` suffixes are reserved for values the kernel needs in network order.
  Everything a human types is host order, converted exactly once at the call
  that hands it to the kernel-facing API (`htons`).

**Architecture invariants**

- **`apps/` are thin**: argument parsing, a clock, a loop, drawing. Any
  coordinate math the demo needs lives in `src/client/view.{h,cpp}` and is
  unit-tested there. The inline mouse→world math currently in
  `tw_client.cpp:119-121` moves there too.
- **Zoom is view-only.** It changes pixels per world unit and nothing else.
  `sim::kPlayerRadius`, hit resolution, and aim vectors (which are world-space,
  from `aimFromCursor`) are untouched. Drawn radius stays exactly the hit radius
  in screen units, so what looks like a hit still is one.
- **`tw_loadclient`'s default behavior must not change.** `--sweep-ticks`
  defaults to `30`, today's hard-coded value. `scripts/bench.sh` and
  `docs/benchmarks.md`'s recorded numbers depend on the default pattern.
- No wire-format change. Protocol stays at version 3.

**Commits**

- `type: description`. One commit per checkpoint, **chained behind its test
  with `&&`**. `git add` names exact paths; never `-A` or `.`.

**Measurement honesty**

- Task 4's numbers and Task 5's human result are reported as measured. If the
  human check still can't distinguish on from off after Tasks 1–3, record that.
  Do not retune until it looks right.

---

## File Structure

| File | Change | Responsibility |
|---|---|---|
| `apps/tw_server.cpp` | modify | `htons(port)` at the bind call |
| `apps/tw_loadclient.cpp` | modify | `--sweep-ticks <n>` flag and its validation |
| `scripts/e2e-udp.sh` | modify | exercise `--sweep-ticks` end-to-end |
| `src/client/view.{h,cpp}` | modify | `Camera`; zoom-aware `worldToScreen`/`worldToScreenRadius`; new `screenToWorld` |
| `tests/client/view_test.cpp` | modify | camera/zoom/inverse cases (the file is 64 lines, safe to grow) |
| `apps/tw_client.cpp` | modify | follow-cam at 4×, world-space arena border, 3-line HUD |
| `tools/lagcomp_probe.cpp` | create | regenerable on/off hit-rate matrix over demo parameters |
| `CMakeLists.txt` | modify | new `add_test`s, `lagcomp_probe` target |
| `README.md`, `docs/project-history.md`, `CLAUDE.md`, `journal/` | modify/create | Task 5 |

---

## Task 1: `tw_server` binds the port it was given

**Files:**
- Modify: `apps/tw_server.cpp:139`
- Modify: `CMakeLists.txt` (next to the other `server_app_*` tests, ~line 114)

**Checkpoint 1: a fixed `--port` binds and reports that exact port**

- [ ] **Step 1: Write the failing test, then run it**

Spec — register in `CMakeLists.txt`:
`add_test(NAME server_app_fixed_port COMMAND tw_server --port 41237 --ticks 5)`
with `set_tests_properties(server_app_fixed_port PROPERTIES PASS_REGULAR_EXPRESSION "port=41237\n")`.
The printed value comes from `getsockname` (`localEndpoint().port_be`, via
`ntohs`), so matching it proves the socket really bound 41237. The trailing
`\n` stops `port=412370` from matching.

Run: `scripts/tw bash -c "cmake -S . -B build/plain && cmake --build build/plain -j8 && ctest --test-dir build/plain -R server_app_fixed_port --output-on-failure"`
Expected: FAIL with `Required regular expression not found`. The output shows
`port=5537` (41237 = 0xA115, swapped to 0x15A1).

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: `transport->bind(htonl(INADDR_LOOPBACK), htons(port))`. Change nothing
else in the file.

```bash
scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain -R 'server_app|e2e_udp' --output-on-failure" && \
  git add apps/tw_server.cpp CMakeLists.txt && \
  git commit -m "fix: bind tw_server to the port it was given, not its byte swap"
```

Expected: every `server_app_*` test and `e2e_udp` pass (the `--port 0` paths are
unaffected), then one commit.

- [ ] **Task boundary:** `scripts/tw bash scripts/ci.sh`.

---

## Task 2: `tw_loadclient --sweep-ticks`

**Files:**
- Modify: `apps/tw_loadclient.cpp`
- Modify: `scripts/e2e-udp.sh`
- Modify: `CMakeLists.txt` (after the `e2e_udp` test)

**Interfaces:**
- Produces: CLI flag `--sweep-ticks <n>` (default `30`, must be `>= 1`). Task 5's
  README recipe uses `--sweep-ticks 120`.

**Checkpoint 1: the bot's reversal period is configurable and validated**

- [ ] **Step 1: Write the failing tests, then run them**

Spec:
- Register `add_test(NAME loadclient_sweep_ticks_zero_rejected COMMAND tw_loadclient --host 127.0.0.1 --port 1 --sweep-ticks 0)`
  and `add_test(NAME loadclient_sweep_ticks_negative_rejected COMMAND tw_loadclient --host 127.0.0.1 --port 1 --sweep-ticks -5)`,
  both with `PASS_REGULAR_EXPRESSION "--sweep-ticks must be >= 1"`. Today both
  print only the generic usage line, which doesn't contain that text.
- In `scripts/e2e-udp.sh`, add `--sweep-ticks 120` to the existing `tw_loadclient`
  invocation. Today that's an unknown argument: usage is printed, the exit is
  1, and `set -euo pipefail` fails the script.

Run: `scripts/tw bash -c "cmake -S . -B build/plain && cmake --build build/plain -j8 && ctest --test-dir build/plain -R 'loadclient_sweep_ticks|e2e_udp' --output-on-failure"`
Expected: FAIL, all three. The two `_rejected` tests fail with `Required regular
expression not found`. `e2e_udp` fails because it exits non-zero after printing
`usage: tw_loadclient ...`.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract, in `apps/tw_loadclient.cpp`:
- `--sweep-ticks <n>` is parsed with `std::atoi` into an **`int`**. If the
  value is `< 1`, print `tw_loadclient: --sweep-ticks must be >= 1\n` to
  stderr and `return 1`, **before** any socket is bound. Checking the `int`
  before converting it to `uint32_t` is what catches negatives. Otherwise
  store it as `uint32_t sweep_ticks` (default `30`).
- The movement line becomes `((t / sweep_ticks + i) % 2 == 0) ? 1.0f : -1.0f`.
  Nothing else in the loop changes.
- The `printUsage` string gains `[--sweep-ticks <n>]`.

```bash
scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain -R 'loadclient_sweep_ticks|e2e_udp|bench_smoke' --output-on-failure" && \
  git add apps/tw_loadclient.cpp scripts/e2e-udp.sh CMakeLists.txt && \
  git commit -m "feat: make tw_loadclient's sweep period configurable"
```

Expected: all pass. `bench_smoke` confirms the default path still works. Then
one commit.

- [ ] **Task boundary:** `scripts/tw bash scripts/ci.sh`.

---

## Task 3: A follow-cam that makes an 8 px target 32 px

**Files:**
- Modify: `src/client/view.h`, `src/client/view.cpp`
- Modify: `tests/client/view_test.cpp`
- Modify: `apps/tw_client.cpp`

**Interfaces:**
- Produces (in `namespace client`, `src/client/view.h`). These **replace** the
  current `worldToScreen(wx, wy, side, ox, oy)` and `worldToScreenRadius(r,
  side)`. `tw_client` always passes `ox = oy = 0` and is their only non-test
  caller, so no overload is kept.
  ```cpp
  // A square viewport of `side` pixels looking at world point
  // (center_x, center_y). zoom == 1 fits the whole arena; zoom > 1 magnifies.
  // Precondition: zoom > 0 and finite (callers pass a constant).
  struct Camera {
    float center_x;
    float center_y;
    float zoom;
  };
  ScreenPos worldToScreen(float wx, float wy, float side, const Camera& cam) noexcept;
  float worldToScreenRadius(float r, float side, float zoom) noexcept;
  void screenToWorld(float sx, float sy, float side, const Camera& cam,
                     float& wx, float& wy) noexcept;
  ```
  With `scale = side / (2 * kArenaHalf) * zoom`:
  - `screen.x = side/2 + (wx - center_x) * scale`
  - `screen.y = side/2 - (wy - center_y) * scale`
  - `screenToWorld` is the exact inverse: `wx = center_x + (sx - side/2) / scale`,
    `wy = center_y - (sy - side/2) / scale`
  - radius: `r * scale`

**Checkpoint 1: zoom-aware world↔screen mapping**

- [ ] **Step 1: Write the failing test, then run it**

Spec. Replace `ViewTest.WorldToScreenMapsCornersAndCentreExactly`'s body and add
one test. Every value below is exact in binary floating point: at side 800,
zoom 1 is 8 px/unit and zoom 4 is 32 px/unit. Assert with `EXPECT_EQ`, not
`EXPECT_FLOAT_EQ`.
- `ViewTest.WorldToScreenMapsCornersAndCentreExactly` with `Camera{0, 0, 1}`,
  side 800:
  - `(0,0) → (400,400)`
  - `(-kArenaHalf, kArenaHalf) → (0,0)`
  - `(kArenaHalf, -kArenaHalf) → (800,800)`
  - `worldToScreenRadius(kPlayerRadius, 800, 1) == 4`
  - The old `ox/oy` offset assertion is deleted; that parameter no longer exists.
- `ViewTest.ZoomedCameraCentresOnItsTargetAndInvertsExactly` with
  `Camera{-30, -30, 4}`, side 800:
  - `(-30,-30) → (400,400)`
  - `(-25,-30) → (560,400)`
  - `(-30,-25) → (400,240)`
  - `worldToScreenRadius(kPlayerRadius, 800, 4) == 16`
  - `screenToWorld(560, 240, 800, cam) → (-25,-25)`
  - `screenToWorld(0, 0, 800, Camera{0,0,1}) → (-kArenaHalf, kArenaHalf)`

Run: `scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain -R view_test --output-on-failure"`
Expected: FAIL, as a compile error (`Camera` is not a member of `client`, and
the calls match no overload).

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: the Interfaces block above, in `view.h`/`view.cpp`, with a doc comment
on `Camera` and `screenToWorld` in the style of the existing ones. `tw_client`
isn't in `build/plain`, so it isn't updated in this checkpoint. Checkpoint 2
does that in the same task.

```bash
scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain -R 'view_test|lagcomp_hitrate_test' --output-on-failure" && \
  git add src/client/view.h src/client/view.cpp tests/client/view_test.cpp && \
  git commit -m "feat: add a zoomable camera to the client view mapping"
```

Expected: both pass. `lagcomp_hitrate_test` confirms `aimFromCursor` is
untouched. Then one commit.

**Checkpoint 2: `tw_client` follows the local player at 4× with a readable HUD**

GUI drawing has no test surface. P6 Task 8 Checkpoint 2's precedent applies:
build it, run the display self-test, and leave the visual check for Task 5's
human step. No RED step.

- [ ] **Step 1: Implement**

Contract, in `apps/tw_client.cpp`:
- Add `constexpr float kZoom = 4.0f;` next to `kSide`.
- Each frame, after `localPosition`, build `client::Camera cam{local_x, local_y,
  kZoom}` when `have_local`, else `{0.0f, 0.0f, kZoom}`.
- Replace lines 119–121's inline mouse math with
  `client::screenToWorld(mouse.x, mouse.y, kSide, cam, world_mx, world_my)`.
- Every `worldToScreen(..., 0.0f, 0.0f)` becomes `worldToScreen(..., cam)`.
  Every `worldToScreenRadius(r, kSide)` becomes `worldToScreenRadius(r, kSide,
  kZoom)`. The hit ring keeps its `* 1.6f`.
- The arena border is drawn in world space: `DrawRectangleLines` from
  `worldToScreen(-kArenaHalf, kArenaHalf, kSide, cam)`, with width/height
  `2 * kArenaHalf * scale`, where `scale` is
  `worldToScreenRadius(1.0f, kSide, kZoom)`. It scrolls with the camera, so the
  arena edge is visible when the player nears it. Raylib clips off-screen lines.
- The HUD becomes three `DrawText` lines at font size 20, x = 10, y = 10/34/58,
  drawn last (on top of the world):
  1. `tick=%u latency=%ums rtt=%ums lead=%d players=%u`
  2. `pred=%s err_p99=%.2f interp=%s render=%u`
  3. `lagcomp=%s hits=%u`

  Same values as today's single line, regrouped. Each line is ≤ ~50 characters,
  well under the ~75 that fit at 800 px (measured from the 2026-09-14
  screenshot).
- `runSelftest` is unchanged.

- [ ] **Step 2: Verify and commit**

```bash
scripts/tw bash -c "cmake -S . -B build/gui -DTW_BUILD_GUI=ON && cmake --build build/gui -j8 && ctest --test-dir build/gui -R client_selftest --output-on-failure" && \
  git add apps/tw_client.cpp && \
  git commit -m "feat: follow the local player at 4x zoom with a three-line HUD in tw_client"
```

Expected: builds under `-Werror`, and `client_selftest` passes (0 with a
display, 77 = skip without). Then one commit.

- [ ] **Task boundary:** `scripts/tw bash scripts/ci.sh`.

---

## Task 4: `tools/lagcomp_probe` — the regenerable numbers behind Tasks 2–3

Findings §3's table came from a gitignored throwaway harness. The project rule
is that no number is claimed until it's been measured, and measurements are
regenerable. This task commits the harness as a tool, in the same spirit as
`bench_queue`.

**Files:**
- Create: `tools/lagcomp_probe.cpp`
- Modify: `CMakeLists.txt` (after `bench_queue`)

**Checkpoint 1: the probe builds and prints its matrix**

- [ ] **Step 1: Register the target and smoke test, then run it**

Spec — in `CMakeLists.txt`:
`add_executable(lagcomp_probe tools/lagcomp_probe.cpp)`,
`target_link_libraries(lagcomp_probe PRIVATE libserver libclient)`,
`add_test(NAME lagcomp_probe_smoke COMMAND lagcomp_probe)` with
`PASS_REGULAR_EXPRESSION "topology=test reverse_ticks=120 axis=y aim_sigma=0.00 on="`.

Run: `scripts/tw bash -c "cmake -S . -B build/plain && cmake --build build/plain -j8 && ctest --test-dir build/plain -R lagcomp_probe_smoke --output-on-failure"`
Expected: FAIL. CMake configure fails with `Cannot find source file:
tools/lagcomp_probe.cpp`.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract, `tools/lagcomp_probe.cpp`:
- **Structure:** it's `lagcomp_hitrate_test.cpp`'s `runArm` harness, generalized
  (same lockstep order: server ingest/tick/advance, shooter tick/advance, target
  tick/advance; `now_ms = i * 16`; 700 settle ticks with the shooter moving +y
  for its first 60 joined ticks; then 1800 shooting ticks). Everything is
  heap-allocated with `std::make_unique`, like the test. It's a single `main`,
  with no gtest and no arguments (any argument → print usage, exit 1).
- **Parameters per row:**
  - `topology`:
    - `test`: all three transports `latency_ms = 100, jitter_ms = 10, seed = 7`.
    - `demo`: shooter `latency_ms = 200, jitter_ms = 0, seed = 1`, matching
      `tw_client`'s `SimConfig`; server and target `latency_ms = 0, seed = 7`,
      matching `tw_server`/`tw_loadclient` (no latency wrapper).
  - `reverse_ticks`: target move sign is `(i / reverse_ticks) % 2 == 0 ? +1 : -1`.
  - `axis`: `x` or `y`, which component carries that sign.
  - `aim_sigma`: world units. When > 0, add independent `std::normal_distribution<float>(0, aim_sigma)`
    samples, from a `std::mt19937` seeded `42` per arm, to the drawn target's x
    and y before `aimFromCursor`.
- **Rows, in this order** (each run on then off):
  1. `test 120 y 0`
  2. `demo 120 y 0`
  3. `test 30 y 0`
  4. `demo 30 x 0`
  5. `demo 30 x 0.25`
  6. `demo 30 x 0.5`
  7. `demo 30 x 1.0`
  8. `demo 120 x 0`
  9. `demo 120 x 0.25`
  10. `demo 120 x 0.5`
  11. `demo 120 x 1.0`
- **Output:** one line per row, exactly
  `topology=<t> reverse_ticks=<n> axis=<x|y> aim_sigma=<%.2f> on=<%.3f> on_hits=<n>/<shots> off=<%.3f> off_hits=<n>/<shots> gap=<%+.3f>`
  (`std::printf`, `%llu` for counts). A leading comment explains that
  `aim_sigma` models human tracking error: pixels of hand error ÷ (8 × zoom) =
  world units.
- **Exit code:** returns 1 if any arm's shooter or target failed to join, else 0.
- Keep the file ≤ 200 lines.

```bash
scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain -R lagcomp_probe_smoke --output-on-failure && build/plain/lagcomp_probe" && \
  git add tools/lagcomp_probe.cpp CMakeLists.txt && \
  git commit -m "feat: add lagcomp_probe, the demo-parameter hit-rate matrix"
```

Expected: the smoke test passes, and the full matrix prints (about 10 s). Rows 1,
4, 7, and 9 should be close to findings §3's table. Lockstep over loopback
reproduced them exactly during planning. If they differ, **stop and record
the difference**, don't adjust the tool. **Save the printed matrix verbatim.**
Task 5 records it. Then one commit.

- [ ] **Task boundary:** `scripts/tw bash scripts/ci.sh`.

---

## Task 5: Docs, security review, human verification

**Files:**
- Modify: `README.md`, `docs/project-history.md`, `CLAUDE.md`
- Create: `journal/<YYYY-MM-DD>_<HHMM>_ansh_p6-demo-followup.md`

**Checkpoint 1: README recipe works as written**

- [ ] **Step 1: Write it**

Contract, `README.md` § "Run the demo":
- Main demo paragraph: add one sentence. The camera follows your own player at
  4× zoom, so other players (spawned 10 units apart) are in view, and the arena
  border scrolls into view near the edges.
- Lag compensation recipe: keep `--port 41234`, which works after Task 1. Add
  `--sweep-ticks 120` to the `tw_loadclient` line. Replace the paragraph under
  it with precise instructions:
  - green dot = you, red dot = the bot;
  - press `W` briefly to get above the bot's row, then get roughly over the
    middle of its sweep;
  - track the red dot with the cursor while holding left click, about 15 s per
    mode;
  - with `lagcomp=on`, the white ring flashes on most shots where the tracer
    was over the dot;
  - press `L` and the same effort mostly stops registering;
  - press `L` again and hits come back;
  - on exit, the server's final `lagcomp rewound_shots=… rewinds_rejected=…`
    line (the recipe already stops the server with `kill -INT` then `wait`, so
    the line prints).

  Use this exact recipe command:
  ```bash
  scripts/tw bash -c "
    build/gui/tw_server --port 41234 & SRV=\$!
    sleep 1
    build/gui/tw_loadclient --host 127.0.0.1 --port 41234 --players 1 --ticks 36000 --sweep-ticks 120 &
    build/gui/tw_client --host 127.0.0.1 --port 41234 --latency-ms 200
    kill -INT \$SRV; wait \$SRV
  "
  ```
- Add one sentence noting the demo is tuned for human eyes and pointing to
  `tools/lagcomp_probe` for why.

- [ ] **Step 2: Verify the recipe's non-visual half, then commit**

The GUI can't be watched, but the connectivity half can be checked: the same
recipe with `tw_loadclient` alone must join.
```bash
scripts/tw bash -c "cmake --build build/gui -j8 && (build/gui/tw_server --port 41234 --ticks 400 & sleep 1; build/gui/tw_loadclient --host 127.0.0.1 --port 41234 --players 1 --ticks 200 --sweep-ticks 120 | grep -q '^joined=1$'; r=\$?; wait; exit \$r)" && \
  git add README.md && \
  git commit -m "docs: fix the lag compensation demo recipe"
```
Expected: exits 0 (`joined=1`), then one commit.

**Checkpoint 2: security review**

`CLAUDE.md` makes this mandatory for any change touching the network surface.
Task 1 changes which port the server listens on, and Task 2 adds attacker-free
but user-supplied CLI parsing. The review is small; the rule doesn't have a
size exemption.

- [ ] Run the `security-reviewer` agent over `git diff dev...HEAD -- apps/ scripts/ tools/ src/client/view.*`.
  Questions to answer explicitly:
  - Does the `htons` fix change which interface is bound? It must still be
    `INADDR_LOOPBACK` only.
  - Can `--sweep-ticks` reach a division by zero or negative wrap by any input?
  - Can `screenToWorld` produce a non-finite aim from finite mouse input? It
    only divides by `scale`, which is a positive constant.
  - Known and **out of scope**: every app parses `--port` as
    `static_cast<uint16_t>(std::atoi(...))`, so `--port 70000` silently wraps
    to 4464. Record it as a LOW finding unless the reviewer rates it higher.
    If rated higher, stop and ask.
- [ ] Fix any CRITICAL/HIGH with its own RED→GREEN commit before continuing.

**Checkpoint 3: human verification — STOP and hand to the user**

- [ ] Build `build/gui`, then ask the user to run the README recipe from
  Checkpoint 1 exactly and report back:
  - (a) `players=2` on HUD line 1;
  - (b) `hits=` before and after ~15 s of tracking in each of on → off → on;
  - (c) whether on vs off was clearly distinguishable by eye;
  - (d) the server's final `lagcomp` line;
  - (e) anything odd (HUD clipped, camera jitter, ring in the wrong place).

  **Do not proceed until the user has answered.** Record the answer as given,
  including "still not clear" if that is the answer.

**Checkpoint 4: history, conventions, journal**

- [ ] **Step 1: Write them**

Contract:
- `docs/project-history.md`, in the P6 section:
  - Replace the placeholder comment
    `<!-- Next entries: Task 10 writeup, human-verified demo result. -->`
    (and the `P5 writeup` comment line under it) with new content.
  - Add a `**Finding — ...**` entry per finding:
    - (1) the bind byte-swap: cause, `3b166fd` origin, why `--port 0` hid it
      for four phases, the fix commit;
    - (2) HUD overflow;
    - (3) the first human check's inconclusive result, the verified-correct
      toggle path, and Task 4's matrix **verbatim** with its provenance
      (`tools/lagcomp_probe`, pinned image), and the reasoning: hit radius vs
      human aim error, zoom as the fix;
    - (4) anything Tasks 1–4 turned up that this plan didn't anticipate.
  - Add `### Interactive feel — human-verified after phase completion` in the
    P4 entry's format, with Checkpoint 3's answer: latency, keys pressed,
    counts, the `lagcomp` line, and what was and wasn't distinguishable.
  - Add the security review's findings under their own `###` heading.
- `CLAUDE.md`:
  - Add `lagcomp_probe` to the `tools/` file-structure row's examples.
  - Add `client::Camera` / `screenToWorld` to the `src/client/` row.
- `journal/`: a session entry via the `journal` skill.

- [ ] **Step 2: Final full verification, then commit**

```bash
scripts/tw bash scripts/ci.sh && \
  scripts/tw bash -c "cmake -S . -B build/gui -DTW_BUILD_GUI=ON && cmake --build build/gui -j8 && ctest --test-dir build/gui -R client_selftest --output-on-failure" && \
  git add docs/project-history.md CLAUDE.md journal/ && \
  git commit -m "docs: record the P6 demo follow-up and its human verification"
```

Expected: `ci.sh` green across plain/ASan/TSan plus the toolchain assertions, and
`client_selftest` passes. Then one commit.

**The branch is now green and verified. Stop here.** `executing-plans` hands off
to `finishing-a-development-branch`, which owns the merge decision. Do not merge,
push, or open a PR from within this plan.

---

## Self-Review

- **Coverage of the request:**
  - port bug → Task 1;
  - `--sweep-ticks` → Task 2;
  - zoom → Task 3 Checkpoints 1–2;
  - HUD → Task 3 Checkpoint 2;
  - README recipe → Task 5 Checkpoint 1;
  - human re-check and history → Task 5 Checkpoints 3–4;
  - regenerable numbers (implied by the project's measurement rule) → Task 4;
  - mandatory security review → Task 5 Checkpoint 2.
- **Falsifiability:**
  - Task 1 RED: the output shows `port=5537`, so the regex misses.
  - Task 2 RED: the flag is unknown today, so the usage text lacks
    `must be >= 1`, and e2e exits 1.
  - Task 3 Checkpoint 1 RED: compile error.
  - Task 4 RED: configure error on the missing source.
  - GUI wiring and docs have no RED, and are marked that way per the P6 Task 8
    precedent.
- **Type consistency:** `client::Camera{center_x, center_y, zoom}`,
  `worldToScreen(wx, wy, side, const Camera&)`,
  `worldToScreenRadius(r, side, zoom)`, and
  `screenToWorld(sx, sy, side, const Camera&, float&, float&)` are used
  identically in Task 3 Checkpoints 1–2. `--sweep-ticks` is the same in Tasks 2,
  4 (as `reverse_ticks`, documented as the same quantity), and 5.
- **Known out of scope:** the `--port` `atoi` → `uint16_t` wrap (recorded via
  Task 5's review, not fixed); `tests/server/server_test.cpp` and
  `tests/client/client_test.cpp` being over 800 lines (untouched here).
