# Tickwire — Project History

A running, cross-phase log of **decisions, pivots, and findings** — the things
worth knowing without re-reading every journal entry or spec doc. This is not
a replacement for those:

- **`journal/`** — session-by-session narrative ("what happened this
  session"). Detailed, dated, one file per session.
- **`docs/specs/`** — the design and its architectural resolution. The full
  reasoning for a decision lives there or in a future `docs/decisions/`
  ADR; this doc points to it rather than restating it.
- **This file** — the skimmable timeline. One entry per load-bearing event:
  a decision made, a decision reversed (pivot), or something discovered by
  actually running the toolchain that the spec didn't predict (finding).

**What belongs here:** anything that changed the plan, closed off an
alternative, or surprised us enough that a future phase could otherwise
re-litigate or re-discover it. **What doesn't:** routine task completion
(that's what `git log` and the journal are for) or reasoning already fully
written down elsewhere (link to it instead).

Entries are grouped by phase, chronological within each phase. Append new
entries as they happen — don't backfill routine work.

---

## P0 — Toolchain & skeleton

**Decision — floats over fixed-point.** The design doc left this open with a
vague revisit trigger ("if P3 reconciliation proves unstable"). Resolved to a
concrete one instead: **the cross-binary determinism test going red** is the
fixed-point signal — binary and attributable, and it exists from P0 rather
than only becoming checkable at P3.
See `docs/specs/2026-09-04-architecture-resolution.md` § Q1.

**Decision — `libsim` link model enforced by the build graph.** A
`tickwire_sim_flags` INTERFACE target carries the float-determinism flags and
propagates them `PUBLIC` through `libsim`, so "identical flags in both
binaries" can't be violated by forgetting a flag on one target — the build
graph makes it structurally impossible, not just documented.
See § Q2.

**Decision — `Transport` interface shape, and a commitment to IPv4.**
`tryReceive(PacketSlot&)` fills a caller-owned slot (no
`optional<vector<byte>>`, no allocation). `Endpoint` commits to IPv4 with
`_be`-suffixed fields to make byte order part of the type name. Deferred to
P1 implementation; the shape is fixed now so P1 doesn't reopen it.
See § Q3.

**Decision — `SpscRing` ordering contract, fixed ahead of P5's need for it.**
Monotonic `uint64_t` indices (never wrapped), acquire/release pairing with no
`seq_cst` on the hot path, `alignas(64)` on both indices and the buffer to
avoid false sharing. Decided at P0 so P5's benchmark measures synchronization
cost, not a design mistake made under benchmark pressure.
See § Q4.

**Pivot — pinned Docker container replaces `sudo apt install g++-10` as task
zero.** The design doc's original P0 task zero needed a sudo password not
available to an automated session. `docker run` needs no sudo and was
verified reachable on this machine, so it became task zero instead — and
closes the CI-pinning problem with the same artifact (CI and local now run
the literal same image, not "the same version" by convention).
See § Q5.

**Finding — CMake 3.18 is a silent false-green.** Debian 11's packaged CMake
doesn't support `ctest --test-dir` — the exact command the workflow guide
prescribes for every checkpoint. It doesn't error; it prints
`No tests were found!!!` and **exits 0**. Every TDD checkpoint would have
reported green having executed nothing. This is why the toolchain image pins
CMake 3.28.4 from the Kitware release tarball rather than apt.
See § Q5, "CMake ≥ 3.21 is a correctness requirement, not polish."

**Finding — `-march=x86-64-v2` does not exist in GCC 10.** Micro-architecture
levels (`v2`/`v3`/`v4`) landed in GCC 11. Caught by an actual failed build,
not by review. `-march=x86-64` was chosen instead — baseline x86-64 has no
FMA at all, which closes the contraction hazard by itself.

**Finding — TSan needs capability flags *and* `setarch -R` together; neither
alone is sufficient.** `--cap-add SYS_PTRACE --security-opt seccomp=unconfined`
without `setarch -R` still aborts (`unexpected memory mapping`); `setarch -R`
without the capability flags fails (`Operation not permitted` —
`personality()` blocked by the container's seccomp profile). This was
re-verified live during P0 execution (2026-09-05): `scripts/verify-toolchain.sh`
failed with exactly the predicted `Operation not permitted` message before the
flags were added to `scripts/tw`, then passed once they were.

**Finding — raylib is not packaged for Ubuntu 20.04.** Must come via
`FetchContent` from source at P2. Its X11/GL build dependencies are already
on the host but will need adding to the Dockerfile if the client builds
inside the container. **The bigger open question — whether the container can
reach WSLg's display at all — is explicitly unresolved and deferred to P2**;
fallback is building the client on the host against the same pinned flags,
accepting a weaker determinism guarantee for the renderer only.
See § Q5, "New findings from verification" and "Residual risks."

**Pivot — the determinism test is two executables plus a comparator, not two
builds inside one test binary.** The design doc's original phrasing ("two
builds of `libsim` inside one test binary") is an ODR violation — a single
binary can't link two differently-built copies of the same symbols. Caught
while writing the P0 plan, before any code existed.
See `docs/plans/2026-09-04-phase-0-toolchain-skeleton.md` Task 4 preamble.

**Finding — Docker Desktop's WSL integration isn't always up.** On this
machine, `/usr/bin/docker` is a symlink into `/mnt/wsl/docker-desktop/`,
which only populates while Docker Desktop is running on the Windows host.
When it's down, every `scripts/tw` command fails as if Docker weren't
installed at all, not as a container error — check `docker info` first
before assuming a repo-level problem.

---

## P1 — Wire protocol, serialization, transport

**Decision — explicit little-endian encoding, byte by byte.** Every protocol
field is written/read through `net::ByteWriter`/`net::ByteReader`, never via
`memcpy` of a struct onto the wire or `reinterpret_cast` of a buffer to a
struct. Both alternatives make padding and host endianness part of the wire
format, and the cast is unaligned-access UB. LE was chosen because every
build target is LE, so the encode compiles to a no-op move. `_be`-suffixed
fields (`Endpoint::addr_be`/`port_be`) are reserved for values the *kernel*
requires in network order — the naming is the guard against silently mixing
the two orderings. See `docs/wire-format.md`.

**Decision — `ByteReader`/`ByteWriter` are the only place a byte offset is
computed.** Every codec above them (`encodeHeader`, `decodeInput`,
`decodeSnapshot`, ...) is bounds-safe by construction because it never does
its own offset arithmetic — it only calls `u8()`/`u16()`/`u32()`/`f32()` and
checks `ok()`. A failed read/write is sticky (once `ok()` is false, it never
resets) and, for reads, `remaining()` reports `0` — this is what lets a
codec check `ok()` once after a sequence of reads instead of after each one.

**Decision — strict framing: `payload_len` must equal the bytes actually
present, exactly.** Established in the header codec and reused by every
payload codec (`InputCommand` requires `remaining() == 17` on entry;
`WorldSnapshot` requires `remaining() == count * 24` after reading `count`).
Trailing bytes beyond the declared length are a rejection, not something a
downstream decoder has to defend against. The one documented exception is
`InputCommand::fire`, decoded as `u8 != 0` rather than requiring exactly `0`
or `1` — a `bool` has no invalid bit pattern to exploit, so strictness there
buys nothing.

**Decision — `WorldSnapshot::count` out of range is rejected, never
clamped.** A clamp is memory-safe at the clamp site but leaves `out.count`
describing more players than were filled — the same out-of-bounds read one
level up, in the caller. Caught in plan review before any code existed.

**Decision — `SimulatedTransport` delays on receive, not send.** `send()` is
a pure pass-through to the inner transport; each endpoint delays its own
*inbound* traffic. Consequence: a test with a `SimulatedTransport` wrapping
both ends of a link produces a round-trip delay equal to the **sum** of the
two configured latencies — which is what a UI slider labelled "RTT" means.

**Decision — `loss_permille` is an integer (0..1000), never a float.** Also,
no `std::uniform_int_distribution`/`uniform_real_distribution` anywhere in
`SimulatedTransport` — only raw `std::mt19937_64` output plus modulo.
Distribution objects are not specified to produce the same sequence across
standard library implementations, which would silently break the
reproducibility the class exists to guarantee. `msToTicks()` similarly stays
integer-only so the delivery schedule never depends on float rounding.

**Decision — `LoopbackTransport` is point-to-point, not a multi-peer
switch.** A multi-peer switch is what P2's authoritative server will want;
P1 needs exactly two parties to test netcode and to give `SimulatedTransport`
an inner transport. `connect()` is the extension point — YAGNI applied
deliberately, not an oversight.

**Deferred — `recvmmsg`/`sendmmsg` batching → P5.** They are batch calls, and
`Transport::tryReceive(PacketSlot&)` hands back one packet at a time by
design. Batching belongs with P5's receiver thread, where it is measurable
against a benchmark rather than guessed at.

**Deferred — join/leave reliability *logic* → P2; the `seq`/`ack_seq` header
fields are reserved now.** The reliability channel needs a session concept,
which arrives with P2's authoritative server. Carrying the fields at P1 means
P2 only adds retransmit logic, never reopens the header layout. Same
treatment for `tick`/`send_time_ms`/`ack_tick`, reserved for P3's clock sync
and RTT/drift estimation.

**Finding — the container's loopback interface works.** Task 5's `UdpTransport`
checkpoint (binding two sockets to `127.0.0.1` inside `tickwire-dev` and
exchanging a datagram) passed on the first run. The risk the P1 plan flagged
— an unverified container networking constraint that would also constrain
P2's demo — did not materialize.

**Finding — a stale test assertion, caught by the checkpoint after it.** Task
2 Checkpoint 2's test asserted that a bare 24-byte header (no payload bytes
following, `payload_len = 4` declared) decodes successfully. Checkpoint 3
then added the invariant that `payload_len` must equal the bytes actually
present — which correctly *rejects* that exact bare-header case, since zero
payload bytes follow a header claiming four. The two assertions couldn't
both hold; Checkpoint 3's invariant is the real one (it's the length-
confusion guard this task exists to build), so Checkpoint 2's assertion was
swapped for a well-formed header-plus-payload packet. Caught by ASan
(stack-use-after-scope in the *test*, from a `ByteReader` spanning a
temporary `std::array` returned by value) while fixing it, not by review.

**Finding — the 20,000-trial fuzz corpus's mandated seed produced zero
useful hits.** `robustness_test.cpp`'s random-byte sweep needs at least one
trial where header decode succeeds *and* a payload decode succeeds, to prove
the corpus isn't degenerating to pure magic-byte rejections. With the
plan-specified seed `0xC0FFEE`, the only realistic path to a successful
payload decode (an `InputCommand`-shaped trial: random total length happens
to equal 41, random type happens to be `kInput`) has expected value under 1
per run, and this seed landed on zero. Substituted seed `2`, which reliably
produces hits, following the same escape hatch the plan itself specifies for
Task 6 Checkpoint 4's jitter-inversion assertion (a fixed seed is for
reproducibility, not sacred).

