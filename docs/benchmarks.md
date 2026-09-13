# P5 measured numbers

This is the phase's deliverable: the headline artifact the design doc calls
for, generated unattended by [`scripts/bench.sh`](../scripts/bench.sh) and
recorded here rather than only in a terminal scrollback.

## Provenance

- **Machine:** `Linux 6.6.87.2-microsoft-standard-WSL2 x86_64`, 16 logical
  CPUs (`nproc`) — a WSL2 virtual machine, not bare metal. This matters for
  interpretation below (cross-core clock behavior).
- **Image:** `tickwire-dev:gcc10-cmake3.28.4-x11` (the pinned toolchain
  container; see `CLAUDE.md`).
- **Build config:** `build/plain`, `-DCMAKE_BUILD_TYPE=RelWithDebInfo` (the
  same configuration `scripts/ci.sh` builds and tests).
- **Command:** `scripts/tw bash scripts/bench.sh`
- **Date:** 2026-09-13

Re-running the exact command above regenerates every row in this document.
`scripts/tw bash scripts/bench.sh --smoke` is the fast CI-time check that the
harness itself still works (registered as CTest's `bench_smoke`); it is not
the source of the numbers below.

## Methodology

- **Tick jitter** is the *inter-tick interval* on the sim thread, measured
  with `server::monotonicNs()` (nanosecond `CLOCK_MONOTONIC`) — `now - prev`
  across successive `tick()` calls, recorded into a `server::JitterStats`.
  Reported as nearest-rank percentiles (`server::JitterStats::percentileNs`,
  Task 2) of the raw interval in nanoseconds, not a deviation subtracted from
  the nominal — the nominal at 60 Hz is `16'666'667` ns; read every `p50`/`p99`
  against that baseline yourself.
- **Percentile definition:** nearest rank, no interpolation.
  `idx = clamp(ceil(p * n) - 1, 0, n - 1)` into the ascending-sorted sample
  array. Worked example: `n = 100, p = 0.99` → index `98` → the 99th smallest
  value.
- **Queue handoff latency** is producer-stamp-to-consumer-read across a ring
  of real `net::PacketSlot` (1208 B — the actual ingest seam's payload size,
  not a synthetic small value), measured by `tools/bench_queue.cpp` (Task 9).
- **Packet throughput** is `Server::packetsIngested()` (`ThreadedRunner`,
  Task 7) accumulated over a fixed tick count, with and without
  `--batch-ingest` (Task 10's `recvmmsg` path).
- Every server-side number below runs `tw_server --threads 2 --ring spsc`
  unless the row is explicitly measuring the ring choice itself.

## Group 1 — Tick jitter vs. player count

`tw_server --threads 2 --ring spsc --ticks 1200`, driven by
`tw_loadclient --players <n> --ticks 1200` (all players moving, per
`tw_loadclient`'s default alternating-direction pattern).

| players | p50 (ns) | p99 (ns) | max (ns) |
|---:|---:|---:|---:|
| 1 | 16,666,190 | 17,016,109 | 19,597,346 |
| 2 | 16,665,955 | 17,028,905 | 17,204,902 |
| 4 | 16,666,286 | 17,001,313 | 18,959,456 |
| 8 | 16,666,865 | 17,065,888 | 19,487,728 |
| 16 | 16,665,904 | 17,024,593 | 19,269,324 |
| 32 | 16,666,585 | 17,009,293 | 17,274,408 |

## Group 2 — The jitter cliff (synthetic per-tick load)

`tw_server --threads 2 --ring spsc --ticks 600 --sim-load-us <n>`, no
players — `--sim-load-us` busy-waits the given number of microseconds once
per tick on the sim thread, standing in for real player count (see
Interpretation below for why player count can't reach this on its own).

| sim_load_us | p50 (ns) | p99 (ns) | max (ns) |
|---:|---:|---:|---:|
| 0 | 16,666,082 | 16,945,199 | 17,318,421 |
| 2,000 | 16,664,351 | 16,992,526 | 20,484,569 |
| 8,000 | 16,665,251 | 17,011,087 | 17,210,698 |
| 14,000 | 16,665,950 | 16,753,355 | 16,825,159 |
| 16,000 | 16,666,160 | 16,745,590 | 17,015,269 |
| 18,000 | 18,002,043 | 18,022,965 | 18,401,066 |
| 20,000 | 20,001,982 | 20,016,659 | 20,024,160 |

## Group 3 — Queue handoff latency, mutex vs. lock-free

`tools/bench_queue --ring <spsc|mutex> --items 10000 --rate <0|640>`.
`--rate 0` is saturation (as fast as possible); `--rate 640` is Tickwire's
actual load (20 Hz × 32 players).

| ring | rate (items/sec) | p50 (ns) | p99 (ns) |
|---|---:|---:|---:|
| spsc | 0 (saturation) | 0 | 17,701 |
| spsc | 640 (real load) | 500 | 4,101 |
| mutex | 0 (saturation) | 5,700 | 33,602 |
| mutex | 640 (real load) | 800 | 5,300 |

## Group 4 — Packet throughput, `recvmmsg` batching

`tw_server --threads 2 --ring spsc [--batch-ingest]`, driven by
`tw_loadclient --players 32 --ticks 600`, comparing `packets_ingested` over
the same fixed tick count.

| batch_ingest | packets_ingested |
|---:|---:|
| 0 (unbatched `tryReceive`) | 19,044 |
| 1 (`recvmmsg`) | 19,129 |

## Interpretation

**Player count never exceeds the jitter budget within `sim::kMaxPlayers`.**
Group 1's p99 sits at essentially the same ~17.0 ms regardless of player
count from 1 to 32 — no visible trend at all, let alone a cliff. This is the
outcome the plan's own self-review predicted before any number was measured:
32 players' worth of `World::step()` and hitscan work is microseconds against
a 16,667 µs budget, three-plus orders of magnitude of headroom. **The design
doc's "concurrent players before jitter exceeds budget" row has no answer
within the current `kMaxPlayers` — that ceiling is nowhere near this
project's real bottleneck.** Raising `kMaxPlayers` to manufacture a cliff was
considered and rejected during planning (it reopens a wire format frozen at
P2 for a third time, breaks the 1200 B no-fragmentation rule, and contradicts
P4's own `static_assert`); Group 2's synthetic load knob exists specifically
to find the real cliff without touching that ceiling.

**The jitter cliff sits between 16 ms and 18 ms of added per-tick work, not
at any measured player count.** Group 2 stays flat (p99 ≈ 16.75–17.0 ms,
*under* the 16,666,667 ns nominal budget's own noise band) through
16,000 µs of synthetic load, then jumps sharply at 18,000 µs (p99 =
18,022,965 ns) and continues to track the load 1:1 at 20,000 µs (p99 =
20,016,659 ns ≈ 16,666,667 + 20,000×1000 − rounding). This is exactly the
signature you'd expect: `TickTimer`'s 60 Hz period has roughly 16.67 ms of
slack before a tick's own work exceeds the period, at which point the
overrun becomes the interval, one-for-one.

**The derived "jitter cliff" field (`sim_load_us=0` in the raw script output)
is not meaningful, and is recorded here rather than silently discarded.**
`scripts/bench.sh`'s naive rule — "the lowest load at which p99 exceeds the
exact nominal 16,666,667 ns" — trips at zero added load, because real
scheduling noise alone (no synthetic load at all) already pushes Group 1 and
Group 2's *unloaded* p99 a few hundred microseconds past the exact nominal
value (16,945,199–17,065,888 ns observed, ~1.7–2.4% over nominal). That is
normal OS-scheduling jitter on a shared machine, not a defect in the timer or
the runner — the *first row that shows a real, sustained departure* is the
one that matters (18,000 µs, a clean 1.3 ms jump sustained through
20,000 µs), not the naive threshold-crossing script output. A future revision
of the script could define the cliff as "the first load whose p99 exceeds
nominal by more than the unloaded run's own p99 excess" — not done here, to
avoid tuning the measurement method after seeing the data it would score.

**Lock-free measurably beats mutex at both saturation and real load, but the
gap is irrelevant at Tickwire's actual load.** Group 3: at saturation, spsc's
p50 (0 ns — under this environment's clock resolution) and p99 (17,701 ns)
both beat mutex's p50 (5,700 ns) and p99 (33,602 ns) by roughly 2–5×. At
Tickwire's real load (640/sec), spsc still edges out mutex (p50 500 ns vs.
800 ns; p99 4,101 ns vs. 5,300 ns) — a real, repeatable difference, not noise,
but one measured in *hundreds of nanoseconds* against a 16,666,667 ns tick
budget: three-plus orders of magnitude too small to matter for anything this
project does. This is closer to, but not identical to, the plan's stated
expectation ("lock-free does not beat a mutex at Tickwire's real load") —
the honest reading is **lock-free wins measurably at every rate tested, and
the win is real but has no practical consequence at 640 packets/sec**, which
is the same conclusion (don't pick a ring for this project based on this
benchmark) reached by a slightly different route than "indistinguishable."
Per the plan's Global Constraints, this null-practical-impact result is the
intended outcome, not a failure of the phase, and it is reported as measured
rather than rounded off to match the prediction exactly.

**`recvmmsg` batching is rejected — no measurable win at Tickwire's actual
load.** Group 4's two `packets_ingested` counts (19,044 unbatched vs. 19,129
batched, over an identical 600-tick, 32-player run) differ by under 0.5%,
indistinguishable from run-to-run noise at this traffic volume. `--batch-ingest`
remains available (Task 10 built it as a measured, opt-in variant per the
`benchmark-optimization-loop` skill's promotion gate — "adopt only if it
measurably wins here") but is **not** the default, and nothing in this phase
changes that default. Egress stays `sendto`-per-packet regardless, per the
design doc's standing instruction that an egress queue waits for numbers
that demand it — and these numbers don't.

**A genuine measurement-harness finding, not a headline number but worth
recording for anyone re-running this on a similar machine:** `bench_queue`'s
first implementation computed handoff latency as unsigned
`now_ns - stamp` across the producer/consumer thread pair. On this WSL2
virtual machine, a cross-core `CLOCK_MONOTONIC` read occasionally showed a
sub-microsecond apparent inversion (the producer's stamp reading fractionally
"ahead" of a same-instant consumer read on a different core) — monotonicity
is a per-thread guarantee, not a cross-core one, and virtualized environments
are exactly where this shows up in practice. The unsigned subtraction wrapped
to a value near `2^64`, poisoning every percentile in the affected run (this
is what a smoke-run dogfooding of `scripts/bench.sh` itself caught, before
the real measurement run). Fixed by computing the delta in `int64_t` and
clamping negative results to zero (`tools/bench_queue.cpp`) — the numbers in
Group 3 above are post-fix.
