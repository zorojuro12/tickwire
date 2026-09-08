# 2026-09-08 — ansh — P2 execution: authoritative server through phase completion

**Status:** All 10 tasks of `docs/plans/2026-09-06-phase-2-authoritative-server.md` complete on `phase-2-authoritative-server`. Full `scripts/ci.sh` (plain/ASan/TSan/toolchain) green, plus the GUI build (`build/gui`, `-DTW_BUILD_GUI=ON`) — the raylib display path the plan flagged as its one environment risk works. Mandatory Task 8 security review done (two agent reviews); findings fixed, deliberately deferred with recorded reasoning, or verified closed. Docs updated (wire-format v2, `CLAUDE.md`, new `README.md`, full `docs/project-history.md` P2 section). Branch merged into `dev` (`--no-ff`) and kept, not deleted, at the user's request.
**Decided:** Three CRITICAL security findings (spoofable address-based session authorization, join/snapshot amplification, session-table exhaustion) — all one root cause: the protocol has no cryptographic session binding, inherent to the raw-UDP threat model the design doc already commits to — were put to the user explicitly rather than resolved unilaterally, since fixing them means reopening the frozen wire format and expanding P2's scope. User chose **defer and record**, matching the treatment P1 gave its own two deferred findings. Full reasoning in `docs/project-history.md`'s P2 section.
**Spec:** `docs/wire-format.md` re-frozen at version 2 (the aim-vector amendment, join/leave payload layouts, which header fields P2 actually populates vs. remain reserved for P3). `docs/project-history.md` gained the full P2 section. No change to `docs/specs/`.
**Next:** Start P3 (client-side prediction, server reconciliation, clock sync) per the design doc's phase table — no plan written yet.
**Blocked on:** Nothing.
**Touches:** `src/sim/world.*`, `src/net/{bytes,protocol,framing}.*`, `src/server/*`, `src/client/*`, `apps/*`, `tests/**`, `scripts/{tw,demo.sh,e2e-udp.sh}`, `Dockerfile`, `docs/{wire-format,project-history}.md`, `CLAUDE.md`, `README.md`, `CMakeLists.txt`

---

## What We Worked On

Executed the P2 phase plan task-by-task via the `executing-plans` skill, inline (no delegation), starting from a clean `dev` with only the plan committed, across two sessions (the first paused mid-phase at the user's request to journal/commit/push/merge without deleting the branch; this entry now covers the whole arc through completion). Built, in order: `sim::World` (roster/movement/arena clamping/hitscan — Tasks 1–2), the wire-format amendment to protocol version 2 plus `framePacket`/`JoinAccept` (Task 3), `PacketRing` (Task 4), `SessionTable` (Task 5), `Server<T>` (Task 6, the plan's largest task), the epoll/timerfd tick loop and `tw_server` (Task 7), `Client<T>` plus the load client and a real-UDP end-to-end test (Task 8), the pure `view.h` world→screen mapping, the raylib GUI client and demo script (Task 9), and the documentation pass (Task 10).

## Decisions Made

- **The security review's three CRITICAL findings — deferred, not fixed. Reasoning:** see `docs/project-history.md`'s P2 section for the full writeup; short version is that address-based session auth is fundamentally unfixable against a source-spoofing attacker without a session-token handshake, which would reopen the wire format and isn't scoped anywhere in the current P2–P6 roadmap.
- **Several plan-internal forward-reference resolutions**, following the same precedent P1 established (checkpoint's own RED/GREEN text wins over a higher-level summary when they conflict): `InputCommand::aim_x`/`aim_y` added to the struct at Task 1 Checkpoint 2 rather than waiting for Task 3; `SessionTable::authorize()` and `Client::sendInput()`'s guard implemented at their respective Checkpoint 1s rather than Checkpoint 2.
- **Task 6 Checkpoint 1's join-reply `tick` field is `world_.tick() + 1`, not `world_.tick()`.** Real bug caught by the checkpoint's own test: replies are framed during the pre-step routing phase of `tick()`, so the pre-step value made the very first join's reply report `tick=0` instead of `1`.
- **`tests/support/recording_transport.h`** — moved the recording transport stub out of `server_test.cpp` (Task 6) into a shared header (Task 8), per the plan's own note.
- **`bullseye-security` disabled entirely in the `Dockerfile`** (Task 9) rather than pinning individual package versions: the repo's apt index had drifted out of sync with its pool (a known Debian-archive gap once a package is superseded), reproducing `404`s across a `--no-cache` rebuild; pinning `curl`/`libgl1-mesa-dri` just pushed the same conflict onto their transitive deps. Every package this image needs resolves from plain `bullseye/main`, and a build-time toolchain image has no runtime exposure security patches would meaningfully cover.
- **The latency slider (Task 9 C3) reconstructs `SimulatedTransport` via `std::optional::emplace()`** rather than mutating it in place (it has no setter and holds a `SimConfig` by value): `emplace()` destroys and reconstructs at the *same* storage address, so `Client<T>`'s `T&` reference into it stays valid across a latency change without losing join/snapshot state.

## What Worked

- **RED-first discipline caught real, non-trivial bugs throughout, not just typos**: the join-reply tick off-by-one (Task 6 C1); a client retransmit-timing off-by-one from checking the retry condition before incrementing the tick counter (Task 8 C1); a flaky `TickTimer` test where busy-polling completed faster than real time could elapse (Task 7 C1); a `PollSet` test where an undrained socket fd's level-triggered readiness starved `epoll_wait`'s timeout so the timer-wait loop never got real wall-clock time to observe a fire (Task 7 C2) — root-caused with a standalone debug-instrumented rerun before fixing; and a second, near-identical `TickTimer` flake in the *same* test resurfacing under system load during Task 9's raylib build (fixed by draining the timer's construction-time head start before asserting a steady state).
- **The two-agent security review (`security-reviewer` + `cpp-reviewer` in parallel) earned its keep again**: caught a genuine HIGH-severity regression (`client_test.cpp` stack-allocating a ~1.24 MB test double, contradicting a convention `server_test.cpp` already followed correctly for the identical type) and confirmed the P1-deferred finding is actually closed by tracing every call site, not just trusting the code's shape.
- **The raylib display path just worked once the two environment defects were fixed** — a real GLX window opened through WSLg passthrough, rendered via Mesa/llvmpipe software rasterization, closed cleanly. Smoke-tested end to end (server + two GUI clients, 8s at target 60 FPS, no crash).

