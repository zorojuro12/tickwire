# 2026-09-04 — ansh — P0 planning, and verifying the toolchain actually exists

**Status:** P0 is planned, committed, and **not executed**. No source code in the repo yet — `docs/` only. The full P0 loop (pinned image, FetchContent GoogleTest, three sanitizer configs) was verified end-to-end in a throwaway project outside the repo, so the plan rests on a proven loop rather than an assumed one.
**Decided:** A pinned Docker image (`gcc:10` + CMake 3.28.4) replaces `sudo apt install g++-10` as P0 task zero — it removes the sudo blocker, and pins the compiler for CI with the same artifact. See `docs/specs/2026-09-04-architecture-resolution.md` § Q5.
**Spec:** Updated — design doc corrected against a machine check; a new architecture-resolution doc answers the five questions gating P0, and was itself corrected twice after real builds falsified it.
**Next:** Execute `docs/plans/2026-09-04-phase-0-toolchain-skeleton.md` Task 1 via `executing-plans` — inline, or handed to a fresh Sonnet window per the two-model loop.
**Blocked on:** Nothing blocks P0 execution. **Push is blocked — no git remote is configured.**
**Touches:** `docs/specs/*`, `docs/plans/*`, `docs/dev-workflow-guide.md`, `journal/`

---

## What We Worked On

Located the project idea, read the workflow guide, then ran the `/impl-plan`
architectural layer and the `writing-plans` execution layer for P0. The through-line
was that almost every confident claim — mine and the docs' — broke when checked
against the actual machine. Three separate rounds of that.

## Decisions Made

- **Container over host install** — see resolution doc § Q5. Docker runs without sudo here; `apt` does not.
- **Floats stay, but the revisit trigger changed** — from "if P3 feels unstable" to "the determinism test goes red." P3's failures are diffuse by the design doc's own account, so the old trigger wasn't actionable.
- **`kMaxPlayers = 32` is derived, not chosen** — 24 B/player against the ~1200 B MTU makes 48 the hard ceiling. This gives P4's snapshot delta a numeric justification instead of "the reference articles mention it."
- **Deleted `docs/project-idea.md`** — a copy I made early in the session, left 160 lines behind after the corrections. Three copies existed while the spec header accounted for two.

## What Worked

- **Pinned image**: GCC 10.5.0 with `jthread`, `span`, `concepts` all present (`std::format` absent as expected — fmtlib instead).
- **FetchContent GoogleTest** `release-1.12.1` fetched, built, and ran green inside the container.
- **All three configurations green end-to-end**: `plain`, `asan` (`address,undefined`), `tsan`.
- **Each sanitizer proven *live* by a deliberate fault** — ASan caught a heap-use-after-free, UBSan a signed-integer overflow, TSan a data race under `setarch -R`. This is now Task 3's design: a configured-but-inactive sanitizer is otherwise indistinguishable from a clean run, which would quietly void the project's central correctness claim.
- **Docker without sudo**, daemon reachable — the fact the whole Q5 answer turns on.

## What Didn't Work

- **`sudo apt install g++-10`** — password required, unavailable to this session. Not retryable here; this is what pushed the answer to a container.
- **`-march=x86-64-v2`** — does **not exist in GCC 10**; micro-arch levels landed in GCC 11. The build fails outright. I had recommended this value in the resolution doc an hour earlier; only a real build caught it. Now `-march=x86-64`.
- **`ctest --test-dir` on Debian's CMake 3.18** — does not error on the unknown flag. Runs **zero tests, prints `No tests were found!!!`, exits 0.** That is the exact command the dev-workflow guide § 6 prescribes for every checkpoint, so every TDD cycle would have reported green having run nothing. Fixed by pinning CMake 3.28.4 from the Kitware tarball (Debian's apt cmake is unusable for this project).
- **`setarch -R` alone inside the container** — `failed to set personality: Operation not permitted`; the default seccomp profile blocks `personality()`.
- **`--cap-add SYS_PTRACE --security-opt seccomp=unconfined` alone** — TSan still aborts with `unexpected memory mapping`. **Both are required together**; neither half is sufficient. Don't retry either alone.
- **`pip3 install cmake` in `gcc:10`** — there is no `pip3` in the image.
- **Constructing a numerical FMA divergence** — tried twice with values chosen to expose intermediate rounding; both times mul+add and FMA produced identical results. Only the *instruction-selection* difference was demonstrated (`vfmadd` emitted under `-march=native`, absent otherwise). The docs now carry that caveat explicitly rather than overclaiming.
- **Piping build output through `tail`** — masked a failing exit code and I reported a failed build as passing. Root cause of one wasted cycle; `scripts/ci.sh` in the plan now mandates `pipefail` because of it.
- **Bind-mounting without `--user $(id -u):$(id -g)`** — left root-owned files on the host. Cleaned up; the wrapper script now always passes `--user`.

## Test Coverage

- **Covered:** nothing in-repo — there is no source code yet. The toolchain, sanitizer, and build loop were verified in a scratch project outside the repo, which was **not preserved**; the plan encodes what it proved.
- **Not covered yet:** everything. P0 Task 2 creates the first in-repo test; Task 4 the first determinism assertion.

## Open Questions / Blockers

- **No git remote** — `git push` is impossible until one is added. Nothing has left this machine.
- **raylib GUI from inside the container** is unvalidated. WSLg is present (`DISPLAY=:0`, `/tmp/.X11-unix`), so passthrough is plausible but assumed. Flagged for validation at **P2**, not P0.
- **raylib is not packaged for Ubuntu 20.04 at all** — must come via FetchContent from source.
- **P7's WebSocket gateway** vs the README GIF — deliberately undecided; no information until a demo exists.

## Relevant Commits

- `42c1b7b` — remove the stale duplicate of the design spec
- `012980e` — resolve the five architectural questions gating P0
- `e12df4c` — add the P0 phase plan; correct the resolution against a built image
- `e6b4796` — (earlier, by hand) correct the design doc against a verified machine check

## Spec Changes

`docs/specs/2026-09-04-architecture-resolution.md` was added, then amended twice
in-session as builds falsified it: the `-march` value and the CMake floor. The
CMake change also **reversed** the design doc's "CMakePresets is polish" call —
correct for the unupgradable host, wrong for an image we control.

The coupling worth remembering: **the workflow guide's test command is only valid
because the image pins CMake ≥ 3.21.** Lowering the image's CMake floor breaks the
documented convention silently, with green checkpoints that ran nothing.

## Next Step

Execute P0 Task 1 (`Dockerfile`, `scripts/tw`, `scripts/verify-toolchain.sh`).
Its first checkpoint is deliberately RED **on the host** — the verification script
rejects GCC 9.4 — which turns the session's central finding into an executable
regression pin rather than a paragraph in a doc.
