# Tickwire

An authoritative multiplayer game server in C++20: raw UDP, a fixed-tick
deterministic simulation core, client-side prediction and server
reconciliation (P3), and — coming next — lag compensation. The deliverable
is a measured numbers table — tick jitter percentiles, concurrent players
before jitter exceeds budget, queue handoff latency, packet throughput —
plus a working local demo, not just a running server.

## Build and test

```bash
scripts/tw bash scripts/ci.sh
```

Builds and tests three configurations (plain, ASan/UBSan, TSan) plus the
toolchain assertions, inside the pinned `tickwire-dev` container — this is
also what CI runs. Never invoke the host `g++`/`cmake`/`ctest` directly; see
`CLAUDE.md` for why.

## Run the demo

The demo client is a separate build configuration (`-DTW_BUILD_GUI=ON`),
skipped by `ci.sh` since it needs raylib and a display:

```bash
scripts/tw cmake -S . -B build/gui -DCMAKE_BUILD_TYPE=RelWithDebInfo -DTW_BUILD_GUI=ON
scripts/tw cmake --build build/gui -j8
scripts/tw bash scripts/demo.sh build/gui/tw_server build/gui/tw_client build/gui/tw_client
```

This starts one server and two clients in one container invocation, sharing a
network namespace. **Display caveat:** the client renders through raylib,
which needs a real X11 display. On WSL2 with WSLg (`DISPLAY` set,
`/tmp/.X11-unix` present), `scripts/tw` passes both through automatically and
a real window opens on the host desktop. Without a display, `tw_client
--selftest` exits `77` (skipped, not failed) rather than crashing —
`docs/project-history.md`'s P2 section has the details of what was verified
and how.

In the demo window: **WASD** moves, the **mouse** aims, **left click** fires,
**`[`**/**`]`** adjust the client's simulated inbound latency by 25ms a step
(0–500ms), and **`P`** toggles client-side prediction. Drag latency up to
~200ms with prediction **off** — movement visibly lurches behind the keys.
Toggle prediction **on** at the same latency — it should feel instantly
responsive again. The local player's circle changes color (green while
predicting, yellow while not) so the toggle's state is visible without
reading the HUD, which also shows `rtt`/`lead`/`err_p99` for whichever mode
is active. Closing the window leaves cleanly.

Headless, measured (not eyeballed) confirmation of the same claim: at a
simulated 200ms round trip, the predicting client's on-screen position
tracks the true (zero-latency) trajectory to within **0.000015 units** —
effectively exact — while an identical client with prediction disabled lags
the true trajectory by **~2.27 units** (roughly 17 ticks' worth of
movement, clearly visible on screen). See
`tests/client/convergence_test.cpp` and `docs/project-history.md`'s P3
section for the full numbers and how they were measured.

## What works today (through P3)

- A deterministic simulation core (`sim::World`): bounded player roster,
  fixed-timestep movement with arena clamping, hitscan resolution.
- A frozen, version-2 wire protocol (`docs/wire-format.md`) with strict,
  non-normalizing decoders — a malformed or adversarial packet is rejected,
  never silently repaired. No layout change since P2; P3 built entirely on
  the header fields P1 reserved and P2 populated.
- A single-threaded authoritative server: session binding (every input is
  authorized against the UDP endpoint it arrived from, not just the id it
  claims), a per-player tick-matched input buffer (one input consumed per
  player per tick, at the tick it was stamped for — never on arrival), a
  60 Hz simulation tick over an epoll/timerfd loop, 20 Hz snapshot
  broadcast, join/leave/timeout handling.
- A client that predicts its own local player's motion immediately on
  input, reconciles against every authoritative snapshot by replaying
  whatever the server hasn't yet consumed, and runs a clock-sync controller
  that keeps its stamped input ticks landing inside the server's
  acceptance window under real network latency and jitter — verified with
  a real, delayed `UdpTransport`, not just in-memory. See
  `docs/project-history.md`'s P3 section for the convergence numbers and
  the control-loop instability that had to be damped to get there.
- A raylib demo renderer with the prediction toggle described above, and a
  headless load client and real-UDP end-to-end test that report per-client
  RTT, clock lead, and prediction-error percentiles — reused as the seed
  for P5's throughput/latency benchmark.
- **Remote players still render exactly what the server's last snapshot
  says, with no smoothing** — prediction is local-player-only by design.
  Between-snapshot jitter on other players is real and visible; that's
  deliberate, and it's what P4 exists to fix.

## What's next (P4–P6)

- **P4** — entity interpolation and snapshot delta compression, to smooth
  the 60 Hz sim / 20 Hz snapshot cadence mismatch for *remote* players (the
  mismatch P2 deliberately left visible, and P3 deliberately left
  unaddressed beyond the local player).
- **P5** — the lock-free `SpscRing` (P2's `PacketRing` prefigures its API),
  a multi-threaded receiver, `recvmmsg`/`sendmmsg` batching, and the
  project's actual numbers table.
- **P6** — lag compensation (rewinding the world for a shot) and
  client-visible hit feedback.

No number in this README, or anywhere in this project, is claimed until it's
been measured — see the design doc's own rejection of unverified performance
claims.
