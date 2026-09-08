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

## Packet header — 24 bytes

Every packet on the wire starts with this header.

| Offset | Size | Field | Notes |
|---:|---:|---|---|
| 0 | 4 | `magic` | must equal `kProtocolMagic` |
| 4 | 1 | `version` | must equal `kProtocolVersion` (2) |
| 5 | 1 | `type` | `MsgType`; must be `1..kMaxMsgType` |
| 6 | 2 | `payload_len` | bytes following the header; must equal the bytes actually present |
| 8 | 4 | `tick` | sender's simulation tick — **populated at P2** on every outgoing packet |
| 12 | 4 | `send_time_ms` | sender's monotonic ms — **populated at P2** on every outgoing packet; **reserved for P3** (RTT estimation is unimplemented — nothing reads it back yet) |
| 16 | 4 | `ack_tick` | highest tick seen from the peer — **populated at P2**, but only by the server on `Snapshot` packets (per-recipient: each session's own highest acknowledged input tick); **reserved for P3** (drift correction doesn't consume it yet) |
| 20 | 2 | `seq` | reliable channel sequence — **populated at P2** on the join/leave channel only (`kJoinSeq = 1`, `kLeaveSeq = 2`); zero on `Input`/`Snapshot` |
| 22 | 2 | `ack_seq` | reliable channel ack — **populated at P2**, echoed by the server on `JoinAccept`/`Leave` replies to the request's `seq`; zero elsewhere |

`MsgType`: `kInvalid = 0, kInput = 1, kSnapshot = 2, kJoinRequest = 3,
kJoinAccept = 4, kLeave = 5`. `kMaxMsgType = 5`. Values `6..255` are unused;
P6's hit-feedback message can be added additively without touching the
header or bumping the version again.

**P2 populates every header field but doesn't yet consume most of them.**
`tick`/`send_time_ms` are stamped on every outgoing packet and `ack_tick` on
every `Snapshot`, but no code computes RTT from a `send_time_ms` round trip
or corrects drift from `ack_tick` — that consumption logic is P3's job. The
join/leave channel (`seq`/`ack_seq`) is the one field pair P2 both populates
and fully consumes: it's what makes a retransmitted `JoinRequest` idempotent
and lets the client tell a stale `JoinAccept` from a current one.

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

## Maximum packet size

```
kHeaderBytes (24) + kSnapshotFixedBytes (8) + kMaxPlayers (32) × kPlayerStateBytes (24)
  = 24 + 8 + 768 = 800 ≤ kMaxPacket (1200)
```

`kMaxPlayers = 32` is the current ceiling on a single full snapshot. Room
between 800 and the 1200-byte MTU-safe ceiling is deliberate: P4's snapshot
delta compression is what spends it, not a larger `kMaxPlayers`.

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

**The one documented exception is `InputCommand::fire`**, decoded as `u8 !=
0` rather than requiring exactly `0` or `1`. A `bool` has no invalid bit
pattern to exploit, so strictness there buys nothing.

On rejection, a decoder never partially populates the caller's output
struct — it is assigned only once every check has passed.

## Authoritative source

The layouts above, including every byte offset, are pinned by golden byte
vectors and exhaustive rejection-case tests in
[`tests/net/protocol_test.cpp`](../tests/net/protocol_test.cpp) (header,
`InputCommand`, `WorldSnapshot`) and
[`tests/net/framing_test.cpp`](../tests/net/framing_test.cpp) (`JoinAccept`,
and `net::framePacket` — the header-plus-payload assembly every sender uses).
Malformed-input robustness (truncation sweeps and a 20,000-trial random-byte
fuzz corpus) lives in
[`tests/net/robustness_test.cpp`](../tests/net/robustness_test.cpp).