### Task 7 security review — findings

Threat model: an unauthenticated attacker controls every byte of every
datagram and can send them at any rate, no handshake required. Reviewed
manually plus via the `cpp-reviewer` and `security-reviewer` agents.

**Fixed — `UdpTransport::bind()` leaked the previous socket on rebind.**
`bind()` unconditionally opened a new socket and overwrote `fd_` on success
with no check for an already-bound instance. Not attacker-triggerable
directly (requires the *caller* to call `bind()` twice), but a real resource
leak. Fixed by closing the old fd only after the new one is fully validated,
so a failed rebind attempt leaves the original working binding untouched.

**Fixed — `LoopbackTransport` was copyable/movable despite holding a raw
back-pointer.** `connect()` sets both sides' `peer_` to point at each other;
the compiler-generated copy/move would leave one side's `peer_` dangling
(after the original is destroyed) or stale. Not attacker-facing
(`LoopbackTransport` never touches real network traffic), but a genuine
footgun for anyone who later moves one into a container. Fixed by deleting
all four copy/move operations — nothing currently needs them.

**Fixed — non-finite floats (`NaN`/`Infinity`) were accepted by
`decodeInput`/`decodeSnapshot`.** An attacker could send a well-formed
packet with a `NaN` `move_x`, and `decodeInput` would hand it straight
through as valid data. `NaN` is sticky under IEEE-754; once P2/P3 wire
decoded payloads into `libsim`, this would permanently corrupt an entity's
simulation state every tick with no recovery short of reset. Fixed by
rejecting non-finite floats in both codecs, before assigning the caller's
output struct — consistent with "decoders reject rather than normalize."

