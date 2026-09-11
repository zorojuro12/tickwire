# 2026-09-05 — ansh — P0 execution: toolchain, libsim, sanitizers, determinism, CI

**Status:** P0 is fully implemented, verified, and merged to `dev`. All six tasks from `docs/plans/2026-09-04-phase-0-toolchain-skeleton.md` are done: pinned toolchain container, CMake skeleton + `libsim` + GoogleTest, all three sanitizer configs (each proven live by a deliberate fault), the two-binary determinism harness, CI as a thin caller of `scripts/ci.sh`, and `CLAUDE.md`.
**Decided:** No new architectural decisions this session — this was pure execution of the plan committed on 2026-09-04. One implementation-level addition beyond the plan's exact file list: `scripts/determinism-test.sh`, a small shell driver for the `determinism_two_binaries` CTest test (the plan's contract implied this logic but didn't name a file for it).
**Spec:** No change — the design and architecture-resolution docs were not touched.
**Next:** Start P1 — see the design doc's phase table for scope. No plan exists for P1 yet; run the planning step first.
**Blocked on:** Nothing. Docker Desktop needed to be started at the top of this session (WSL integration was down); once up, execution was uninterrupted. Push is still blocked — no git remote configured.
**Touches:** `Dockerfile`, `scripts/*`, `CMakeLists.txt`, `src/sim/*`, `tests/*`, `tools/*`, `.github/workflows/ci.yml`, `CLAUDE.md`

---

## What We Worked On

Executed the P0 plan task-by-task via the `executing-plans` skill, on branch `phase-0-toolchain-skeleton` off `dev`. Each checkpoint followed RED→GREEN exactly as scripted in the plan: write/extend the test, confirm the predicted failure, implement the verbatim contract, confirm green, commit.

## What Worked

- Every RED step failed with exactly the message the plan predicted (host GCC major 9; `setarch: ... Operation not permitted`; missing `CMakeLists.txt`; `No tests were found` for each unregistered sanitizer/determinism test; `bash: scripts/ci.sh: No such file or directory`; missing `CLAUDE.md`). No surprises — the plan's verified-in-a-throwaway-project claim held up against the real repo.
- All three sanitizer configurations detect their deliberate faults: ASan reports `heap-use-after-free`, UBSan reports `signed integer overflow`, TSan reports `data race` under `setarch -R`.
- The two-binary determinism harness proves both directions: `digest_dump_a`/`digest_dump_b` agree byte-for-byte, and the comparator is shown to reject a deliberately mismatched pair (not just assumed to detect mismatches).
- `scripts/tw bash scripts/ci.sh` runs all three configurations plus the toolchain assertions in one call, green end to end — this is what the CI workflow calls, so CI correctness is verifiable locally.
- Final merge to `dev` (`--no-ff`) verified green post-merge before finishing.

## What Didn't Work

- Docker wasn't reachable at session start — `/usr/bin/docker` on this WSL2 (Ubuntu) distro symlinks into `/mnt/wsl/docker-desktop/`, which only populates when Docker Desktop is running on the Windows host. Fix was external (user started Docker Desktop); nothing to change in the repo. Worth knowing for future sessions on this machine: if `scripts/tw` fails with "docker: command not found" or a similar early error, check `docker info` before assuming a repo-level problem.

## Test Coverage

- **Covered:** `libsim::advance` and `kTickDt` (smoke test); ASan/UBSan/TSan each proven live by a fault test; determinism harness covers both the positive (binaries agree) and negative (comparator rejects mismatch) cases.
- **Not covered yet:** No coverage tooling (gcov/lcov) wired up yet — P0's `libsim` surface is deliberately tiny (one function), so the 100%-of-`libsim` bar from `docs/plans/2026-09-04-phase-0-toolchain-skeleton.md` Global Constraints is trivially met but not yet measured. Set this up before `libsim` grows in P2.

## Relevant Commits

- `4a92b54` — chore: pin toolchain container and add toolchain assertions
- `7802915` — chore: grant the container the capabilities TSan requires
- `8c96299` — feat: add CMake skeleton, libsim, and GoogleTest harness
- `a964572` — test: prove ASan is active by detecting a deliberate use-after-free
- `90ca9a1` — test: prove UBSan is active by detecting a deliberate signed overflow
- `22aa603` — test: prove TSan is active by detecting a deliberate data race
- `aa51a86` — test: add two-binary determinism harness with a falsifiable comparator
- `ef31ad5` — ci: run every build configuration through one script
- `9fba55d` — docs: add CLAUDE.md recording P0's verified constraints
- merge commit on `dev` (`--no-ff`) closing out `phase-0-toolchain-skeleton`

## Next Step

Plan P1 (see `docs/specs/2026-09-04-tickwire-design.md` for the phase table) — likely the full `libsim` surface (`World`, `PlayerState`, `WorldSnapshot`), given the architecture-resolution doc defers those from P0. Run the planning step before writing any P1 code, same as P0 was planned before execution.
