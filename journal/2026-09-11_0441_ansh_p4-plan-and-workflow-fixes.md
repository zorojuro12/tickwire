# 2026-09-11 — ansh — P4 planning, P3's GUI verification, and a checkpoint-template defect

**Status:** P3 is now fully closed — its one remaining open item (human verification that prediction *looks* convincing) was run and recorded. P4 is planned and committed but not started: `docs/plans/2026-09-11-phase-4-interpolation-snapshot-delta.md`, 11 tasks / 38 checkpoints / 38 commits. Three workflow-and-docs fixes also landed. Working tree clean, `dev` four commits ahead of where the session opened, `main` still at bootstrap as before.
**Decided:** Six P4 architectural questions settled up front in the plan's "Decisions this plan makes" section — see it rather than a restatement here. The load-bearing two: **a new additive message type (`kSnapshotDelta = 6`) instead of a wire-format version bump**, using the extension path `docs/wire-format.md` designated for itself, and **the client→server `ack_tick` field as the delta baseline channel**, which has been present and always-zero since P1 so the baseline costs zero new bytes. Separately: skill adaptation records are now written hazard-first, and the standing "don't promote rules to `~/projects/claude-skills`" verdict was re-examined and **kept**.
**Spec:** No change. `docs/wire-format.md` is deliberately untouched — P4's plan schedules its amendment at Task 11 (a new `SnapshotDelta` payload section, the `ack_tick` row amended for its new client→server meaning, and a version-history entry recording that `kProtocolVersion` stays **2**).
**Next:** Execute the P4 plan with the `executing-plans` skill — either inline, or from a fresh Sonnet session per the two-model loop; the plan carries its own Global Constraints and needs no conversation history.
**Blocked on:** Nothing.
**Touches:** `docs/plans/2026-09-11-phase-4-interpolation-snapshot-delta.md`, `docs/dev-workflow-guide.md`, `docs/project-history.md`, `CLAUDE.md`, `.claude/skills/writing-plans/SKILL.md`

---

## What We Worked On

Three things, in order, with the third arising out of the second.

**Resumed from P3's journal**, confirmed `dev` and `phase-3-prediction-reconciliation` are the same commit (the branch had already been folded in, which P3's journal predated), and that the only thing P3 left open was a verification no session can perform.

**Ran that verification.** P3's journal and P2's before it both recorded the same limitation: the GUI renders through WSLg onto the host desktop and no session tool can capture or click into it, so "does prediction actually look convincing?" needed a human. It now has an answer — recorded in `docs/project-history.md` under a new "Interactive feel — human-verified after phase completion" heading.

**Planned P4** via `writing-plans`, in Opus per the guide's two-model loop. The plan settles six architectural questions before any code is written, on the principle the workflow guide states: answering them inside a phase plan's execution is how they get answered badly.

## Decisions Made

- **P4's six architectural decisions** — recorded in full, with reasoning, in the plan's own "Decisions this plan makes" section. Not restated here. The one that is a deliberate *deviation* rather than a choice: the architecture-resolution doc names `radius` as "the first field to drop from a delta", and the plan instead drops the entire 24-byte record for unchanged players while keeping `radius` in the records it does send. Dropping `radius` saves 4 bytes but forces a variable-size record for players absent from the baseline; keeping it buys a byte-exact "delta + baseline == full snapshot" property. The deviation is scheduled to be recorded in `docs/project-history.md` at Task 11 so the resolution doc is not silently contradicted.
- **Skill adaptation records are written hazard-first** — `Rule / Prevents / Translating it elsewhere`, not a wording diff. Reasoning is in `docs/dev-workflow-guide.md` § Skill adaptation record, written inline there so it survives without this journal.
- **The "don't promote adapted rules to the library" verdict stands** (originally 2026-09-04). It was re-examined because a rule was demonstrably lost this session — but lost translating *out of CallIt*, which promotion would not have prevented. The decision row now carries that reasoning plus a real revisit trigger (a rule lost translating out of the **library** itself), replacing the bare `—` it had.

## What Worked

- **The GUI interactive feel test passed**, closing the item P2 and P3 both deferred. With `pred=on` (green) movement tracked input essentially instantly; `pred=off` (yellow) at 200 ms was described as "way more laggy and like low frame rate"; `[`/`]` visibly moved the HUD's `latency=` value at runtime.
- **`scripts/ci.sh` is green** — all three configurations plus the toolchain assertions — run specifically to check that the checkpoint-template defect below had not left P3 with a real problem. It had not: 24/24 tests pass against freshly built binaries.
- **The corrected checkpoint command form was validated before being written into 38 checkpoints**, rather than after: `scripts/tw bash -c "cmake --build build/plain -j8 && ctest --test-dir build/plain -R world_test --output-on-failure"`, 3.3 s end to end.

