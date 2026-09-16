# 2026-09-12 — ansh — P5 plan, and auditing the skill import map

**Status:** P4 is merged into `dev` (branch kept, not deleted) and verified
green on a fresh build — 26/26 tests including `determinism_two_binaries` and
`e2e_udp`. The P5 plan is written and committed to `dev`
(`docs/plans/2026-09-12-phase-5-threading-queue-benchmark.md`, 13 tasks,
~28 checkpoints). No code written this session. Nothing blocked.
**Decided:** `benchmark-optimization-loop` imported for P5; its two companions
named in the workflow guide's import map (`benchmark`, `benchmark-methodology`)
rejected on reading them, along with two further rows closed. The load-bearing
P5 design decisions are argued in the plan itself and recorded in
`docs/project-history.md`'s P5 section — not restated here.
**Spec:** No change to the wire format or the design docs.
`docs/dev-workflow-guide.md` corrected (§2 status block, three §3a rows, §3
"installed now"); `docs/project-history.md` gained its P5 section.
**Next:** Execute the P5 plan. Per the guide's §2a two-model loop this hands off
to a fresh Sonnet session — the plan carries its own Global Constraints and
needs no conversation history. User has not yet chosen inline vs. handoff.
**Blocked on:** Nothing.
**Touches:** `docs/plans/2026-09-12-phase-5-threading-queue-benchmark.md`,
`docs/dev-workflow-guide.md`, `docs/project-history.md`,
`.claude/skills/benchmark-optimization-loop/`

---

## What We Worked On

Started as a resume-and-orient session, became an audit plus a planning session.

Three pieces. First, getting current on P4 (merged, clean, nothing outstanding).
Second — prompted by the user asking whether any skills or rules still needed
importing — an audit of `docs/dev-workflow-guide.md` §3a's phase-by-phase import
map against what those skills actually contain. Third, writing the P5 phase plan.

The audit was the unplanned part and produced the session's only real finding.

## Decisions Made

- **Imported `benchmark-optimization-loop`; rejected `benchmark` and
  `benchmark-methodology`** — the other two the import map named for P5. Reason:
  the map was built from a name inventory (`~/projects/ecc-survey.md`) without
  opening a single `SKILL.md`, and the names mislead. Full reasoning in
  `docs/project-history.md`'s P5 section; the short version is that
  `benchmark-methodology` is competitive *marketing* analysis and `benchmark` is
  web/cloud-only (Core Web Vitals, HTTP endpoints, JS/Docker build times).
- **Closed two further import-map rows rather than importing them.** The
  `security-scan` *skill* row asserted a distinction from the installed
  `/security-scan` *command* that does not exist — same `.claude/` target, same
  `npx ecc-agentshield` engine. The `documentation-lookup`/`docs-lookup` row's
  trigger did fire at P2 (raylib) and was missed then, but both are inert: no
  Context7 MCP is configured anywhere, and that MCP is the skill's entire
  mechanism.
- **Evaluated `latency-critical-systems` even though the map never listed it**,
  because its description (p95 latency, hot paths, queues) reads like a direct
  P5 hit. Rejected — its hot path is `provider API → ingest worker → queue →
  cache → edge route → browser render` and its optimization order is about round
  trips and cache freshness. Only its "Split The Metrics" list transfers, and
  the design doc already fixes Tickwire's four metrics.
- **P5's own design decisions are in the plan, not here** — the ring as a
  defaulted template parameter, the transport shared without a mutex, no egress
  queue, `recvmmsg` as a measured variant with a promotion gate, `kMaxPlayers`
  staying at 32. Each is argued where it will be read.
- **Renamed the plan `2026-09-11-…` → `2026-09-12-…`** after noticing the
  session crossed midnight and the plan actually committed at 01:07 on the 12th.
  It also collided confusingly with the P4 plan's date. No external references
  to fix.

## What Worked

- **Reading the skills instead of trusting the map found three wrong rows out of
  four.** Cheap check, and the alternative was discovering it at the exact phase
  that depends on them.
- **Verifying green with a real build rather than trusting the previous
  session's journal.** `scripts/tw` plain config, full suite: 26/26 including
  `determinism_two_binaries` and `e2e_udp` (10.02 s). The journal's claim held,
  but now there's evidence for it rather than a citation.
- **Reading the code before writing the plan surfaced three things that would
  otherwise have been execution-time discoveries**, all now written into the
  plan:
  - `monotonicMs()` *cannot* measure tick jitter — at millisecond resolution
    every 16.67 ms interval reads as 16 or 17, so the deviation is smaller than
    the measuring unit. A nanosecond clock became a prerequisite for the
    headline row.
  - The transport needs **no mutex** across the thread split. `UdpTransport::send()`
    and `tryReceive()` both read `fd_` and mutate no member state
    (`src/net/udp.cpp:64-97`), and POSIX permits concurrent `sendto`/`recvfrom`
    on one fd. This matters beyond tidiness: a transport mutex would serialize
    the two halves and make the benchmark measure the wrong thing.
  - The **counters**, not the ring, are the remaining hazard — the main thread
    reads `droppedPackets()` etc. while workers run. Resolved by joining both
    threads before reading any counter, rather than making them atomic (which
    would add hot-path cost to satisfy a shutdown-time read).
