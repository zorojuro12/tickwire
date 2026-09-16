# 2026-09-11 — ansh — P4 execution: interpolation, snapshot delta

**Status:** P4 is complete — all 11 tasks, every checkpoint, on
`phase-4-interpolation-snapshot-delta` (43 commits ahead of `dev`). Verified
green across plain/ASan/TSan plus the toolchain assertions (`scripts/ci.sh`)
and the GUI build's `client_selftest`. The interactive interpolation toggle
was confirmed by a human running the real demo. Not yet merged.
**Decided:** No new architectural decisions this session — the plan already
settled all six up front (previous session). The one execution-time judgment
call: Task 6 Checkpoint 1's own prescribed verification (`ctest -R
client_test`) transiently failed on an unrelated pre-existing integration
test, because sending a real `ack_tick` unlocks server-side delta broadcasts
the client can't decode until the very next checkpoint. Verified that
checkpoint's own new test in isolation instead, documented the deviation in
the commit, and closed the gap with the next commit — recorded in full in
`docs/project-history.md`'s "What execution discovered" subsection.
**Spec:** Updated — `docs/wire-format.md` gained the `SnapshotDelta` payload
section, the `ack_tick` row's new client→server meaning, the worst-case
player-ceiling table, and a P4 version-history entry (no version bump,
`kProtocolVersion` stays 2).
**Next:** User asked to journal, commit, push, and merge to `dev` (keeping
the branch, not deleting it) — in progress via `finishing-a-development-branch`.
**Blocked on:** Nothing.
**Touches:** `src/net/snapshot_ring.*`, `src/net/snapshot_delta.*`,
`src/client/interpolation.*`, `src/server/server.h`, `src/server/session.*`,
`src/client/client.h`, `apps/tw_client.cpp`, `apps/tw_server.cpp`,
`apps/tw_loadclient.cpp`, `docs/wire-format.md`, `docs/project-history.md`,
`CLAUDE.md`, `README.md`

---

## What We Worked On

Executed the full P4 phase plan (`docs/plans/2026-09-11-phase-4-interpolation-snapshot-delta.md`)
task-by-task via the `executing-plans` skill, inline (no delegation — the
plan didn't mark any task for it). Two independent halves that share one new
data structure: `net::SnapshotRing` (last 16 snapshots, found by tick) backs
both the server's per-session delta baseline lookup and the client's
interpolation buffer. Snapshot delta (`MsgType::kSnapshotDelta = 6`) omits
unchanged players entirely; entity interpolation (`client::Interpolator`)
smooths remote players between the server's 20 Hz broadcasts.

Tasks 1–6 (the wire half): `SnapshotRing`, the delta codec with full
decoder-strictness coverage and a golden byte vector, server-side per-session
broadcasting with byte-savings counters, and client-side reconstruction that
self-heals under packet loss (a dropped delta just leaves the client
acknowledging the older baseline). Tasks 7–9 (the render half):
`Interpolator`'s render timeline and bracketing lerp, wiring into
`Client<T>`, and the GUI's `I` toggle with red/orange remote players. Task 10
was the mandatory security review (network-surface phases require it, no
exceptions) — two agents in parallel, five real findings, all fixed. Task 11
was documentation and final verification.

## Decisions Made

No new ones — see the P4 plan's own "Decisions this plan makes" section
(previous session) and `docs/project-history.md`'s P4 section, which now
records all six with their reasoning in full.

## What Worked

- **Every TDD checkpoint followed the RED→GREEN→commit discipline** using
  the corrected command form (`cmake --build && ctest -R`, never bare
  `ctest`) the P4 plan's own Global Constraints fixed after last session's
  checkpoint-template defect. 38 planned checkpoints plus 5 security-review
  fix commits, each its own commit.
- **The security review caught a real, independently-corroborated bug**: both
  the `security-reviewer` and `cpp-reviewer` agents, run in parallel with no
  shared context, separately flagged the exact same latent out-of-bounds
  read in `decodeSnapshotDelta` (dormant only because `sim::kMaxPlayers`
  happens to equal 32). Two independent agents converging on the same finding
  is a much stronger signal than either alone.
- **The measured byte-savings run confirmed the phase's own claim**: even in
  a worst-realistic-case scenario (8 players, every one moving every tick via
  `tw_loadclient`'s default pattern), delta encoding was still 11.9% smaller
  than full snapshots — `snapshot_bytes=140992 full_equiv_bytes=160000
  deltas=792 keyframes=8` over a 300-tick run.
- **The human interpolation-toggle verification matched the design claim
  exactly**: red (interpolating, smooth glide) vs orange (not, visible 20 Hz
  stepping), confirmed by direct comparison of the same remote player under
  both states.

## What Didn't Work

- **A WSLg-forwarded `tw_client` window froze when the user tried to resize
  it, and neither Ctrl+C nor Windows Task Manager could stop the process
  after closing the terminal.** The process runs inside the `tickwire-dev`
  container's own PID namespace under WSL2 — invisible to Task Manager,
  unreachable by a signal sent to a terminal that no longer exists. Fixed
  from any WSL shell: `docker ps -a` found the orphaned container
  immediately (still `Up`), and `docker stop <name>` reached it directly
  (it was run with `--rm`, so it also self-removed). Recorded in
  `docs/project-history.md`'s P4 section since P2/P3's GUI verification
  sessions never hit this — their smoke runs never resized the window
  mid-session.
- **Writing all of a task's checkpoint tests into one file at once, before
  implementing anything.** Caught myself doing this once, early (Task 1's
  `SnapshotRing` test file), which would have skipped genuine per-checkpoint
  RED signals. Reverted to strictly incremental: one checkpoint's test
  written, run RED, implemented, run GREEN, committed, then the next
  checkpoint's test added.

## Test Coverage

- **Covered:** every new module (`SnapshotRing`, the delta codec,
  `Interpolator`) has its own dedicated test file with checkpoint-by-
  checkpoint coverage including a golden byte vector, decoder-strictness
  cases, and a 20,000-trial fuzz sweep. Server and client integration is
  covered by extended `server_test.cpp`/`client_test.cpp` cases plus a new
  `convergence_test.cpp` case for a delta-only stream. The five security-
  review fixes each have their own regression test except the one structural
  finding (`kMaxPlayers == 32` static_assert) — not runtime-testable, since
  there's no invalid mask bit to construct while that constant holds.
- **Not covered:** the GUI's actual rendering — same limitation every prior
  phase recorded; closed instead by the human verification above.

## Open Questions / Blockers

None. The branch is complete and verified; the only remaining action is the
merge, which the user has now requested (see Next above).

## Relevant Commits

43 commits on `phase-4-interpolation-snapshot-delta`, from `6eb4c5b` (Task 1
Checkpoint 1) through `d7db45a` (Task 11 Checkpoint 2, docs). Full list via
`git log --oneline dev..phase-4-interpolation-snapshot-delta`. Notable ones:
- `a447349` — the `robustness_test.cpp` fixed-seed fuzz sweep broke again
  (same class of finding as P1/P2) when `kMaxMsgType` widened 5→6; re-seeded.
- `2323324` / `b419799` — the Task 6 cross-checkpoint coupling and its fix,
  documented above.
- `6478c35` / `9fab5f9` / `d037982` / `037120c` — the four security-review
  fixes with regression tests.
- `f98d747` — the full security review write-up in `docs/project-history.md`.

## Spec Changes

`docs/wire-format.md`: new `SnapshotDelta` payload section (offset table,
mask invariants, the "absent from baseline → always sent in full" rule); the
`ack_tick` header row amended for its new client→server meaning; "Maximum
packet size" extended with the delta's own arithmetic and the 48→58
worst-case player-ceiling table; decoder-strictness list extended with the
delta's rejection cases; version history entry for P4 (no bump,
`kProtocolVersion` stays 2).
