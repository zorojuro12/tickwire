# 2026-09-12 — ansh — P5 execution: threading, SpscRing, benchmark numbers

**Status:** P5 complete. All 13 tasks executed inline (no delegation) on
branch `phase-5-threading-queue-benchmark`, verified green on `scripts/tw
bash scripts/ci.sh` (plain/ASan/TSan + toolchain assertions) after every task
boundary and again after the security review's fixes. The headline numbers
table exists at `docs/benchmarks.md`, linked from `README.md`'s new
"Measured results" section. Branch not yet merged into `dev` — that's
`finishing-a-development-branch`'s job next.
**Decided:** `recvmmsg` batching rejected as the default (`--batch-ingest`
stays opt-in) — Group 4 showed under 0.5% throughput difference at real
load. Lock-free wins measurably at every rate tested but the gap is
three-plus orders of magnitude too small to matter at Tickwire's real 640
packets/sec — a more precise finding than the plan's predicted
"indistinguishable," reported as measured rather than rounded to match the
prediction. Full reasoning for both, plus the ring-as-template-parameter and
transport-without-a-mutex decisions, in `docs/project-history.md`'s P5
section.
**Spec:** No change to the wire format (`kProtocolVersion` stays 2,
`sim::kMaxPlayers` stays 32, per the plan's Global Constraints). New doc:
`docs/benchmarks.md`. Updated: `README.md` (Measured results section, phase
status corrected from stale "through P3"), `CLAUDE.md` (file-structure rows,
new Verified-constraints entry for the two-thread ownership split),
`docs/project-history.md` (P5 decisions and Task 12 security findings).
**Next:** Hand off to `finishing-a-development-branch` to decide the merge.
Per `docs/dev-workflow-guide.md`, expected choice is self-merge into `dev`
with `--no-ff` (no PR ceremony in this project).
**Blocked on:** Nothing.
**Touches:** `src/server/{spsc_ring,mutex_ring,threaded_runner,jitter_stats}.h`,
`src/net/udp.{h,cpp}`, `src/server/server.h`, `apps/tw_server.cpp`,
`tools/bench_queue.cpp`, `scripts/bench.sh`, `docs/benchmarks.md`,
`tests/server/{mutex_ring,spsc_ring,threaded_runner,jitter_stats}_test.cpp`,
`tests/faults/relaxed_ring.cpp`, `tests/net/udp_test.cpp`.

---

## What We Worked On

Executed the P5 phase plan (`docs/plans/2026-09-12-phase-5-threading-queue-benchmark.md`)
task-by-task via the `executing-plans` skill, inline (the plan's header
didn't mark any task for delegation). Reviewed the plan critically first —
no concerns worth raising; it already documented fallbacks for its own known
risk points (the `alignas` assertion, the TSan fault gate, a ctest no-match
trap). Created the feature branch and went through all 13 tasks in order:
bounding the ingest retry loop (P1's deferred finding), a nanosecond clock
and `JitterStats`, `MutexRing` then `SpscRing` behind `PacketRing`'s
four-call contract, a TSan fault binary proving the TSan gate has teeth,
making `Server`'s ring a template parameter, `ThreadedRunner` splitting
ingest from tick across two real threads, wiring `tw_server`'s CLI, the
`bench_queue` microbenchmark, `recvmmsg` batching, `scripts/bench.sh` and
the real measured table, the mandatory security review, and this writeup.

## Decisions Made

- **Ring made a template parameter on `Server`, not swapped in some other
  way** — `Server<T, Ring = PacketRing<...>>` keeps every existing call site
  compiling unchanged. See `docs/project-history.md`'s P5 section.
- **Transport shared between the I/O and sim threads without a mutex** —
  verified against the real `UdpTransport` implementation (touches only the
  fd, no shared member state; POSIX permits concurrent `sendto`/`recvfrom`),
  not just assumed from the `Transport` concept. A transport mutex would
  have serialized the benchmark's two arms.
- **No egress queue** — per the design doc's standing instruction; nothing
  measured this phase produced a number that would justify one.
- **`recvmmsg` rejected as the default** — built as a measured, opt-in
  variant (`--batch-ingest`) per the `benchmark-optimization-loop` skill's
  promotion gate; Group 4's measurement showed no win worth adopting.
- **CMake's built-in regex engine has no `{n}` bounded-repetition support**
  — the plan's own `[0-9]{6}` regex for `server_app_sim_load`'s
  `PASS_REGULAR_EXPRESSION` silently failed to match a correct value.
  Expanded to six literal `[0-9]` terms with identical meaning. Worth
  carrying forward to any future CTest regex in this project.

## What Worked

- **The plan's own documented "correctness pin" allowance** — several
  checkpoints (Task 3 CP2, Task 4 CP2/CP3, Task 9 CP2, and a few more that
  came up during execution) passed the moment they were written because an
  earlier checkpoint's implementation already satisfied them. Recording each
  explicitly as a correctness pin rather than forcing an artificial failure
  kept the TDD discipline honest without wasted motion.
- **Reading the actual `PacketRing`/existing test files before writing new
  ones** made `MutexRing`/`SpscRing`'s contract match byte-for-byte on the
  first attempt — no rework needed when Task 6 substituted them into
  `Server`.
- **`scripts/bench.sh --smoke` dogfooding itself caught a real bug before
  the real measurement run**: an 8 MB `JitterStats` accidentally
  stack-allocated in `bench_queue.cpp` (stack overflow, caught via ASan),
  and separately a cross-core `CLOCK_MONOTONIC` read producing a tiny
  apparent inversion that unsigned subtraction wrapped to ~`UINT64_MAX`,
  poisoning every percentile in the affected arm. Both are recorded in
  `docs/project-history.md`'s P5 section and fixed before the real run.
- **The mandatory Task 12 security review (security-reviewer + cpp-reviewer
  in parallel) found a real CRITICAL** — `receiveBatch()` misattributed
  payload bytes to the wrong sender/length whenever a batch mixed an
  oversized datagram with valid ones, reachable via `--batch-ingest`. Two
  independent agents converging on a strong, well-reasoned finding from one
  of them (the CRITICAL came from `cpp-reviewer`, not `security-reviewer` —
  both were run in parallel over the same files, and this is exactly why:
  neither agent alone was guaranteed to catch it, and running both cost
  nothing extra in wall time).

## What Didn't Work

- **Piping a gated verify command through `| tail -N` silently defeats the
  `&&`-chained commit gate.** Early in Task 1, `scripts/tw bash -c "... &&
  ctest ..." | tail -20 && git commit ...` let a genuinely failing test's
  exit code get replaced by `tail`'s (always 0), so the commit landed despite
  a red test. Caught immediately by noticing the commit succeeded when it
  shouldn't have; fixed with a follow-up commit (not an amend, per this
  session's git safety rules) correcting the test's own wrong assertion
  (unrelated second bug: the plan's own worked example assumed 32 total
  oversized skips from only 20 sent datagrams, which is arithmetically
  impossible — applied the plan's own documented fallback assertion).
  **Lesson for any future checkpoint-gated verify command in this project:
  never pipe the build+test command through anything that changes the
  pipeline's exit status** — run it bare, or use `set -o pipefail` if piping
  is unavoidable.
- **The plan's literal `[0-9]{6}` CTest regex** (see Decisions Made above) —
  don't assume CMake's regex engine supports standard bounded-repetition
  syntax; it doesn't.

## Test Coverage

- **Covered:** every new module (`MutexRing`, `SpscRing`, `JitterStats`,
  `ThreadedRunner`, `receiveBatch`, `ingestBatch`, the `tw_server` CLI
  surface) has unit tests in `tests/server/` and `tests/net/`, plus
  TSan-specific verification (`tsan_detects_relaxed_ring` proving the TSan
  gate itself has teeth, and the ordering/thread-boundary tests for both
  rings and the runner). `bench_smoke` and `bench_queue_{spsc,mutex}` keep
  the measurement harness itself under CI. Full suite: 40/40 across
  plain/ASan/TSan.
- **Not covered yet:** the ring-saturation fairness gap recorded as a
  deferred MEDIUM finding (no per-source admission control — same root
  cause as the pre-existing P2 CRITICALs) has no regression test, since
  fixing it is out of this phase's scope. `LoopbackTransport`'s lack of
  internal thread-safety is a latent footgun for a future test author, not
  currently exercised concurrently by any test.

## Open Questions / Blockers

None. Two things a future phase might revisit, both already argued in
`docs/project-history.md`: whether per-source rate limiting/admission
control is ever added (it's the same root cause as the P2 CRITICALs, and
still out of scope for whatever comes next unless a phase explicitly takes
on session authentication), and whether `ingestBatch`'s 38 KB per-call
stack-zeroing is worth optimizing if a future `--batch-ingest` measurement
ever looks off.

## Relevant Commits

Twenty-some commits on `phase-5-threading-queue-benchmark`, one per
checkpoint per the plan's convention; the ones worth calling out by name:

- `31d8360` — `fix:` preserve payload/metadata pairing in `receiveBatch`
  across a skip (the Task 12 CRITICAL)
- `b0093ad` — `fix:` clamp `bench_queue`'s handoff latency against
  cross-core clock skew
- `8aaca20` — `docs:` record the P5 measured numbers table
- `93e0c2e` — `docs:` record the P5 security review findings

## Spec Changes

No change to `docs/wire-format.md` (still version 2) or the design/
architecture-resolution docs. `docs/benchmarks.md` is new. `README.md` and
`CLAUDE.md` updated as described above. `docs/project-history.md` gained a
substantial P5 section: decisions, two measurement findings, and the full
Task 12 security review writeup.
