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

<!-- Next section: ## P2 continued — Task 9/10 decisions -->