**Fixed — several tests stack-allocated `LoopbackTransport`
(~310 KB)/`SimulatedTransport` (~157 KB) instead of using
`std::make_unique`.** This is an explicit Global Constraint in the P1 plan
specifically to avoid stack frame pressure under ASan. Self-caught while
reviewing the test files; converted throughout `simulated_test.cpp`.

**Deferred to P5 — `UdpTransport::tryReceive()`'s oversized-datagram retry
loop is not bounded per call.** On `MSG_TRUNC` detecting a datagram larger
than `kMaxPacket`, the loop discards it and retries `recvfrom` immediately,
within the same call. An attacker flooding oversized datagrams could make a
single `tryReceive()` call do more work than the "one call returns one
packet" contract implies — bounded by the kernel's socket receive buffer
(not unbounded), but a real deviation worth capping. Not fixed now because
P5 already owns the receiver-thread and `recvmmsg`-batching design where a
per-call drain cap belongs and can be benchmarked rather than guessed at.

**Deferred to P2 — no binding between a decoded `InputCommand::player_id`
and the UDP source `Endpoint` it arrived from.** Raw, handshake-less UDP
means any attacker who can reach the port can forge an `InputCommand`
claiming to be any `player_id`, including another live player's. No P1 code
path wires a decoded `InputCommand` into any authoritative state — `World`
and all simulation behavior are P2's job — so nothing is exploitable today.
Recorded so P2's join/leave + session (`seq`/`ack_seq`) design treats
endpoint-to-player binding as a requirement, not an afterthought.

---

## P2 — Authoritative server, `World`, first demo

**Pivot — the wire format reopened once, deliberately, to add `aim_x`/`aim_y`
to `InputCommand` at protocol version 2.** P1 froze `InputCommand` at 17
bytes before any consumer of an aim direction existed; P2's hitscan needs
one. Two alternatives were considered and lost: a separate `kFire` message
(rejected — it would need its own aim payload anyway, so it's the same cost
with an extra message type and no benefit) and aiming along the movement
vector (rejected — it forces "strafe to aim," which fights P6's rewind story
and would look wrong in the demo, where mouse-aim is expected). Contained to
Task 3, which re-froze the format in the same commit; no other task changed
a byte layout. See `docs/wire-format.md` for the frozen v2 tables.

