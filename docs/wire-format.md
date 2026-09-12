# Tickwire Wire Format (frozen at P1, amended once at P2)

The wire format was frozen at P1 and amended exactly once, at P2, to add an
aim vector to `InputCommand` (see "Version history" below). This document
describes the current (version 2) format; the golden byte vectors in
[`tests/net/protocol_test.cpp`](../tests/net/protocol_test.cpp) **define**
it — if this document and that test ever disagree, the test is right and
this document has a bug.

## Endianness

Every multi-byte protocol field is **explicitly little-endian**, encoded and
decoded byte-by-byte through `net::ByteWriter`/`net::ByteReader`
([`src/net/bytes.h`](../src/net/bytes.h)) — never a `memcpy` of a struct onto
the wire, never a `reinterpret_cast` of a buffer to a struct. Both would make
padding and host endianness part of the format, and the cast is unaligned-
access undefined behavior.

Fields named with a `_be` suffix (`Endpoint::addr_be`, `Endpoint::port_be`)
are the one exception: they hold values the **kernel** requires in network
(big-endian) byte order, for POSIX socket calls. Every other field is
little-endian with no suffix. Mixing the two silently is the classic bug in
this area — the naming is the guard, not a convention to memorize.

## Magic and version

```
kProtocolMagic   = 0x52495754
kProtocolVersion = 2
```

`0x52495754`'s little-endian bytes are `54 57 49 52`, which read as ASCII
`TWIR` in a hexdump.

### Version history

- **v1 (P1):** the format below minus `InputCommand::aim_x`/`aim_y`
  (`InputCommand` was 17 bytes).
- **v2 (P2, 2026-09-08):** adds `aim_x`/`aim_y` to `InputCommand` (17 → 25
  bytes). P1 froze the payload before any consumer of an aim direction
  existed; P2's hitscan needs one. A version-1 header is now rejected outright
  — there is no cross-version compatibility. See
  `docs/project-history.md`'s P2 section for the alternatives considered and
  why they lost.
- **P3 (2026-09-09): no layout change, `kProtocolVersion` stays 2.** Client
  prediction, server reconciliation, and clock sync were built entirely on
  the header fields P1 reserved and P2 populated (`tick`/`send_time_ms`/
  `ack_tick`) — this is the record that the reservation worked as intended.
  See `docs/project-history.md`'s P3 section.
- **P4 (2026-09-11): no version bump, `kProtocolVersion` stays 2.** One
  additive message type, `MsgType::kSnapshotDelta = 6`, using the extension
  path this document already designated (`6..255` were reserved unused).
  `ack_tick` gains a second meaning, populated by the *client* on `Input`
  packets for the first time (previously always `0` in that direction) — see
  the header table and the new `SnapshotDelta` payload section below. See
  `docs/project-history.md`'s P4 section for the full design rationale.

## Packet header — 24 bytes

Every packet on the wire starts with this header.