## What Didn't Work

- **Launching `tw_server` and `tw_client` as two separate `scripts/tw` invocations.** Each `scripts/tw` is its own `docker run`, so the two processes landed in two network namespaces and the client's `127.0.0.1:<port>` never reached the server. The symptom is quietly misleading: the window opens, renders, and runs at 60 FPS forever at `tick=0 players=0 rtt=0ms`, with **no error in either process's log** — it looks like a client-side join bug, not a topology mistake. The fix is one container running both (`scripts/tw bash -c '<server> & … <client>'`), which is what `scripts/demo.sh` was already shaped for. The correct invocation is written into the P4 plan at Task 9 Checkpoint 3 so the next session doesn't rediscover it.
- **Wrapping `setarch -R` around the build** in a first draft of the plan — it wraps `ctest` only (`scripts/ci.sh:18`). Caught by reading `ci.sh` rather than by a failure. Both the plan's Global Constraints and the skill now state the placement explicitly.
- **Docker Desktop was not running at session start**, and `scripts/tw` fails with a WSL-integration message that reads like a configuration problem rather than "the daemon is down". Checking `docker.exe version` distinguishes the two (it reported the engine pipe missing).

## Test Coverage

- **Covered:** nothing new — this session changed no production code. The verification performed was existential rather than additive: full `ci.sh` green across plain/ASan/TSan plus toolchain, and `tw_client --selftest` green against a rebuilt GUI configuration.
- **Not covered, and knowingly so:** P4 itself is entirely unimplemented. Every test named in the plan is a test that does not exist yet.
- **Not recoverable:** whether P3's individual checkpoints genuinely went red. The committed code is verified correct, but the *evidence* that each red step was red cannot be reconstructed retroactively — see below.

## Open Questions / Blockers

- None blocking. One question was raised and deliberately declined rather than left open: whether the generalizable half of the false-green rule should be promoted to `~/projects/claude-skills`. Declined per the standing verdict; the reasoning and the new revisit trigger are recorded in `docs/dev-workflow-guide.md`.

## The checkpoint-template defect, in full

Worth recording because the interesting part is not the bug but why the existing safeguard didn't catch it.

`ctest` does not build. `scripts/ci.sh` runs `cmake --build` before every `ctest` for that reason, but the `writing-plans` skill's Test Commands section prescribed a bare `ctest --test-dir build -R <regex>`, and the P3 plan used that form at all 35 of its checkpoints — while asserting "Expected: FAIL — compile error", an outcome bare `ctest` cannot produce. A bare run either executes the previously built binary (so a newly written test case is simply absent and the run is falsely green) or matches no target and exits 0.

Provenance, traced rather than assumed: the section is **not** from ECC (which has no `writing-plans` skill) and not from upstream `obra/superpowers` or the `~/projects/claude-skills` library — neither has a Test Commands section at all. It was invented in CallIt, where it was **correct**: `go test` caches results, hence `-count=1`, and `go test` also builds, so no build step was needed. Tickwire's bootstrap (`1620175`) translated it to CMake, correctly dropped the caching advice — and missed that the other half of the Go→CMake difference was that `ctest` does not build.

The safeguard that should have caught this is the adaptation record in `docs/dev-workflow-guide.md`, which *already documented this exact translation*: "both sources say include the flag that defeats cached results (`-count=1` in Go). That doesn't transfer — `go test` caches, CTest does not — so only the scoping half survives here." Accurate, and the rule was still lost, because it recorded the **diff** rather than the **hazard**. Stated as a wording difference, "caching doesn't apply here" ends the thought. Stated as a hazard — *the runner reports green without having run the new test* — it forces the next question, "then what is this runner's route to a false green?", whose answer is a stale binary. Hence the record's restructuring into `Rule / Prevents / Translating it elsewhere`.

Fixed at both sources rather than in the P4 plan alone: `CLAUDE.md`'s Build and test section (auto-loaded, so every session gets it, not only plan-writing ones) and this project's copy of the skill.

## Relevant Commits

- `5e366ef` — docs: record human-verified prediction interactive feel test (closes P3's last open item)
- `70d0239` — docs: add the P4 phase plan for interpolation and snapshot delta (2,058 lines)
- `4b9a26c` — docs: record that ctest does not build, at both sources of the mistake
- `b457de1` — docs: record skill adaptations by hazard, not by wording diff

## Next Step

Execute `docs/plans/2026-09-11-phase-4-interpolation-snapshot-delta.md` with `executing-plans`, starting at Task 1 (`net::SnapshotRing`). The plan's task order is a dependency order — Tasks 1–6 are the snapshot-delta half and Tasks 7–9 the interpolation half, and the two only meet in `Client<T>`, so a reviewer could reasonably stop after Task 6 and still have a shippable improvement.