## What Didn't Work

- Several test assertions needed correction after a genuine RED revealed the *test's* expectation was wrong, not the implementation — worth recording so a future session doesn't assume every RED means "add missing code": the arena-clamping corner-case test initially placed the player at the origin instead of one-step-short-of-the-corner; the "unknown endpoint drops an input" test compared position against a stale pre-tick snapshot without accounting for the target's already-latched velocity continuing to move it that tick.
- **First attempt at fixing the Task 9 Docker apt failure (`Acquire::Check-Valid-Until=false`) didn't work** — that flag addresses a stale-*Release*-file symptom, but the actual failure was `404`s on specific `.deb` files (index/pool drift), a different problem that needed disabling the repo, not relaxing its freshness check.
- **Second attempt (pinning `curl`/`libgl1-mesa-dri` to the working plain-repo version) also didn't work** — it just moved the same version conflict onto their transitive dependencies (`libcurl4`, `libglapi-mesa`), which apt then also wanted at the (missing) security version. Disabling the repo outright was the fix that actually held.

## Test Coverage

- **Covered:** The full phase — `World`'s complete roster/movement/clamping/hitscan surface (100% branch target), the v2 wire codec plus `framePacket`/`JoinAccept` with golden byte vectors, `PacketRing`, `SessionTable`'s full contract, `Server<T>`'s join/authorize/broadcast/leave/timeout end-to-end, the epoll/timerfd loop and `tw_server` under plain/ASan/TSan, `Client<T>`'s handshake/input/snapshot-adoption logic, in-memory and real-UDP client-server convergence, and the pure `view.h` world→screen mapping. The GUI path's mechanical correctness (window opens, doesn't crash under an 8s run) is smoke-tested.
- **Not covered yet, and not coverable by this session:** the GUI's actual interactive/visual behavior — WASD responsiveness, mouse-aim feel, whether the latency slider's lag is visually convincing, whether a departing player's circle actually disappears within one snapshot on the *other* client's screen. The windows render through WSLg onto the host desktop; this session has no tool to capture or click into them. Needs a human to run `scripts/tw bash scripts/demo.sh ...` and watch.

## Open Questions / Blockers

- None. The GUI display risk the plan flagged going in resolved cleanly (see Decisions Made / What Worked above).

## Relevant Commits

40 commits on `phase-2-authoritative-server`, `dc663d6`..`b5661d4` — see `git log --oneline dev..phase-2-authoritative-server`. Highlights beyond the first session's (already covered in earlier revisions of this entry): `8664aeb` (raylib client builds, window-open proof, the two Dockerfile apt fixes), `ffcee2a` (full render loop, latency slider, demo script, the second `TickTimer` flake fix), `b5661d4` (Task 10 docs). Merge commit into `dev`: see `git log --oneline dev` for the `merge: phase-2-authoritative-server into dev` commits (one from the mid-phase pause, one — or an amendment to the branch pointer — from this completion).

## Next Step

P3 per the design doc's phase table (client-side prediction, server reconciliation, clock sync) — no plan exists yet; start with `/impl-plan` or the `writing-plans` skill against the design doc's P3 row. The header already carries `send_time_ms`/`ack_tick`, populated but unconsumed by P2 — P3 adds the RTT/drift logic that reads them back, not new wire layout.
