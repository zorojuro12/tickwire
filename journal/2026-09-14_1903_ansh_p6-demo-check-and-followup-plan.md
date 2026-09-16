# 2026-09-14 — ansh — P6 human demo check, port bug, demo follow-up plan

**Status:** The P6 visual check was run by a human. It is **not yet recorded
as verified**: the mechanics work, but on vs off was not distinguishable by
eye. The check turned up a real port bug and explained why the demo can't
show the difference. A follow-up plan is written and committed (`a2aa788`) on
`dev`, not yet executed. `dev` was pushed to `origin` at the start of the
session. The plan and journal commits since then are unpushed.
**Decided:** Fix the demo rather than record a partial result: a 4× follow-cam
zoom (the fix that matters), a `--sweep-ticks 120` bot, a 3-line HUD, and the
`htons` port fix. See `docs/plans/2026-09-14-p6-demo-followup.md` § "Why this
plan exists".
**Spec:** No change. The plan's Task 5 will update `README.md`,
`docs/project-history.md`, and `CLAUDE.md`.
**Next:** User reviews the plan. Then execute it with `executing-plans` on a new
`p6-demo-followup` branch cut from `dev`.
**Blocked on:** Nothing. Waiting on plan review.
**Touches:** `docs/plans/2026-09-14-p6-demo-followup.md` (new),
`apps/tw_server.cpp:139` (bug site), `apps/tw_client.cpp`,
`apps/tw_loadclient.cpp`, `src/client/view.{h,cpp}`, `README.md` (broken recipe),
`build/probe/lagcomp_probe.cpp` (gitignored throwaway, becomes `tools/lagcomp_probe.cpp`
in the plan's Task 4)

---

## What We Worked On

Closing P6's "Outstanding for a human" item: watch the lag-comp demo and record
the result in `docs/project-history.md`, as P2–P4 did. It turned into a bug
hunt and a demo-design diagnosis instead.

## What Worked

- `git push origin dev` sent 60 commits (P5 + P6) and moved `origin/dev` from
  `ad23530` to `96c3f6e`.
- Workaround recipe: start `tw_server --port 0`, read `port=` from its output,
  then point `tw_loadclient`/`tw_client` at it. Headless, the bot joined
  (`joined=1`). With the window, the human saw `players=2`, hits registering,
  the `L` toggle, and the ring. The server's final line was
  `lagcomp rewound_shots=328 rewinds_rejected=0`.
- The toggle path was verified correct by reading the code. `Client` stamps
  `view_tick = 0` when off (`client.h:223`), and `Server` rewinds and counts
  only when `view_tick != 0` (`server.h:127`). 328 is just about 65 s of
  compensated firing, including runs at lower latency.
- Maximizing the `tw_client` window did **not** freeze it. The P4 freeze was
  from a manual resize.
- A diagnostic lockstep harness (`build/probe/lagcomp_probe.cpp`, the real
  `Client`/`Server` over `SimulatedTransport<UdpTransport>`) reproduced
  `lagcomp_hitrate_test` (1.000 vs 0.100). It then showed the demo's problem:

  | topology | reverse every | axis | aim σ | on | off |
  |---|---|---|---|---|---|
  | demo | 30 | x | 0 | 1.000 | 0.400 |
  | demo | 30 | x | 0.5 | 0.687 | 0.193 |
  | demo | 30 | x | 1.0 | 0.307 | 0.233 |
  | demo | 120 | x | 1.0 | 0.293 | 0.260 |
  | demo | 30 | x | 0.25 | 0.973 | 0.193 |
  | demo | 120 | x | 0.25 | 0.980 | 0.060 |

  At 8 px per world unit, the hit radius is 4 px. About one dot-width of human
  aim error erases the gap. 4× zoom turns the same 8 px hand error into σ ≈
  0.25 units.

## What Didn't Work

- **The README's P6 lag-comp recipe (`--port 41234`)** failed because
  `apps/tw_server.cpp:139` passes the host-order port to
  `UdpTransport::bind(addr_be, port_be)` without `htons`. The server binds 4769
  (and prints `port=4769`), while clients send to 41234, so nobody joins.
  Present since `3b166fd` (P2, 2026-09-08). Hidden because every script and
  test uses `--port 0`, and 0 byte-swapped is still 0.
- **Judging on vs off by eye at 200 ms with the current demo:** "very minimal,
  not very clear." Cause: the tiny target relative to human aim error, plus the
  bot's 30-tick reversal inflating "off" hits. Not a lag-comp bug.
- **Lowering latency with `[` to compare** was also hard to read, for the same
  reason.
- **A slower bot alone** (`reverse_ticks` 120 or 240) does not fix it at
  σ = 1.0 (0.293 vs 0.260, and 0.287 vs 0.313). Don't propose sweep tuning
  without zoom.
- **The HUD's single line** overflows 800 px, so `lagcomp=`/`hits=` are
  clipped. The human had to maximize the window.

## Test Coverage
- **Covered:** nothing new was committed this session. The harness numbers
  are from a gitignored file.
- **Not covered yet:** no test exercises a fixed `--port` (the plan's Task 1
  adds `server_app_fixed_port`). Nothing makes the harness numbers regenerable
  yet (the plan's Task 4 adds `tools/lagcomp_probe`).

## Open Questions / Blockers
- The `phase-5-threading-queue-benchmark` and `phase-6-lag-compensation`
  branches are still local-only. Their commits are on `origin` via `dev`.
- Every app's `--port` parse (`static_cast<uint16_t>(std::atoi(...))`)
  silently wraps values over 65535. The plan records this as LOW in its
  security review rather than fixing it.
- `docs/project-history.md:1447` still holds the P6 placeholder comment. The
  plan's Task 5 replaces it.

## Relevant Commits
- `a2aa788` — docs: plan the P6 demo follow-up