**Decision — `PacketRing` prefigures P5's `SpscRing` API while being
explicitly non-atomic.** Same four-call shape (`acquireWrite`/`commitWrite`/
`acquireRead`/`commitRead`, monotonic indices, power-of-two masking) the
resolution doc fixed for `SpscRing` at P0, but single-threaded with no
`#include <atomic>` — so nobody mistakes it for the lock-free version. P5
swaps the implementation behind the same calls and benchmarks one against
the other, rather than restructuring the server to add threading.

**Decision — endpoint↔player-id binding is a single chokepoint in the
router, not a per-call-site check.** `Server::handleInput` is the only place
a decoded `InputCommand` reaches `World::applyInput`, gated by one
`SessionTable::authorize()` call. This is what makes the P1-deferred finding
(no binding between a decoded `player_id` and the UDP source it arrived
from) actually closeable by inspection — verified by grepping every call
site of `World::applyInput`, not just trusting the code's shape (see the
Task 8 security review below).

**Decision — hit counts stay server-side, off the wire.** `PlayerState` is
frozen at 24 bytes with no room for health or a hit counter, and the format
is being reopened exactly once this phase (see the pivot above), not twice.
`Server` tracks `hits(player_id)` in a private array; P6 can add a
client-visible hit-feedback `MsgType` additively (values 6–255 are unused)
without touching the header or the version.

**Decision — spawn positions are server policy, not simulation.** Player id
`i` spawns at a deterministic 8×4 grid position
(`x = -35 + 10*((i-1)%8)`, `y = -35 + 10*((i-1)/8)`), computed in
`Server::spawnPosition`, not in `libsim`. `World` only knows how to place a
player wherever it's told; where that is is a server-level policy decision,
kept out of the boundary that must stay I/O- and policy-free.

**Decision — the 60 Hz sim / 20 Hz snapshot cadence mismatch is deliberate,
not a bug to "fix."** `Server::tick()` steps the world every call but only
broadcasts a snapshot every third tick (`kSnapshotIntervalTicks = 3`). This
is what creates the need P4 exists to fix (entity interpolation smoothing
between sparser snapshots) — Task 8's `Client<T>` deliberately has no
interpolation or prediction yet, so the demo's visible lag at P2 is the
correct "before" state, not an oversight.