| Offset | Size | Field | Notes |
|---:|---:|---|---|
| 0 | 4 | `magic` | must equal `kProtocolMagic` |
| 4 | 1 | `version` | must equal `kProtocolVersion` (2) |
| 5 | 1 | `type` | `MsgType`; must be `1..kMaxMsgType` |
| 6 | 2 | `payload_len` | bytes following the header; must equal the bytes actually present |
| 8 | 4 | `tick` | sender's simulation tick — **populated at P2** on every outgoing packet. On a `Snapshot`, this doubles as P3's reconciliation acknowledgment: the server consumes inputs strictly in tick order, so a snapshot at tick `S` has consumed every input stamped `≤ S`, and the client's reconciliation replay covers only the pending inputs stamped after it |
| 12 | 4 | `send_time_ms` | sender's monotonic ms — **populated at P2** on every outgoing packet; **consumed at P3**: the client records it per pending input and, when a later snapshot's `ack_tick` names that input, computes `now_ms - send_time_ms` as an input-to-snapshot latency estimate. This is *not* a pure network round trip — it includes however long the server's `InputBuffer` held the input before consuming it, plus the snapshot broadcast interval — so it reads roughly 50-100 ms above the wire RTT at 60 Hz and must not be labelled "ping" |
| 16 | 4 | `ack_tick` | highest tick seen from the peer — **populated at P2**, originally only by the server on `Snapshot` packets (per-recipient: each session's own highest accepted input tick); **consumed at P3**: `ack_tick - tick` on a snapshot is the depth of that session's server-side input buffer, and is the *sole* feedback signal driving the client's clock-sync controller (`client::ClockSync`) — no separate RTT measurement is taken. `ack_tick == 0` means the session has had no input accepted yet; the controller ignores such a snapshot entirely rather than reading it as an enormous negative lead. **Populated by the client too, at P4**: every outgoing `Input` now sets `ack_tick` to the newest snapshot tick that client holds (`0` if none yet) — this field had been reserved-but-always-zero in the client→server direction since P1. **Consumed by the server at P4**: `SessionTable::noteSnapshotAck` records it per session (monotonic — an older or implausible value, anything beyond the server's own current tick, is ignored) as the baseline a `SnapshotDelta` is cut against; `ack_tick == 0` (or a baseline that has aged out of the server's 16-snapshot history) makes the server send a full `Snapshot` instead |
| 20 | 2 | `seq` | reliable channel sequence — **populated at P2** on the join/leave channel only (`kJoinSeq = 1`, `kLeaveSeq = 2`); zero on `Input`/`Snapshot` |
| 22 | 2 | `ack_seq` | reliable channel ack — **populated at P2**, echoed by the server on `JoinAccept`/`Leave` replies to the request's `seq`; zero elsewhere |

`MsgType`: `kInvalid = 0, kInput = 1, kSnapshot = 2, kJoinRequest = 3,
kJoinAccept = 4, kLeave = 5, kSnapshotDelta = 6`. `kMaxMsgType = 6`. Values
`7..255` are unused; P6's hit-feedback message can take `7`, added
additively without touching the header or bumping the version again.

**Every header field is now both populated and consumed.** P2 stamped every
field but only fully consumed `seq`/`ack_seq` (the join/leave channel — what
makes a retransmitted `JoinRequest` idempotent and lets the client tell a
stale `JoinAccept` from a current one). P3 is the phase P1's reservation was
made for: `tick`/`ack_tick` on a `Snapshot` drive reconciliation and clock
sync (see the field notes above), and `send_time_ms` drives the input-to-
snapshot latency estimate. No byte layout changed to make this happen —
see "Version history" below.

## `InputCommand` payload — 25 bytes

`MsgType::kInput`'s payload.

| Offset | Size | Field |
|---:|---:|---|
| 0 | 4 | `player_id` |
| 4 | 4 | `tick` |
| 8 | 4 | `move_x` (IEEE-754 binary32) |
| 12 | 4 | `move_y` (IEEE-754 binary32) |
| 16 | 4 | `aim_x` (IEEE-754 binary32) — **added at v2** |
| 20 | 4 | `aim_y` (IEEE-754 binary32) — **added at v2** |
| 24 | 1 | `fire` |

## `JoinRequest` payload — empty

`MsgType::kJoinRequest`'s payload: zero bytes (`payload_len == 0`). The
client's own `seq` (always `kJoinSeq = 1`) is what the server echoes back in
`JoinAccept`'s `ack_seq`, making a retransmitted request idempotent — the
server never allocates a second player id for an endpoint it has already
bound.

## `JoinAccept` payload — 4 bytes

`MsgType::kJoinAccept`'s payload.

| Offset | Size | Field |
|---:|---:|---|
| 0 | 4 | `player_id` — never `0` (`kInvalidPlayerId` is rejected on both the encode and decode side) |

A server rejects a join against a full session table (32 slots) by replying
`MsgType::kLeave` instead, with `payload_len == 0` and `ack_seq` echoing the
request's `seq` — the same message type a voluntary leave uses, distinguished
by context (a client only expects it as a *reply to a join it is still
waiting on*).

## `Leave` payload — empty

`MsgType::kLeave`'s payload: zero bytes (`payload_len == 0`). Sent by a
client leaving voluntarily (`seq == kLeaveSeq = 2`, no reply expected) or by
a server rejecting a full-table join (see above, `ack_seq` echoes the
request).

## `WorldSnapshot` payload — `8 + count × 24` bytes

`MsgType::kSnapshot`'s payload.

| Offset | Size | Field |
|---:|---:|---|
| 0 | 4 | `tick` |
| 4 | 4 | `count` |
| 8 | 24 × `count` | `count` `PlayerState` records, back-to-back |

Each `PlayerState` record is 24 bytes: `id` (4), `x`, `y`, `vx`, `vy`,
`radius` (4 each, IEEE-754 binary32, in that order). Slots at or beyond
`count` are never transmitted.

## `SnapshotDelta` payload — `16 + 20 × changed_count` bytes

`MsgType::kSnapshotDelta`'s payload — **added at P4**. Encodes a snapshot as
the difference against a baseline the recipient already holds, instead of
resending every player. The server picks the baseline per session from the
client's own acknowledgment (`ack_tick` on `Input`, see the header table
above); a session with no usable baseline gets a full `Snapshot` instead —
`SnapshotDelta` is an optimization layered over the existing message, never a
replacement for it.