- **Verifying three plan claims a cold executor would trip on**, rather than
  asserting them: `LoopbackTransport` genuinely lacks `nativeHandle()` (so the
  plan's `if constexpr (requires { … })` fork is correct), `client_selftest` is
  the real CTest name with `SKIP_RETURN_CODE 77`, and `Threads`/`pthread` is
  genuinely absent from `CMakeLists.txt` (so Task 3's `find_package` is needed).

## What Didn't Work

- **Trusting a skill name to describe a skill.** `benchmark-methodology` matched
  P5 on the word "benchmark" and nothing else. Don't re-derive an import
  recommendation from a name inventory — reopen the `SKILL.md`.
- **The first attempt at Task 4's `alignas(64)` checkpoint was unfalsifiable as
  written** — it asserted the cache-line separation via `offsetof` on private
  members, which does not compile. Caught during the plan's own self-review, not
  at execution. Resolved in the plan by adding a runtime
  `indexByteSeparation()` accessor, which is the `writing-plans` skill's
  prescribed way out of an unobservable distinction: extend the interface in its
  own checkpoint rather than assert against a surface that can't see the state.

## Test Coverage

- **Covered:** nothing new — no production code was written this session. The
  existing suite was run in full (plain config) purely as a no-blockers check
  before planning: 26/26.
- **Not covered yet:** everything P5 adds. The plan allocates its own coverage,
  including two TSan-specific gates that don't exist yet — a `SpscRing`
  acquire/release verification (Task 4 CP3) and, separately, proof that the TSan
  gate *has teeth* via a deliberately relaxed-ordering fault binary (Task 5),
  following the `tsan_detects_race` pattern already at `CMakeLists.txt:90`.
  Without the latter, "our lock-free ring is TSan-clean" is an untested claim
  about the test.

## Open Questions / Blockers

None blocking. Two things the next session will decide by measuring rather than
arguing:

- **Whether `recvmmsg` earns adoption.** Built as one variant with an explicit
  promotion gate (Task 10, gated on Task 11's throughput group). Egress stays
  `sendto`-per-packet regardless, per the design doc's standing instruction.
- **Where the jitter cliff actually sits.** The design doc's "concurrent players
  before jitter exceeds budget" row likely has *no answer* within
  `sim::kMaxPlayers` — 32 players at 60 Hz is microseconds against a 16'667 µs
  budget. The plan measures to 32 anyway and locates the real cliff with a
  synthetic per-tick load knob, and Task 11 CP2 *requires* saying so plainly.
  Raising `kMaxPlayers` to manufacture a cliff was considered and rejected: it
  reopens a format frozen at P2 for a third time, breaks the 1200 B
  no-fragmentation rule, and contradicts P4's own `static_assert`.

One thing worth stating in advance, since it shapes how P5's result should be
read: **the expected outcome is that lock-free does not beat a mutex at
Tickwire's real load** (20 Hz × 32 players ≈ 640 packets/sec, three orders of
magnitude below where contention bites). That null result is the intended
finding, and the plan's Global Constraints forbid re-scoping the benchmark to
manufacture a win.

## Relevant Commits

- `4736cfa` — `chore:` import the `benchmark-optimization-loop` skill for P5
- `4c3c7ed` — `docs:` correct three name-derived rows in the skill import map
  (also the §2 stale status block and the P5 project-history finding)
- `52a15c2` — `docs:` add the P5 threading and queue benchmark plan

## Spec Changes

No change to `docs/wire-format.md` (still version 2), the design doc, or the
architecture-resolution doc.

`docs/dev-workflow-guide.md`: §2's status block said *"`/impl-plan` has NOT been
run for Tickwire"* — written pre-P0 and stale ever since, given
`docs/specs/2026-09-04-architecture-resolution.md` exists and `CLAUDE.md` names
it authoritative. Left alone it would have told a cold P5 planning session to
re-run the architectural layer, which §2 itself warns against. Rewritten to
point at the resolution doc's § Q1–Q5 and to say a phase starts at
`writing-plans`. §3a's three wrong rows corrected with a note on the hazard
(a map built from names recommends tools that don't do what their names imply,
and the cost lands at the phase depending on them). §3's "installed now" list
extended.

`docs/project-history.md`: new `## P5` section holding the import-map finding in
full, plus the note on the renamed/stale guide block. Ends with a
`<!-- Next entries: P5 execution … -->` marker.
