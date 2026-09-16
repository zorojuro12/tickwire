# 2026-09-15 — ansh — P6 demo follow-up: port fix, zoom, lagcomp_probe, human verification

**Status:** All 4 implementation tasks of `docs/plans/2026-09-14-p6-demo-followup.md`
executed on `p6-demo-followup` (cut from `dev`) via the `executing-plans` skill,
inline, TDD checkpoint by checkpoint — 6 commits, every task boundary green
across plain/ASan/TSan. Task 5's security review (security-reviewer agent)
found **no CRITICAL or HIGH findings**. Task 5's human verification is
**done and positive**: on vs. off is now clearly distinguishable by eye.
`docs/project-history.md` and `CLAUDE.md` are updated to record all of it.
Not yet merged to `dev` — final `ci.sh` re-verification, the docs commit, and
`finishing-a-development-branch` are next.
**Decided:** Nothing new beyond what the plan already locked in (4× follow-cam
zoom as the fix that mattered, `--sweep-ticks 120` for the README recipe,
`htons` at the one bind call) — see `docs/project-history.md`'s new "P6 demo
follow-up" section for the full Finding writeups and the security review.
**Spec:** Updated — `docs/wire-format.md` untouched (no protocol change this
phase); `CLAUDE.md` (`src/client/` row gains `Camera`/`screenToWorld`,
`tools/` row gains `lagcomp_probe`); `docs/project-history.md` (new "P6 demo
follow-up" section: four Findings, the human-verified interactive-feel
writeup, and the security review, replacing the stale placeholder comment
left over from P6 itself); `README.md` already fixed and committed
(`886f230`) in an earlier part of this session, before the journal-writing step.
**Next:** Run the plan's Task 5 Checkpoint 4 Step 2 full verification
(`ci.sh` plus a `client_selftest` re-check on `build/gui`), commit the docs,
then hand off to `finishing-a-development-branch` to merge into `dev`
(`--no-ff`, self-merge, no PR — per `CLAUDE.md`).
**Blocked on:** Nothing.
**Touches:** `apps/tw_server.cpp`, `apps/tw_loadclient.cpp`, `apps/tw_client.cpp`,
`src/client/view.{h,cpp}`, `tests/client/view_test.cpp`, `tools/lagcomp_probe.cpp`
(new), `scripts/e2e-udp.sh`, `CMakeLists.txt`, `README.md`,
`docs/project-history.md`, `CLAUDE.md`.

---

## What We Worked On

Closed the last item the 2026-09-14 planning session left open: the P6
lag-compensation demo didn't actually work as written (a port byte-order bug
meant the README recipe never connected) and, once worked around, a human
couldn't clearly tell lag compensation on from off by eye. The follow-up plan
diagnosed both — a real bug plus a genuine demo-readability problem (hit
radius vs. human aim error at the original 8px/world-unit zoom) — and this
session executed the fix.

## Decisions Made

All decisions were already locked in by the 2026-09-14 plan; execution surfaced
no new ones. See `docs/plans/2026-09-14-p6-demo-followup.md` §"Why this plan
exists" for the original reasoning (zoom as the fix that matters, not bot
speed alone) and `docs/project-history.md`'s new P6 demo follow-up section for
the confirmed-by-execution version of the same reasoning, now with the
regenerated `lagcomp_probe` matrix inline.

## What Worked

- **Every RED step reproduced exactly what the plan predicted**, same as P6
  itself: Task 1's `port=5537` byte-swap, Task 2's missing-flag usage text and
  `e2e_udp` failure, Task 3's compile error (`Camera` not a member of
  `client`), Task 4's CMake configure error on the missing source file. No
  checkpoint needed a second implementation pass.
- **`tools/lagcomp_probe`'s matrix reproduced the pinned-image numbers from
  planning bit-for-bit across two separate runs** — fully deterministic
  lockstep harness, no flakiness. Rows 1, 4, 7, and 9 matched the planning
  session's own findings table exactly (test 120/y/0: 1.000/0.100; demo
  30/x/0: 1.000/0.400; demo 30/x/1.0: 0.307/0.233; demo 120/x/0.25:
  0.980/0.060).
