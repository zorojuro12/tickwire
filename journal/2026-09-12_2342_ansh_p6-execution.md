# 2026-09-12 — ansh — P6 execution: lag compensation

**Status:** All 10 tasks of the P6 plan executed on `phase-6-lag-compensation`
(cut from `dev`), inline (no delegation), TDD checkpoint by checkpoint. 17
commits, every task boundary green across plain/ASan/TSan. Task 9's mandatory
security review (security-reviewer + cpp-reviewer, run in parallel) found
**no CRITICAL or HIGH issues** — two LOW/INFO notes recorded, neither fixed
(both explicitly deferred per existing project precedent). Not yet merged to
`dev` — that's `finishing-a-development-branch`'s job next.
**Decided:** Nothing new beyond what the plan already locked in (protocol v3,
exact-reproduction rewind via shared `net::samplePlayerAt`, refusal-falls-back,
id-reuse bracket rule) — see `docs/project-history.md`'s P6 section, now
substantially expanded with Decision/Finding entries for each of these plus
the full Task 9 security write-up.
**Spec:** Updated — `docs/wire-format.md` (protocol v3: `InputCommand::view_tick`,
`kHitConfirm`), `CLAUDE.md` (wire-protocol heading, new `src/server/rewind.{h,cpp}`
file-structure row, new "rewind symmetry" verified-constraint entry),
`docs/project-history.md` (P6 section: Decisions, Findings, measured numbers,
full Task 9 security review), `README.md` (What works today → P6, Measured
results → Lag compensation subsection, Run the demo → lag-comp recipe,
What's next → P7).
**Next:** Human-verify the visual demo (the README's lag-comp recipe) and
record the result in `docs/project-history.md`, the way P2–P4 did — this
session's tools can't watch a WSLg-rendered window. Then run
`finishing-a-development-branch` to merge into `dev`.
**Blocked on:** Nothing.
**Touches:** `src/server/rewind.{h,cpp}` (new), `src/server/server.h`,
`src/server/session.{h,cpp}`, `src/client/client.h`,
`src/net/{protocol,framing,snapshot_ring}.{h,cpp}`,
`src/sim/{sim.h,world.h,world.cpp}`, `src/client/interpolation.cpp`,
`apps/tw_{server,client}.cpp`, `tests/server/rewind_test.cpp` (new),
`tests/server/server_lagcomp_test.cpp` (new),
`tests/client/client_lagcomp_test.cpp` (new),
`tests/client/lagcomp_hitrate_test.cpp` (new), `CMakeLists.txt`.

---

## What We Worked On

Executed the already-written P6 plan (`docs/plans/2026-09-12-phase-6-lag-compensation.md`)
task-by-task via the `executing-plans` skill: wire format v3 (Task 1),
`sim::resolveHitscan` extracted to a free function over a snapshot (Task 2),
`net::samplePlayerAt` shared between client render and server rewind
(Task 3), `server::buildRewoundView` with its full refusal/id-reuse rule set
(Task 4), `Server` resolving shots against the rewound view and confirming
hits (Task 5), `Client` stamping `view_tick` and hearing `kHitConfirm`
(Task 6), the hit-rate measurement (Task 7), demo/operator surface (Task 8),
the mandatory security review (Task 9), and this writeup (Task 10).

## What Worked

- **Every implementation checkpoint passed on the first real attempt** after
  its RED step — no checkpoint needed a second implementation pass. The
  plan's exactness (exact byte sequences, exact fixture tables, exact
  contracts) paid off directly: there was very little ambiguity to resolve
  while implementing.
- **Task 7's measurement test passed on its first run**, exactly as the plan
  predicted for a measurement task once Tasks 1–6 are correct: `lagcomp=on`
  hit_rate=1.000, `lagcomp=off` hit_rate=0.107, measured max rewind depth 24
  ticks — landing almost exactly on Task 4's ~24-tick estimate made before
  any code existed.
- **The `finishing-a-development-branch`-style discipline of running the
  full `ci.sh` at every task boundary** (not just per-checkpoint) caught
  nothing new this phase, but confirmed TSan/ASan stayed green through every
  stage, including the thread-sensitive Task 5 boundary
  (`threaded_runner_test`).
- Running `security-reviewer` and `cpp-reviewer` **in parallel as two
  background agents** worked cleanly — both returned independent, detailed,
  non-overlapping reports (security reasoned about the threat model per
  question the plan raised; cpp-reviewer reasoned about memory safety/thread
  safety/idiom) that were easy to reconcile into one write-up.

## What Didn't Work

- **The robustness-fuzz seed broke again** — same class of finding recorded
  at P1/P2/P4. Widening `kInputBytes` (25→29) and `kMaxMsgType` (6→7) shifted
  `RandomByteBuffersNeverCrashADecoder`'s draw sequence enough that seed 2
  (in place since P4) stopped producing a payload-shaped hit in 20,000
  trials. Re-searched upward from 1; seed 1 reliably hits. Not a defect, just
  the same seed-sensitivity every prior wire-format reopening has hit.
- **A stale golden-vector test the plan didn't name broke as a side effect.**
  `tests/net/framing_test.cpp`'s `FramePacketTest.BuildsAWholeDatagramWithACorrectPayloadLen`
  hardcodes the pre-v3 header version byte and payload length in its
  `expected_header` array; Task 1's plan text named `protocol_test.cpp`,
  `robustness_test.cpp`, and `framing_test.cpp`'s *new* `HitConfirmCodecTest`,
  but not this pre-existing test. Caught immediately by the Checkpoint 1
  verification command (its `-R` regex matched the whole file, so this test
  ran and failed), not silently missed — fixed in the same commit.
- **A GTest footgun, not a project bug:** `ASSERT_TRUE`/`ASSERT_GT` cannot be
  used inside a constructor (C++ forbids `return <expr>;` in a constructor,
  and GTest's `ASSERT_*` macros expand to exactly that). Hit this writing
  `MovingTargetScenario`'s constructor in `server_lagcomp_test.cpp` — fixed
  by using `EXPECT_*` there instead, which doesn't early-return.

## Test Coverage

- **Covered:** every new function (`sim::resolveHitscan` over a snapshot,
  `net::samplePlayerAt`, `server::buildRewoundView` and all its refusal
  rules, `SessionTable::joinedTick`, `Server::shots/rewoundShots/rewindsRejected`,
  `Client::setLagCompensationEnabled`/`hitsConfirmed`/etc.) has a dedicated
  unit test; the full shooter/target/rewind path is covered end-to-end at
  the `Server` level (`server_lagcomp_test.cpp`) and the real-network,
  real-latency level (`lagcomp_hitrate_test.cpp`).
- **Not covered yet:** the interactive/visual demo (HUD text, hit-flash ring,
  aim tracer, `L` key) — GUI drawing has no test surface, per the P2–P4
  precedent; needs a human running the README's recipe. `tw_client
  --selftest` only proves the display path opens/closes, not that the new
  drawing code renders correctly.

## Open Questions / Blockers

- The interactive demo verification (see **Next** above) is the one item
  this session's tools structurally cannot do — no session tool can watch a
  WSLg-rendered window, the same limitation P2–P4 each recorded.

## Relevant Commits

17 commits on `phase-6-lag-compensation`, `dev..HEAD`:
`4d7b40e` view_tick/protocol v3, `bd1800c` HitConfirm message type,
`ae88cc9` resolveHitscan over a snapshot, `d339c19` shared snapshot sampling,
`ac7e171`/`b361a00`/`32a52b1` buildRewoundView (build/refuse/id-reuse),
`e366bb1`/`bd2e40a`/`a69372e` Server resolves/confirms/resets,
`9c6f60f`/`b289220` Client stamps view_tick/counts hits,
`4009adf` the hit-rate measurement, `c504881`/`a03d813` demo/operator surface,
`0de4ec4` security review findings, `be9e74d` README writeup.

## Spec Changes

- `docs/wire-format.md`: protocol version 3 (`InputCommand::view_tick`,
  `MsgType::kHitConfirm`), new `HitConfirm` payload section, decoder-strictness
  and "Version history" entries.
- `CLAUDE.md`: Wire protocol heading now says v3; `src/server/` row gains
  `rewind.{h,cpp}`; new Verified Constraints entry for rewind symmetry
  (shared `net::samplePlayerAt`), naming the tests that prove it.
- `docs/project-history.md`: P6 section expanded with Decision entries
  (`kMaxRewindTicks` derivation, refusal-fallback, id-reuse bracket rule),
  the Task 7 measured numbers, two execution Findings (seed, stale golden
  vector), and the full Task 9 security review write-up (every question
  answered, all verified closed, two LOW/INFO notes).
- `README.md`: "What works today" now covers through P6 (wire protocol v3,
  the rewind bullet); new "Lag compensation" subsection under Measured
  results with the hit-rate table; "Run the demo" gained the lag-comp
  recipe (careful to run server+loadclient+client in *one* `scripts/tw`
  invocation, since separate invocations get separate Docker network
  namespaces and can't reach each other over loopback); "What's next" is
  now P7.
