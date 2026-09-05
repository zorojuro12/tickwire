# 2026-09-05 — ansh — ECC import map, project history doc, CLAUDE.md follow-through

**Status:** P0 is merged to `dev` (see the prior entry for that work). This
session's second half was documentation: mapped ECC's skill/agent/command
survey to Tickwire's remaining phases, created a standing cross-phase history
log, and closed a gap where a documented recommendation was never actually
applied to `CLAUDE.md`.
**Decided:** No architectural decisions. One process correction: recommendations
written into `docs/dev-workflow-guide.md` don't count as done until they're
applied to the artifact they're about — caught when asked directly "did we
edit `CLAUDE.md` based on your finding?" and the honest answer was no.
**Spec:** No change to `docs/specs/`. `docs/dev-workflow-guide.md` and
`CLAUDE.md` both updated; new file `docs/project-history.md` created.
**Next:** Plan P1 (wire protocol, serialization, injectable transport) via
`writing-plans` — no open architecture question blocks it; `/impl-plan`
already resolved the `Transport` interface shape and `SpscRing` contract at
P0. Not yet started — user was about to confirm before this entry was
requested.
**Blocked on:** Nothing.
**Touches:** `docs/dev-workflow-guide.md`, `docs/project-history.md`,
`CLAUDE.md`

---

## What We Worked On

Three asks, handled in sequence:

1. Cross-reference `~/projects/ecc-survey.md`'s skill/agent/command inventory
   against Tickwire's P0–P7 phase table to find what's not yet imported and
   when each gap actually needs closing.
2. Survey the CLAUDE.md example templates buried in `~/projects/ECC/examples/`
   to see what structural sections Tickwire's `CLAUDE.md` is missing and why.
3. When asked directly whether any of that led to actual `CLAUDE.md` edits —
   it hadn't, and it should have for the two sections flagged as safe to add
   immediately.

## Decisions Made

- **P5 is the clearest concrete ECC-import gap.** The `benchmark`,
  `benchmark-methodology`, and `benchmark-optimization-loop` skills aren't
  installed, and P5's entire purpose (the tick-jitter/throughput numbers
  table) is exactly what they're for. See `docs/dev-workflow-guide.md` §3a.
- **P3/P4/P6/P7 have no applicable ECC skills at all** — confirmed as a
  genuine domain gap (netcode prediction/reconciliation/lag-comp isn't
  something ECC's skill set covers), not an oversight. The design doc already
  routes those to external references instead.
- **`CLAUDE.md`'s Project Overview and Workflow table are safe to add any
  time** — unlike File Structure, Key Patterns, or Environment Variables,
  they don't depend on verified reality that doesn't exist yet. Applied in
  this session after being caught not having done so.

## What Worked

- The three-way split (journal = narrative, specs/ADRs = full rationale,
  project-history.md = skimmable cross-phase timeline) held up when actually
  populating `project-history.md` — every P0 entry had an obvious home and
  didn't need to duplicate the architecture-resolution doc's reasoning, just
  point to it.
- `CLAUDE.md`'s Task 6 regression check (`test -f CLAUDE.md && grep -q
  "scripts/tw" CLAUDE.md`) and a full `scripts/tw bash scripts/ci.sh` run both
  stayed green through both rounds of `CLAUDE.md` edits — confirms the doc
  changes didn't silently break anything load-bearing (there wasn't code risk
  here, but worth confirming the discipline held for a docs-only change too).

## What Didn't Work

- Writing a recommendation into `docs/dev-workflow-guide.md` and treating
  that as equivalent to acting on it. It isn't — the user had to ask "did we
  not need to make any edits to CLAUDE.md based on your finding?" for the gap
  to surface. Future sessions: when a survey/analysis task produces a "do X
  now, it's safe" line, do X in the same turn, don't just record that X
  should happen.

## Relevant Commits

- `884d8a1` — docs: map ECC imports to future phases and CLAUDE.md structure to reality
- `d9c5b94` — docs: add project history log for cross-phase decisions, pivots, and findings
- `364e4c8` — docs: add Project Overview and a workflow quick-reference to CLAUDE.md

## Next Step

Run `writing-plans` for P1 (wire protocol, serialization, injectable
transport) once the user confirms. Flag going in: P1 is where the network
surface opens up, so `security-review`/`security-reviewer` become mandatory
per `CLAUDE.md`'s new Workflow table, not optional — the UDP deserializer
parses untrusted bytes from an unauthenticated source.