| Offset | Size | Field |
|---:|---:|---|
| 0 | 4 | `tick` — this delta's own tick, same meaning as `WorldSnapshot::tick` |
| 4 | 4 | `baseline_tick` — the tick of the snapshot this delta is cut against |
| 8 | 4 | `present_mask` — bit `(id - 1)` set: player `id` is present in this snapshot |
| 12 | 4 | `changed_mask` — bit `(id - 1)` set: a 20-byte record for `id` follows; else the recipient copies `id`'s record from its own baseline |
| 16 | 20 × `popcount(changed_mask)` | records for exactly the ids whose `changed_mask` bit is set, ascending by id: `x`, `y`, `vx`, `vy`, `radius` (IEEE-754 binary32, in that order — `id` itself is not on the wire; it is recovered from the bit position) |

Both masks are 32-bit, one bit per id, which is exactly `kMaxPlayers` — the
codec's `static_assert(sim::kMaxPlayers == 32, ...)` in
[`src/net/snapshot_delta.h`](../src/net/snapshot_delta.h) is what keeps this
true if that constant ever changes.

**Invariants a decoder enforces, not just documents:**

- `changed_mask` is always a subset of `present_mask`
  (`changed_mask & ~present_mask == 0`) — a record for a player the sender
  simultaneously claims is absent is incoherent and rejected.
- A player present in `current` but **absent from the baseline** (joined
  since, or a reused session slot) always has its `changed_mask` bit set —
  there is no baseline record to diff against, so it is always sent in full.
  A player **absent from `current`** (left since the baseline) has neither
  bit set: reconstructing drops it, driven by `present_mask` alone, never by
  iterating the baseline's own player list.
- `radius` travels in every sent record, even though it is always
  `sim::kPlayerRadius` today. The architecture-resolution doc names `radius`
  as "the first field to drop from a delta"; this format deliberately keeps
  it instead, in exchange for the stronger property that **a delta applied to
  its baseline reconstructs the full snapshot of the same tick, field for
  field** — no special-casing a joined-since-baseline player's radius. See
  `docs/project-history.md`'s P4 section for the full reasoning.
- A player unchanged since the baseline (all five fields compare `==`) costs
  zero bytes: no bit in `changed_mask`, no record.

## Maximum packet size

```
kHeaderBytes (24) + kSnapshotFixedBytes (8) + kMaxPlayers (32) × kPlayerStateBytes (24)
  = 24 + 8 + 768 = 800 ≤ kMaxPacket (1200)
```

`kMaxPlayers = 32` is the current ceiling on a single full snapshot. Room
between 800 and the 1200-byte MTU-safe ceiling is deliberate: P4's snapshot
delta compression is what spends it, not a larger `kMaxPlayers`.

**P4's delta, worst case (every player changed):**

```
kHeaderBytes (24) + kSnapshotDeltaFixedBytes (16) + kMaxPlayers (32) × kDeltaRecordBytes (20)
  = 24 + 16 + 640 = 680 ≤ kMaxPacket (1200)
```

