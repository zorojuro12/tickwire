# Tickwire

An authoritative multiplayer game server in C++20: raw UDP, a fixed-tick
deterministic simulation core, and (from P3 onward) client-side prediction,
server reconciliation, and lag compensation. The deliverable is a measured
numbers table — tick jitter percentiles, concurrent players before jitter
exceeds budget, queue handoff latency, packet throughput — plus a working
local demo, not just a running server.

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
(0–500ms) to see prediction-less netcode lag directly — closing the window
leaves cleanly.

## What works today (through P2)

- A deterministic simulation core (`sim::World`): bounded player roster,
  fixed-timestep movement with arena clamping, hitscan resolution.
- A frozen, version-2 wire protocol (`docs/wire-format.md`) with strict,
  non-normalizing decoders — a malformed or adversarial packet is rejected,
  never silently repaired.
- A single-threaded authoritative server: session binding (every input is
  authorized against the UDP endpoint it arrived from, not just the id it
  claims), a 60 Hz simulation tick over an epoll/timerfd loop, 20 Hz snapshot
  broadcast, join/leave/timeout handling.
- A client and a raylib demo renderer that draw **exactly** what the server's
  last snapshot says — **no client-side prediction or interpolation yet**.
  At nonzero simulated latency the lag is real and visible; that's
  deliberate, not a bug, and it's what P3 exists to fix.
- A headless load client and a real-UDP end-to-end test, both reused as the
  seed for P5's throughput/latency benchmark.

## What's next (P3–P6)

- **P3** — client-side prediction, server reconciliation, clock sync (the
  header already carries `send_time_ms`/`ack_tick`; P2 populates them, P3
  adds the logic that reads them back).
- **P4** — entity interpolation and snapshot delta compression, to smooth
  the 60 Hz sim / 20 Hz snapshot cadence mismatch P2 deliberately leaves
  visible.
- **P5** — the lock-free `SpscRing` (P2's `PacketRing` prefigures its API),
  a multi-threaded receiver, `recvmmsg`/`sendmmsg` batching, and the
  project's actual numbers table.
- **P6** — lag compensation (rewinding the world for a shot) and
  client-visible hit feedback.

No number in this README, or anywhere in this project, is claimed until it's
been measured — see the design doc's own rejection of unverified performance
claims.
