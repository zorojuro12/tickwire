# 2026-09-09 — ansh — P3 execution: prediction, reconciliation, clock sync, full phase completion

**Status:** All 10 tasks of `docs/plans/2026-09-08-phase-3-prediction-reconciliation.md` complete on `phase-3-prediction-reconciliation`. Full `scripts/tw bash scripts/ci.sh` (plain/ASan/TSan/toolchain) green, plus the GUI build and `client_selftest` with a real display. Mandatory Task 8 security review done (two agent reviews); one CRITICAL and one HIGH finding fixed with regression tests, three lower-severity findings deferred with recorded reasoning, all prior P1/P2 findings re-verified (not assumed) after the rewrite. Docs updated (wire-format annotations, `CLAUDE.md`, `README.md`, full `docs/project-history.md` P3 section). Branch not yet merged into `dev`.
**Decided:** Two CRITICAL/HIGH security findings fixed outright rather than deferred (client accepting packets from any source; a departed player's queued input surviving into a reused id) — unlike P2's three deferred findings, both required no wire-format change and had cheap, clean fixes with no scope tradeoff, so there was no genuine decision to put to the user. Three lower-severity findings (a diagnostic-only int32 truncation, InputBuffer's ~2.3-year uint32 wraparound, `hits_` not reset on id reuse) deferred with reasoning, matching P1/P2's established treatment.
**Spec:** `docs/wire-format.md` — no byte layout change (`kProtocolVersion` stays 2); rewrote the `tick`/`send_time_ms`/`ack_tick` annotations from "reserved for P3" to what now consumes them, and added a Version history entry recording that the P1 reservation worked as intended. `docs/project-history.md` gained the full P3 section (pivot, five decisions/findings, two bugs found and fixed via real-latency testing, the measured convergence numbers, and the security review). `CLAUDE.md` gained the tick-matched-consumption/underrun-repeat symmetry invariant.
**Next:** Hand off to `finishing-a-development-branch` for the merge/PR/keep decision, then start P4 (entity interpolation, snapshot delta) per the design doc's phase table.
**Blocked on:** Nothing. One thing this session cannot verify: the GUI's actual interactive feel (whether P's effect and the latency slider look as convincing as the design doc claims) — needs a human to run the demo and watch, same limitation P2 recorded.
**Touches:** `src/sim/world.{h,cpp}`, `src/server/{server.h,input_buffer.{h,cpp}}`, `src/client/{client.h,clock_sync.{h,cpp},prediction.{h,cpp}}`, `apps/{tw_loadclient,tw_client}.cpp`, `scripts/e2e-udp.sh`, `tests/**`, `docs/{wire-format,project-history}.md`, `CLAUDE.md`, `README.md`, `CMakeLists.txt`

---

## What We Worked On

Executed the P3 phase plan task-by-task via the `executing-plans` skill, inline, across one long session starting from a clean `dev` with only the plan committed. Built, in order: `World::setPlayerState` (Task 1), `InputBuffer` (Task 2), the server's tick-matched input consumption replacing P2's apply-on-arrival (Task 3 — the riskiest task, the only one rewriting shipped P2 behavior), `ClockSync` (Task 4), the client's clock-ahead-of-server seeding and correction (Task 5), local prediction plus RTT estimation (Task 6), reconciliation replay and error stats (Task 7), convergence testing against a real server plus the mandatory security review (Task 8), the load client's stats output and the GUI's prediction toggle (Task 9), and the documentation pass (Task 10).

Task 8 was where the phase actually got hard, matching the design doc's own warning that P3 is "the death phase." The convergence test's zero-latency checkpoint passed after a tolerance recalibration, but the 200ms-real-latency checkpoint uncovered a chain of three compounding, genuine production defects that no earlier unit test could have caught, because none of Tasks 1–7's checkpoints drove the system over a real, delayed transport.

## Decisions Made

- **Server input buffering replaces apply-on-arrival — see `docs/project-history.md`'s P3 pivot entry** for the full reasoning (Gambetta's simpler model was considered and rejected). This was decided at planning time, not this session, but it's the single biggest architectural fact this session built on.
- **Two-pass fire resolution, but the reason it matters got corrected mid-execution.** The plan's own Task 3 Checkpoint 3 assumed `applyInput` integrates position immediately; tracing `World::step()` precisely showed position never changes until `step()` runs once at the end of the tick, after both passes — so pass-ordering doesn't actually affect hitscan outcome the way the plan's literal test scenario assumed. The two-pass split is still correct and worth keeping, just not for the reason first written down. See `docs/project-history.md`.
- **`predicted_scratch_` converted from a persistent mutable member to a local variable** (cpp-reviewer's MEDIUM finding) — it was never used to carry state between calls, just as a call-scoped `writeSnapshot` buffer.

## What Worked

- **RED-first discipline caught real, non-trivial bugs again**, not just typos: `input_underruns_`'s join-tick off-by-one (Task 3 Checkpoint 1); the arena clamp silently swallowing a test's authoritative position (`x=100.0f` outside the ±49.5 clamp) and producing a misleading "reconciliation bug" symptom (Task 7 Checkpoint 2); `setPredictionEnabled`'s own contract text contradicting its own checkpoint's test (Task 6 Checkpoint 3) — three separate plan-writing mistakes from the *same planning session*, each caught by the checkpoint that followed it, not by re-reading the plan.
- **The real-latency convergence test earned its keep decisively.** It found things zero-latency and hand-built-packet tests structurally cannot: a join-seed that doesn't account for the accept packet's own transit delay, a clock controller that oscillates with growing amplitude under dead time, and an acceptance window too narrow for the phase's own headline demo scenario to bootstrap in. All three were root-caused via iterative debug tracing (not guessed at) and fixed with real, verified-RED-then-GREEN changes to already-committed Task 4/5 files.
- **The security review found two real, serious issues neither of us had considered**, both closed with cheap fixes and no wire-format cost: the client never checked that inbound packets came from the server it joined (zero spoofing needed to exploit), and a departed player's already-queued future input could execute under whoever inherited their reused player id.
- **The GUI display path still works cleanly** (WSLg X11 passthrough, Mesa/llvmpipe), and the demo smoke-test ran ~10s with two windows, no crashes, correct 60 FPS target — matching P2's precedent.

## What Didn't Work

- **Comparing the predicting client's position against the server's own live tick, as the plan's Checkpoint 2 literally specified, produced a fundamentally wrong test.** Prediction is deliberately ahead of the server (that is what prediction means), so that comparison always shows a gap proportional to the lead margin and penalizes prediction for doing its job — it never converges to near-zero no matter how correct the implementation is. Root-caused by hand-tracing the actual tick arithmetic (found the real gap was `~11 ticks`, not the `~4` `clockLead()` reports, because `clockLead()` itself reports a stale, round-trip-old value). Fixed by comparing both clients against the zero-latency ideal trajectory instead — the comparison that actually matches the design doc's demo claim.
- **A proportional (not just sign) correction step made `ClockSync`'s oscillation worse, not better** — larger steps amplify dead-time instability rather than fixing it. Reverted; the fix that actually worked was a cooldown limiting correction frequency, not correction magnitude.
- **Oscillating test movement (flipping direction every 100 ticks) to avoid the arena wall introduced its own measurement artifact** — a transient right at each flip could coincidentally show near-zero error. Replaced with a cleaner two-phase design (long stationary settle, then one continuous movement burst), decoupling "give the clock time to converge" from "don't hit the wall."
- **`ack_tick=0` used as a deliberately-extreme test value in a standalone repro** silently no-opped, because `0` is `ClockSync::observe`'s own "no input acknowledged yet" sentinel — a reminder that a value chosen purely to be "large" can collide with a domain-specific magic value.

## Test Coverage

- **Covered:** the full phase — `World::setPlayerState`'s full contract; `InputBuffer`'s window/highestTick/reset contract; the server's tick-matched consumption including underrun-repeat and fire-ordering (one regression pin passed on first write, as the plan explicitly permitted); `ClockSync`'s sign/snap/clamp/cooldown behavior; client clock seeding, correction, and underflow saturation; `PendingInputs`/`PredictionStats`; reconciliation replay including the underrun-gap case; two convergence tests (zero-latency in-memory, 200ms-RTT real-UDP) proving the phase's actual demo claim with measured numbers; both security fixes verified RED-then-GREEN with dedicated regression tests.
- **Not covered, and not coverable by this session:** the GUI's actual interactive/visual behavior. The windows render through WSLg onto the host desktop; this session has no tool to capture or click into them. Needs a human to run `scripts/tw bash scripts/demo.sh ...`, drag the latency slider to ~200ms, press `P`, and confirm the difference looks as convincing as the design doc claims — same limitation P2 recorded for its own display path.

## Open Questions / Blockers

- None. `main` is still at the original bootstrap commit with none of P0–P3's history (an intentional-looking `dev`-accumulates/`main`-lags model from earlier sessions, not reconsidered this session).

## Relevant Commits

31 commits on `phase-3-prediction-reconciliation`, `d00e80f`..`87f63b1` — see `git log --oneline dev..phase-3-prediction-reconciliation`. Highlights: `a721e74` (the riskiest task — tick-matched server input consumption), `4bd8d9f` (Task 8's convergence tests plus all three real-latency production fixes), `d31dfcc`/`fe291cc` (the two security fixes), `87f63b1` (Task 10 docs).

## Next Step

Hand off to the `finishing-a-development-branch` skill for the merge/PR/keep decision — not this session's call to make unilaterally. Once resolved, P4 (entity interpolation, snapshot delta) is next per the design doc's phase table; no P4 plan exists yet.
