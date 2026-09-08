# 2026-09-08 — ansh — P2 execution: authoritative server through Task 9 Checkpoint 1

**Status:** Tasks 1–8 of `docs/plans/2026-09-06-phase-2-authoritative-server.md` fully complete on `phase-2-authoritative-server`, plus Task 9 Checkpoint 1 (pure world→screen mapping). Full `scripts/ci.sh` (plain/ASan/TSan/toolchain) green as of Task 8's boundary; `view_test` (Task 9 C1) verified green on top of that. Mandatory Task 8 security review done — two agent reviews, findings triaged and recorded. Remaining: Task 9 Checkpoints 2–3 (raylib GUI + demo, the one task with unresolved environment risk) and Task 10 (docs). At the user's explicit request, stopping here to journal, commit, push, and merge into `dev` **without deleting the branch** (phase incomplete — resuming later).
**Decided:** Three CRITICAL security findings (spoofable address-based session authorization, join/snapshot amplification, session-table exhaustion) — all one root cause: the protocol has no cryptographic session binding, which is inherent to the raw-UDP threat model the design doc already commits to — were put to the user explicitly rather than resolved unilaterally, since fixing them means reopening the frozen wire format and expanding P2's scope. User chose **defer and record**, matching the treatment P1 gave its own two deferred findings. Full reasoning in `docs/project-history.md`'s new P2 section.
**Spec:** No change to `docs/specs/`. `docs/project-history.md` gained a `## P2` section (Task 8 security review findings only so far; Task 10 will add the rest of the phase's decisions).
**Next:** Resume with Task 9 Checkpoint 2 (raylib GUI build + window-open proof) when picked back up — the pinned image is missing `libGL`/`libXrandr`/`libXi`/`libXcursor`/`libXinerama` and needs the `Dockerfile`/`scripts/tw` image-tag bump described in the plan before that checkpoint can even attempt to build.
**Blocked on:** Nothing technical. Session ending at the user's request, not a blocker.
**Touches:** `src/sim/world.*`, `src/net/{bytes,protocol,framing}.*`, `src/server/*`, `src/client/{client.h,view.*}`, `apps/*`, `tests/**`, `docs/project-history.md`, `CMakeLists.txt`

---

## What We Worked On

Executed the P2 phase plan task-by-task via the `executing-plans` skill, inline (no delegation), starting from a clean `dev` with only the plan committed. Built, in order: `sim::World` (roster/movement/arena clamping/hitscan — Tasks 1–2), the wire-format amendment to protocol version 2 plus `framePacket`/`JoinAccept` (Task 3), `PacketRing` (Task 4), `SessionTable` (Task 5), `Server<T>` (Task 6, the plan's largest task — join/authorize/broadcast/leave-timeout across 4 checkpoints), the epoll/timerfd tick loop and `tw_server` (Task 7), `Client<T>` plus the load client and a real-UDP end-to-end test (Task 8), then started Task 9 with the pure, raylib-free `view.h`/`view.cpp` world→screen mapping.

## Decisions Made

- **The security review's three CRITICAL findings — deferred, not fixed. Reasoning:** see `docs/project-history.md`'s new P2 section (linked above) for the full writeup; short version is that address-based session auth is fundamentally unfixable against a source-spoofing attacker without a session-token handshake, which would reopen the wire format and isn't scoped anywhere in the current P2–P6 roadmap.
- **Two plan-internal forward-reference resolutions**, following the same precedent P1 established (checkpoint's own RED/GREEN text wins over a higher-level summary when they conflict): `InputCommand::aim_x`/`aim_y` added to the struct at Task 1 Checkpoint 2 rather than waiting for Task 3 (only the wire *codec* is frozen-until-Task-3, not the in-memory struct); `SessionTable::authorize()` and `Client::sendInput()`'s guard implemented at their respective Checkpoint 1s rather than Checkpoint 2, since both checkpoints' own "fresh state" specs already called them and neither has a meaningful partial implementation.
- **Task 6 Checkpoint 1's join-reply `tick` field is `world_.tick() + 1`, not `world_.tick()`.** Real bug caught by the checkpoint's own test: replies are framed during the pre-step routing phase of `tick()`, so using the pre-step value made the very first join's reply report `tick=0` instead of `1`. Documented in that commit.
- **`tests/support/recording_transport.h`** — moved the recording transport stub out of `server_test.cpp` (Task 6) into a shared header (Task 8), per the plan's own note for that checkpoint, rather than duplicating it.

## What Worked

- **RED-first discipline caught four real, non-trivial bugs**, not just typos: the join-reply tick off-by-one (Task 6 C1); a client retransmit-timing off-by-one from checking the retry condition before incrementing the tick counter instead of after (Task 8 C1); a flaky `TickTimer` test where busy-polling completed faster than real time could elapse (Task 7 C1) — fixed by pacing the poll loop, not by weakening the assertion; and a `PollSet` test where an undrained socket fd's level-triggered readiness starved `epoll_wait`'s timeout, making it return instantly instead of actually blocking, so the timer-wait loop never got real wall-clock time to observe a fire (Task 7 C2) — root-caused with a standalone debug-instrumented rerun before fixing.
- **The two-agent security review (`security-reviewer` + `cpp-reviewer` in parallel) earned its keep again**, same as P1: caught a genuine HIGH-severity regression (client_test.cpp stack-allocating a ~1.24 MB test double, contradicting a convention the sibling `server_test.cpp` file already followed correctly) plus confirmed the P1-deferred finding is actually closed by tracing every call site, not just trusting the code's shape.

## What Didn't Work

- Nothing was tried and abandoned outright, but several test assertions needed correction after a genuine RED revealed the *test's* expectation was wrong, not the implementation — worth recording so a future session doesn't assume every RED means "add missing code": the arena-clamping corner-case test initially placed the player at the origin instead of one-step-short-of-the-corner (200 steps wasn't enough distance-at-speed to reach the bound from origin); the "unknown endpoint drops an input" test compared position against a stale pre-tick snapshot without accounting for the target's already-latched velocity continuing to move it that tick.

## Test Coverage

- **Covered:** Everything through Task 8 — `World`'s full roster/movement/clamping/hitscan surface (100% branch target per `CLAUDE.md`), the v2 wire codec plus `framePacket`/`JoinAccept` with golden byte vectors, `PacketRing` FIFO/bounded/wrap behavior, `SessionTable`'s full join/authorize/expire/fire-cooldown contract, `Server<T>`'s join/authorize/broadcast/leave/timeout end-to-end (including the spoof-rejection case discharging P1's deferred finding), the epoll/timerfd loop and `tw_server` under plain/ASan/TSan, `Client<T>`'s handshake/retransmit/input/snapshot-adoption logic, an in-memory client-server convergence test (including one proving the `SimulatedTransport` latency wrapper delays join completion against real server code), and a real-UDP 4-player end-to-end test. Task 9's `view.h` world→screen mapping is covered by its own pure unit tests.
- **Not covered yet:** The raylib GUI path (Task 9 C2–C3) — entirely unbuilt and unverified in this environment; the display-capability risk the plan flagged going in is still unresolved. `docs/wire-format.md`, `CLAUDE.md`, and `README.md` updates (Task 10) are not yet written.

## Open Questions / Blockers

- **The raylib display path remains the plan's one flagged environment risk**, unchanged from the planning session's assessment: the pinned image has `libX11` but is missing `libGL`/`libXrandr`/`libXi`/`libXcursor`/`libXinerama` and GL headers; WSLg was confirmed live on the host during planning but passthrough into a container is still unverified. Per the plan, if Task 9 Checkpoint 2 can't go green, the fallback is to record the exact failure and treat Tasks 1–8 as a complete, shippable phase on their own — not something to fight in-session.

## Relevant Commits

29 commits on `phase-2-authoritative-server` covering Tasks 1–8 plus Task 9 Checkpoint 1 and the security-review fixes/writeup — see `git log --oneline dev..phase-2-authoritative-server`. Highlights: `dc663d6` (`sim::World` roster), `ede363c` (protocol v2), `d33d332` (`Server` join handling, with the tick off-by-one fix), `b0a1b22` (leave/timeout, closing Task 6), `355baa3`/`b8c30a9` (timer/epoll, both with real flakiness fixes), `5a3382b` (client handshake, with the retransmit off-by-one fix), `59031a2` (in-memory + real-UDP convergence tests, closing Task 8), `e50ae05` (security/cpp review fixes), `94f17a6` (security findings recorded), `90e1339` (Task 9 C1).

## Next Step

Resume Task 9 at Checkpoint 2: bump the image tag in `scripts/tw`, add the X11/GL dev packages to the `Dockerfile`, wire up `-DTW_BUILD_GUI=ON` and the conditional X passthrough, then attempt `tw_client --selftest`. If it fails, record the exact failure and which layer it failed at (raylib's build, the X connection, or GLFW's window creation) per the plan's explicit instruction not to fight the display in-session — Tasks 1–8 already stand alone as a complete, tested phase.