**Finding — the raylib display path works.** The one flagged environment
risk going into this phase (resolution doc § Q5 residual risk 2) resolved
cleanly: `tw_client --selftest` opens a real 800×800 GLX window through
WSLg's X11 passthrough into the container (Mesa/llvmpipe software
rasterizer, OpenGL 4.5 Core Profile), draws one frame, and exits 0; with
`DISPLAY` unset it exits 77 (CTest's `SKIP_RETURN_CODE`, not a failure). Two
unrelated environment defects had to be fixed first, both in the pinned
image, not in raylib or the project's own code:
- `bullseye-security`'s apt package *index* had drifted out of sync with its
  *pool* (a known Debian-archive gap once a package is superseded) — `curl`
  and `libgl1-mesa-dri` 404'd reproducibly, including across a `--no-cache`
  rebuild. Pinning individual package versions just pushed the same conflict
  onto their transitive dependencies, so the fix was disabling
  `bullseye-security` for this build entirely (every needed package resolves
  from plain `bullseye/main`, and a build-time toolchain image has no
  runtime exposure security patches would meaningfully cover).
- `scripts/tw`'s image tag bumped to `tickwire-dev:gcc10-cmake3.28.4-x11`
  (required — `scripts/tw` skips the build when the tag already exists, so
  editing the `Dockerfile` alone would have silently reused the stale
  image), with X11 passthrough (`DISPLAY` plus the `/tmp/.X11-unix` mount)
  applied only when both are present on the host, so CI (no X socket) is
  unaffected.

The full interactive/visual behavior (WASD feel, mouse aim, the latency
slider's effect on visible lag) was smoke-tested — server plus two GUI
clients ran 8s at target 60 FPS with no crash — but not eyeballed by this
session: the windows render through WSLg onto the host desktop, which this
session has no tool to capture or click into. That verification needs a
human actually running `scripts/tw bash scripts/demo.sh` and watching.

### Task 8 boundary — mandatory security review findings

Threat model (same as P1's): an unauthenticated attacker controls every byte
of every datagram, can send them at any rate, and can forge any source
address the network lets through. Reviewed via the `security-reviewer` and
`cpp-reviewer` agents over `src/server/`, `src/client/`, `apps/`, and
`tests/`.

**Verified closed — the P1-deferred finding.** P1 recorded (above) that no
code path bound a decoded `InputCommand::player_id` to the UDP source
`Endpoint` it arrived from. Grepping every call site of `World::applyInput`
confirms exactly one: `Server::handleInput` (`src/server/server.h`), gated
unconditionally by `SessionTable::authorize(from, in.player_id)` before the
input reaches `World`. `resolveHitscan` is reached only after that same gate
plus a cooldown check. `World::removePlayer` in `handleLeave` uses the id
looked up from the caller's own recorded endpoint, never an attacker-supplied
one. This defect is closed as designed.

**Fixed — `tests/client/client_test.cpp` stack-allocated `RecordingTransport`
(~1.24 MB) in all 8 tests.** Contradicted the project's own P1-established
convention (heap-allocate large test objects via `std::make_unique`), which
`tests/server/server_test.cpp` already follows for the identical type — a
straightforward regression in the newer file, not an ambiguous case. A
>1.2 MB base stack frame is a real overflow risk under ASan/TSan
instrumentation. Fixed by switching to `std::make_unique`.

**Fixed (footgun prevention) — `Server<T>` and `Client<T>` were copy-
constructible.** Both hold a `T&` reference member, which blocks implicit
copy/move *assignment* but not the copy *constructor* — copying either would
deep-copy `World`/`SessionTable`/`PacketRing` while binding the copy's
transport reference to the *same* transport as the original, giving two
independent simulation/session states silently sharing one socket. Not
currently reachable (every construction site uses `make_unique`), but deleted
outright since nothing needs it.

**Deferred, not fixed — `SessionTable` (900 B) stack-allocated in
`tests/server/session_test.cpp` (9 tests).** Named by the project's
large-test-object convention, but the reviewing agent's own assessment is
"not a real safety hazard at this size" (well under any plausible stack
limit even under ASan). Fixing it means touching dozens of call sites across
9 tests for no real risk reduction; judged not worth the churn.

**Deferred, not fixed — three CRITICAL findings, all one root cause: the
protocol has no cryptographic session binding, so an attacker who can spoof
UDP source addresses defeats address-based authorization entirely.** Put to
the user explicitly (this is a risk-posture/scope decision, not an
implementation detail); the user chose to defer and record, matching the
treatment P1 gave its own two deferred findings.

1. **Spoofable session authorization** (`SessionTable::authorize`,
   `src/server/session.cpp`, consumed at `src/server/server.h`'s
   `handleInput`/`handleLeave`). `authorize()` correctly implements its own
   contract — bind endpoint to player id, reject a mismatch — but the
   "endpoint" it trusts is the UDP source address, which is exactly the
   value the stated threat model already grants an attacker the power to
   forge. `player_id` is not a secret either: it is broadcast in cleartext
   in every `Snapshot`. A forged-source `Input` or `Leave` packet naming a
   known live player's id is indistinguishable from that player's own
   traffic. This is not a missed check (see "Verified closed" above); it's
   that the check's security property is exactly the capability the threat
   model grants the attacker.
2. **Join/snapshot amplification** (`Server::handleJoin` and
   `broadcastSnapshot`, `src/server/server.h`) — a direct consequence of
   Finding 1. A single ~24-byte forged `JoinRequest` creates a session
   broadcasting `Snapshot` packets (up to 800 B at 32 players) at 20 Hz to
   the forged address for up to `kSessionTimeoutTicks` (5 s) before it
   expires — up to ~3,333x volumetric amplification from one attacker
   packet, extendable indefinitely with a trickle of further forged `Input`
   packets (which do refresh the session's liveness).
3. **Session-table exhaustion** (`SessionTable::joinOrGet`,
   `src/server/session.cpp`) — the table is a fixed 32 slots with no
   proof-of-work or per-source rate limit on join. 32 forged distinct
   endpoints fill it, after which legitimate joins are rejected (server
   replies `Leave` on a full table, per Task 6). `expire()`'s 5 s timeout
   reclaims slots, but an attacker can win the race to refill them
   indefinitely for the cost of small forged UDP packets.

**Why deferred rather than fixed:** a real fix requires a per-session secret
(e.g., a token issued in `JoinAccept` and required on every subsequent
packet) — address correlation alone cannot authorize against a
source-spoofing attacker. That means reopening the wire format, which
`CLAUDE.md`'s Global Constraint says happens "exactly once, in Task 3... If a
later task discovers it needs a format change, that is a finding to record
and raise, not a change to make quietly." No phase in the current P2–P6
roadmap allocates scope for a session-authentication redesign, and the
design doc's own stated threat model (an unauthenticated, address-spoofing-
capable attacker) already assumes this class of gap exists — the project's
deliverable is a netcode-techniques demo and a measured numbers table, not a
hardened production service. Recorded here so **P3 does not silently
re-litigate or re-discover this** — any future phase that changes the wire
format again should treat a session-token scheme as a candidate addition,
not a surprise.

---

## P3 — Prediction, reconciliation, clock sync

**Pivot — the server's apply-on-arrival input path had to become a
tick-matched buffer, discovered before any prediction code was written.**
P2's `Server::handleInput` applied a decoded `InputCommand` the moment it
arrived, latching it as velocity that `World::step()` then integrated for
however many ticks passed until the next packet — a count that depends on
network jitter and that the client has no way to know in advance.
Reconciliation's replay assumes the opposite: "N pending inputs = N
simulated steps." Gabriel Gambetta's simpler model — the server processes
inputs as they land and reports the last one processed, the client replays
the rest — was considered and rejected for exactly this reason: it still
lets the server integrate a latched input for a jitter-dependent tick count
the client cannot reproduce, so reconciliation would fight prediction under
precisely the 200 ms condition the demo exists to showcase. `InputBuffer`
(Task 2) replaced it: one input consumed per player per tick, at the tick it
was stamped for, with an underrun repeating the previously-latched velocity
— the same behavior `World::step()` already had, now made deliberate rather
than incidental.

**Decision — clock sync is derived from `ack_tick − tick`, not an explicit
RTT round trip.** This is why the wire format did not need to reopen: a
`Snapshot`'s `tick` is already the reconciliation acknowledgment (the server
consumes inputs strictly in tick order, so a snapshot at tick `S` has
consumed every input stamped `≤ S`), and `ack_tick − tick` is the depth of
the session's server-side input buffer — the whole feedback signal
`client::ClockSync` needs, from two fields P1 reserved and P2 populated. No
separate handshake or RTT sample was ever required for the *ongoing*
correction loop. (The join handshake's own round trip *is* used, once, to
seed the clock — see the finding below; that is a bootstrap concern, not the
steady-state controller's signal.)

**Decision — the client predicts the local player only.** Predicting a
remote player would require predicting *its* inputs, which nothing can do;
smoothing remote players between snapshots is entity interpolation, P4's
scope. `Client`'s prediction world (`predicted_`) holds exactly one player.

**Decision — fires resolve in a second pass, after every session's input for
the tick has been applied.** Removes an ordering ambiguity P2 had
implicitly: a single fused per-session pass (apply, then fire, then move to
the next session) would make a hit depend on session iteration order.
Tracing this precisely surfaced a correction to the plan itself: `World`'s
position never changes until `World::step()`, which runs once at the end of
`Server::tick()`, after *both* passes — so a shot always evaluates against
positions from the start of the tick, regardless of any movement submitted
for that same tick by the shooter or the target, in either a two-pass or a
fused design. The two-pass split still matters (for future consumers that
apply per-session, and simply for the ordering being explicit rather than
incidental), but not for the reason first assumed.

**Finding — `sim::World::latched_` is dead state.** Written by `addPlayer`
and `applyInput`, never read anywhere — `World::step()` integrates
`players_[i].vx/vy` directly. This is *why* `World::setPlayerState`
(reconciliation's overwrite primitive, position and velocity only) is
sufficient: there is no hidden latch also needing to be restored. Flagged as
a `/refactor-clean` candidate; deliberately not removed in this phase, which
is not the place to touch working code for tidiness alone.

**Finding — `rttMs()` measures input-to-snapshot latency, not ping.** It
includes however long the server's `InputBuffer` held the input before
consuming it (up to `kTargetLeadTicks` worth) and the snapshot broadcast
interval (`kSnapshotIntervalTicks`), so it reads roughly 50–100 ms above the
wire round trip at 60 Hz. This is the number that matches what a player
actually feels, which is why it's the one shown on the HUD — but it must
never be labelled "ping" in the UI or the docs. See `docs/wire-format.md`'s
`send_time_ms` entry for the full accounting.

**Finding — a join-time clock seed needs the join handshake's own round
trip, not just a fixed margin, to bootstrap under real latency.** The
client's clock is seeded from a `JoinAccept`'s `h.tick` plus
`kTargetLeadTicks`, but `h.tick` is the server's tick as of when it *sent*
the accept — under real one-way latency, the server has already advanced
further by the time the client processes it, and every subsequent input
separately spends its own one-way trip reaching the server. A seed that only
adds `kTargetLeadTicks` leaves every input arriving already behind the
server's `InputBuffer` window — and since the ongoing correction loop only
engages once at least one input is accepted (`ack_tick != 0`), a clock that
starts this far behind has no way to recover on its own. Caught by Task 8's
real-latency convergence test, not by any unit test — the join handshake
inherently only exercises real latency when driven over an actual delayed
transport, which no Task 1–7 checkpoint did. Fixed by seeding from the join
handshake's own measured round-trip time (`Client::handleJoinAccept`), taken
in full: half covers "catch up to where the server is now," half covers
"survive this input's own future transit."

**Finding — a fixed-size, single-signal clock controller can oscillate
without damping, under real latency+jitter.** `ClockSync`'s original design
(a raw ±1 nudge on every observed snapshot, whenever the reported lead
missed `kTargetLeadTicks`) is a proportional controller closing a loop with
substantial dead time: by the time a snapshot is seen, it already reflects
the server's state from roughly one round trip ago, so several corrections
can be sent — and each one's effect not yet visible — before the next
observation arrives. Under 100 ms latency + 10 ms jitter, this produced a
measurable, *growing* oscillation (debug tracing showed the raw observed
lead swinging from roughly −43 to +42 across the run, the swing widening
rather than settling) rather than convergence. This is a real,
well-documented control-theory failure mode (a P-controller under dead time
needs damping proportional to the delay), not a tuning slip. Fixed with two
changes to the correction decision, both in `client::ClockSync`: an
EMA-smoothed lead estimate (filtering single-sample jitter noise) and — the
change that actually damped the oscillation — a cooldown limiting
corrections to at most one per `kCorrectionCooldownObservations`, so each
nudge has time to be reflected in a new observation before another is
allowed to fire. The large-error snap path (`kSnapErrorTicks`) is
deliberately exempt from the cooldown, so bootstrapping and recovery from a
genuine large desync stay immediate.

**Finding — `InputBuffer`'s original 16-slot (266 ms) acceptance window was
too narrow for the phase's own 200 ms-RTT demo scenario to even bootstrap.**
The join-seed fix above needs roughly 12 ticks of lead just to "catch up to
now" before `kTargetLeadTicks` is even added — inside a 16-slot window, that
leaves almost no headroom. A single overcorrection or jitter sample could
push the client's stamped ticks past the window's *upper* bound (not just
the lower one the window is usually reasoned about), and once that happens
every subsequent input is rejected as arriving "too far in the future" —
freezing `ack_tick` while the server's own tick keeps rising. `ClockSync`
reads a frozen, falling-behind `ack_tick` as "too far behind" and pushes the
clock *further* ahead in response: a runaway positive-feedback loop in the
wrong direction (one debug trace showed the client's tick reaching 6753
against a server tick of 801 before the fix). Widened `kInputBufferSlots` to
64 (1.067 s) — comfortably larger than any lead the current join-seed
formula or correction cooldown can produce.

**Measured convergence, Task 8 Checkpoint 2 (100 ms one-way latency + 10 ms
jitter each direction, 200 ms round trip, real `UdpTransport`, both server
and client legs delayed):** comparing each client's on-screen position
against the zero-latency ideal trajectory (both clients send identical
input from the same spawn) — the comparison that matches the demo's actual
claim, not a comparison against the server's own live, deliberately-lagging
tick (see the self-review note on this in the plan; comparing against the
server's live state would penalize prediction for doing its job). Predicting
client: **0.000015 units** from ideal — effectively exact. Non-predicting
client: **2.267 units** behind ideal (roughly 17 ticks' worth of movement at
`kMoveSpeed`) — a clearly visible lag. Full numbers and the two-phase
settle/move test design (decoupling "give the clock time to converge" from
"don't run the mover into the arena's clamp") are in
`tests/client/convergence_test.cpp`.

### Task 8 boundary — mandatory security review findings

Threat model (same as P1/P2's): an unauthenticated attacker controls every byte
of every datagram, can send them at any rate, and can forge any source address
the network permits. Reviewed via the `security-reviewer` and `cpp-reviewer`
agents in parallel over `src/server/`, `src/client/`, and `tests/`.

**Fixed (CRITICAL) — `Client::handlePacket` accepted a packet from any
source, not just the joined server.** `UdpTransport`'s socket is unconnected
(`bind()` never calls `connect()`), so `recvfrom` hands back a datagram from
*any* source reachable to the socket. `Client<T>` stored `server_` only as the
destination for outgoing sends; `handlePacket` never checked `slot.peer`
against it. This is a materially different, worse gap than the three findings
below: none of it requires source-address spoofing, unlike every P2-deferred
finding, which all require the attacker to already forge the *server's*
address. Concrete, zero-spoofing failure scenarios: a single forged `Leave`
with `ack_seq == kJoinSeq` while `kJoining` permanently blocks the client
(`kRejected` has no retry path back out); a forged `JoinAccept` races the real
server for an attacker-chosen player id; a stream of forged `Snapshot` packets
reaches `ClockSync::observe` and can walk the client's clock via the snap path
(deliberately exempt from the correction cooldown, so genuine large
desyncs still recover immediately) at an attacker-chosen rate rather than the
server's 20 Hz, eventually pushing every legitimate input outside the
server's `InputBuffer` window. Fixed with one source-endpoint check
(`if (slot.peer != server_) return;`) at the top of `handlePacket`, before any
header decode — no wire-format impact. Verified RED (both scenarios
reproduced exactly as predicted) with the check disabled, GREEN restored.

**Fixed (HIGH) — a departed player's queued future input survived to execute
under whoever inherited their id next.** `InputBuffer::reset()` existed
(documented as clearing every slot) but was never called. `SessionTable`
hands out the *lowest free* id, and `InputBuffer::push` accepts a tick up to
`kInputBufferSlots` (64, ~1.07 s) ahead of the server's own progress — so a
player who queues a future input and then leaves (or times out) leaves it
sitting in `inputs_[id-1]`, keyed only by id. The next player handed that same
id inherits it: `World::applyInput` (and `resolveHitscan`, if `fire` was set)
runs under the new player's identity for a command they never sent. This
reopens, at the `InputBuffer` layer, exactly the finding P2's own security
review verified closed — the chokepoint itself still holds (`authorize()`
gates every push, unchanged); the gap is that a buffered input's *lifetime*
isn't tied to the session's, so authorization checked at push time doesn't
protect against replay under a different, unconsenting owner of the same id
later. Fixed by clearing the departing player's `InputBuffer` at both removal
sites (`handleLeave`, and the timeout-expiry loop in `tick()`), immediately
after `World::removePlayer`. Regression test covers the explicit-`Leave` path,
verified RED then GREEN the same way as the finding above. The timeout-expiry
call site's fix is deliberately left without an equivalent test:
`kInputBufferSlots` (64) is narrower than `kSessionTimeoutTicks` (300), so a
still-live session's own `tick()` pass naturally consumes anything queued
within the window long before 300 ticks of silence could ever trigger
`expire()` — correct, consistent defensive practice (and it protects against
either constant changing independently later), but not an independently
exploitable path today given the current values.

**Fixed (MEDIUM) — `predicted_scratch_` was a persistent member used as a
call-scoped buffer.** `localPosition()` and `reconcile()` both wrote through it
via `predicted_.writeSnapshot(...)` but never needed its contents to persist
between calls. Converted to a local `sim::WorldSnapshot` in each, removing an
unnecessary `mutable` and shrinking every `Client<T>` by ~776 B.

**Deferred, not fixed (LOW) — `ClockSync::lead_` truncates a wider-range value
on the diagnostic accessor only.** `raw_lead` is computed in `int64_t` (needed,
since `ack_tick`/`server_tick` are `uint32_t` and their difference can be
large), but `lead_ = static_cast<int32_t>(raw_lead)` narrows it before storing.
Confirmed **not** a hazard for the actual correction math — `smoothed_lead_`
and `error`, which drive every decision `observe()` makes, are computed from
the untruncated `raw_lead`/its `float` cast, never from `lead_`. This is
diagnostic-accessor corruption only: under a large desync, `clockLead()` (used
by `tests/client/convergence_test.cpp` and intended for future telemetry/HUD
use, per Task 9) can report a small or wrapped value for what is actually a
huge gap. Real `ack_tick`/`server_tick` deltas are always small in practice
(single/low-double-digit ticks), so this doesn't currently manifest; deferred
as genuinely cosmetic rather than fixed now, since fixing it means deciding
`lead()`'s saturation behavior for values that can't happen under any
currently-reachable scenario. Revisit if `lead()` is ever surfaced to an
operator-facing dashboard rather than a HUD number or a test assertion.

**Deferred, not fixed (LOW) — `InputBuffer`'s window-ceiling arithmetic can
wrap after ~2.3 years of continuous uptime.** `last_consumed_ + kInputBufferSlots`
(`input_buffer.cpp`) is unguarded `uint32_t` addition; once `last_consumed_`
is within 64 of `UINT32_MAX` the sum wraps to a small value, transiently (~1 s,
self-recovering once `last_consumed_` itself wraps past it) rejecting
legitimate near-future inputs. Not attacker-acceleratable — tick count is
server-paced at 60 Hz, not attacker-influenced — so this is a long-uptime
robustness note, not a security finding worth blocking on.

**Noted, not fixed — `hits_` is also id-indexed and never reset on
removal.** Pre-existing since the P2 tip commit, not a P3 regression: a player
who inherits a reused id also inherits the departed occupant's cumulative hit
count (`Server::hits(id)`). Same fix pattern (reset-on-reuse) as the `InputBuffer`
finding above would apply; left as a follow-up rather than folded into this
phase's fix, since it's cosmetic (a reported statistic, not a state that
affects gameplay or authorization) and predates P3.

**Verified unchanged (still deferred) — the three P2 CRITICAL findings.**
`src/server/session.cpp`/`session.h` are byte-for-byte unchanged between the P2
tip commit and this phase (confirmed by diff, not assumed). All three —
spoofable session authorization, join/snapshot amplification, and
session-table exhaustion — stand exactly as recorded in P2's section above.
P3 adds no authentication and removes none; the input-buffering rewrite
changes *when* an authorized input is applied, never *whether* authorization
gates it.

**Re-verified closed — the P1-deferred, P2-verified-closed endpoint↔player
binding chokepoint.** Grepped every call site of `World::applyInput` across
the whole tree after the P3 rewrite, rather than assuming the P2 verification
survived it. Exactly two callers exist: `Server::tick()`'s pass 1
(`src/server/server.h`), still reached only through `handleInput`'s
unconditional `authorize()` gate — the buffering rewrite sits entirely
downstream of the same chokepoint, not around it — and `Client::sendInput`/
`Client::reconcile` (`src/client/client.h`), both applying only to `predicted_`,
a client-local `sim::World` with no authority. Confirmed by tracing every use
of `predicted_`: it feeds only local rendering (`localPosition`) and stats
(`reconcile`'s error recording); nothing derived from it is ever sent —
`sendInput` builds its outgoing `InputCommand` from the caller-supplied
movement parameters, never from `predicted_`'s own state. This is the same
distinction P2 recorded, now confirmed rather than waved at across a rewrite
of the surrounding code.

---

<!-- Next section: ## P4 — Entity interpolation, snapshot delta -->