- **The security review found nothing to fix.** No CRITICAL/HIGH; two LOW/
  informational notes (an unenforced `Camera::zoom` precondition, unreachable
  today; the fixed-seed RNG in the probe, intentional for reproducibility)
  and confirmation that the pre-existing, out-of-scope `--port` `uint16_t`
  truncation is correctly LOW, not something to escalate.
- **The human verification landed a clean, unambiguous result on the first
  re-run of the fixed recipe**: 54 hits with `lagcomp=on` vs. ~6 with
  `lagcomp=off` at 200ms latency (~9×), plus an unprompted extra control at
  0ms latency/off (~16 hits) that makes physical sense (less real desync to
  compensate for, so more shots land on the live target by chance alone).
  Server's final line: `lagcomp rewound_shots=82 rewinds_rejected=0`. No
  visual artifacts (no HUD clipping, no camera jitter, ring drawn correctly).

## What Didn't Work

- **The user's first attempt to run the demo hit a dead Docker daemon**
  (`/usr/bin/docker: Input/output error`) — the same WSL2/Docker-Desktop
  integration gap `docs/project-history.md`'s P0 section already recorded:
  `/usr/bin/docker` is a symlink into `/mnt/wsl/docker-desktop/`, which only
  populates while Docker Desktop is running on the Windows host. Not a
  project or code issue; resolved by the user restarting Docker Desktop.
  Recorded here only because it's the second time this exact failure mode has
  hit a session (P0 first saw it during toolchain verification) — worth
  checking `docker info` on sight of an unexplained `scripts/tw` failure
  before assuming a repo-level problem.
- The `lagcomp_probe.cpp` first draft was 211 lines against the plan's
  200-line cap; trimmed to 196 by removing incidental blank-line separators
  between adjacent single-purpose statements, no logic change. Also hit
  `-Werror=unused-parameter` on `argv` in `main` (the tool takes no
  arguments) — fixed by leaving the parameter unnamed.

## Test Coverage

- **Covered:** `server_app_fixed_port` (port byte-order), `loadclient_sweep_ticks_zero_rejected`/
  `_negative_rejected` (flag validation), `view_test`'s two camera cases
  (zoom-1 and zoom-4, both directions including the exact inverse），
  `lagcomp_probe_smoke` (the tool builds and its first row matches). The
  `client::Camera`/`screenToWorld` follow-cam and three-line HUD in
  `tw_client` have no test surface (GUI drawing, per the P2–P6 precedent) —
  covered instead by `client_selftest` (display path only) and this session's
  human verification (actual rendered behavior).
- **Not covered yet:** nothing new; the plan's own scope didn't ask for
  anything beyond what's listed above.

## Open Questions / Blockers

None. The plan's Task 5 Checkpoint 4 Step 2 (final `ci.sh` run, `client_selftest`
re-check, docs commit) is the only remaining step before
`finishing-a-development-branch`.

## Relevant Commits

6 commits on `p6-demo-followup`, `dev..HEAD` so far:
`48055e3` fix: bind tw_server to the port it was given, not its byte swap,
`5fc9ffa` feat: make tw_loadclient's sweep period configurable,
`40bfa65` feat: add a zoomable camera to the client view mapping,
`294ba13` feat: follow the local player at 4x zoom with a three-line HUD in tw_client,
`9bffc7e` feat: add lagcomp_probe, the demo-parameter hit-rate matrix,
`886f230` docs: fix the lag compensation demo recipe.
(The docs/CLAUDE.md/journal commit for Task 5 Checkpoint 4 is still pending
as of this entry.)

## Spec Changes

- `CLAUDE.md`: `src/client/` file-structure row now names `client::Camera`,
  `worldToScreen`/`worldToScreenRadius`, and `screenToWorld`; `tools/` row
  gains `lagcomp_probe`.
- `docs/project-history.md`: new "P6 demo follow-up" section replacing the
  stale `<!-- Next entries: ... -->` placeholder — four Finding entries (the
  port byte-swap bug and its four-phase invisibility, the HUD overflow, the
  hit-radius-vs-aim-error diagnosis with the full `lagcomp_probe` matrix
  inline, and a closing "nothing else deviated" note), an "Interactive
  feel — human-verified after phase completion" section with this session's
  measured hit counts, and a "P6 demo follow-up security review" section with
  the security-reviewer agent's findings.