That is still smaller than a full snapshot (800 B) even in the pathological
case where every player moved. The headroom this buys, expressed as "how
many players fit under the 1200 B MTU ceiling":

| Encoding | Payload | On the wire (+24 B header) | Max players |
|---|---|---|---:|
| Full snapshot | `8 + 24N` | `32 + 24N` | **48** |
| Delta, worst case (every player changed) | `16 + 20N` | `40 + 20N` | **58** |

At today's `kMaxPlayers = 32`: an all-changed delta is 680 B against a full
snapshot's 800 B (a 15% reduction even in the worst case), and a delta with
only a couple of movers is under 100 B — see `docs/project-history.md`'s P4
section for byte counts measured from an actual `tw_server` run.
`kMaxPlayers` itself stays at 32 this phase — P4 measures the headroom
delta buys rather than spending it; raising the ceiling is P5's concern,
where it can be load-tested rather than asserted.

## Decoder strictness

Every decoder rejects malformed input rather than normalizing it:

- Unknown `magic`, `version`, or `type` — rejected.
- `payload_len` disagreeing with the bytes actually present (too few, too
  many, or a bare header claiming a nonzero payload) — rejected.
- An out-of-range `WorldSnapshot::count` (`> kMaxPlayers`) — rejected, never
  clamped. Clamping would leave `out.count` describing more players than were
  filled, which is the same out-of-bounds read one level up in the caller.
- A payload longer or shorter than its declared framing (`InputCommand`'s
  fixed 25 bytes; `WorldSnapshot`'s `count × 24` bytes; `JoinAccept`'s fixed
  4 bytes) — rejected.
- A non-finite (`NaN`/`Infinity`) float in any `move_x`/`move_y`/`aim_x`/
  `aim_y` or `PlayerState` field — rejected. An unauthenticated sender can
  otherwise inject a value that, once wire-decoded payloads reach `libsim`
  (as of P2), corrupts an entity's simulation state every tick thereafter
  (`NaN` is sticky under IEEE-754). `World::applyInput`/`resolveHitscan`
  additionally re-check finiteness after squaring, catching the case two
  large-but-finite floats overflow to `Inf` on multiplication — see
  `docs/project-history.md`'s P2 security review section.
- A `JoinAccept` carrying `player_id == 0` (`kInvalidPlayerId`) — rejected on
  both the encode and decode side; `0` never travels as an accepted id.
- **`SnapshotDelta` (added at P4):** a `changed_mask` bit not present in
  `present_mask` — rejected (a record for a player simultaneously claimed
  absent is incoherent). A payload whose length disagrees with
  `16 + 20 × popcount(changed_mask)`, whether too few bytes (truncated) or
  too many (trailing bytes) — rejected, the same "declared framing must
  match the bytes present" rule `WorldSnapshot`'s `count × 24` already
  enforces. A non-finite float in any record field — rejected, same
  reasoning as `WorldSnapshot`'s records above.

**The one documented exception is `InputCommand::fire`**, decoded as `u8 !=
0` rather than requiring exactly `0` or `1`. A `bool` has no invalid bit
pattern to exploit, so strictness there buys nothing.

On rejection, a decoder never partially populates the caller's output
struct — it is assigned only once every check has passed.

## Authoritative source

The layouts above, including every byte offset, are pinned by golden byte
vectors and exhaustive rejection-case tests in
[`tests/net/protocol_test.cpp`](../tests/net/protocol_test.cpp) (header,
`InputCommand`, `WorldSnapshot`),
[`tests/net/framing_test.cpp`](../tests/net/framing_test.cpp) (`JoinAccept`,
and `net::framePacket` — the header-plus-payload assembly every sender uses),
and [`tests/net/snapshot_delta_test.cpp`](../tests/net/snapshot_delta_test.cpp)
(`SnapshotDelta`, added at P4). Malformed-input robustness (truncation sweeps
and 20,000-trial random-byte fuzz corpora, one per payload type) lives in
[`tests/net/robustness_test.cpp`](../tests/net/robustness_test.cpp).
