# Tickwire Wire Format (P1)

The wire format is frozen at P1. This document describes it; the golden byte
vectors in [`tests/net/protocol_test.cpp`](../tests/net/protocol_test.cpp)
**define** it — if this document and that test ever disagree, the test is
right and this document has a bug.

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
kProtocolVersion = 1
```

`0x52495754`'s little-endian bytes are `54 57 49 52`, which read as ASCII
`TWIR` in a hexdump.

## Packet header — 24 bytes

Every packet on the wire starts with this header.

| Offset | Size | Field | Notes |
|---:|---:|---|---|
| 0 | 4 | `magic` | must equal `kProtocolMagic` |
| 4 | 1 | `version` | must equal `kProtocolVersion` |
| 5 | 1 | `type` | `MsgType`; must be `1..kMaxMsgType` |
| 6 | 2 | `payload_len` | bytes following the header; must equal the bytes actually present |
| 8 | 4 | `tick` | sender's simulation tick — **reserved for P3** |
| 12 | 4 | `send_time_ms` | sender's monotonic ms — **reserved for P3** (RTT estimation) |
| 16 | 4 | `ack_tick` | highest tick seen from the peer — **reserved for P3** (drift correction) |
| 20 | 2 | `seq` | reliable channel sequence — **reserved for P2** (join/leave) |
| 22 | 2 | `ack_seq` | reliable channel ack — **reserved for P2** (join/leave) |

`MsgType`: `kInvalid = 0, kInput = 1, kSnapshot = 2, kJoinRequest = 3,
kJoinAccept = 4, kLeave = 5`. `kMaxMsgType = 5`.

**P1 transports `tick`, `send_time_ms`, `ack_tick`, `seq`, and `ack_seq` and
implements none of the logic that consumes them.** Carrying the fields now
means P2 (join/leave reliability) and P3 (clock sync, RTT, drift correction)
never have to reopen the header layout — only add the code that reads what's
already on the wire.

## `InputCommand` payload — 17 bytes

`MsgType::kInput`'s payload.

| Offset | Size | Field |
|---:|---:|---|
| 0 | 4 | `player_id` |
| 4 | 4 | `tick` |
| 8 | 4 | `move_x` (IEEE-754 binary32) |
| 12 | 4 | `move_y` (IEEE-754 binary32) |
| 16 | 1 | `fire` |

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
  fixed 17 bytes; `WorldSnapshot`'s `count × 24` bytes) — rejected.
- A non-finite (`NaN`/`Infinity`) float in any `move_x`/`move_y` or
  `PlayerState` field — rejected. An unauthenticated sender can otherwise
  inject a value that, once P2/P3 wire decoded payloads into `libsim`,
  corrupts an entity's simulation state every tick thereafter (`NaN` is
  sticky under IEEE-754).

**The one documented exception is `InputCommand::fire`**, decoded as `u8 !=
0` rather than requiring exactly `0` or `1`. A `bool` has no invalid bit
pattern to exploit, so strictness there buys nothing.

On rejection, a decoder never partially populates the caller's output
struct — it is assigned only once every check has passed.

## Authoritative source

The three layouts above, including every byte offset, are pinned by golden
byte vectors and exhaustive rejection-case tests in
[`tests/net/protocol_test.cpp`](../tests/net/protocol_test.cpp). Malformed-
input robustness (truncation sweeps and a 20,000-trial random-byte fuzz
corpus) lives in
[`tests/net/robustness_test.cpp`](../tests/net/robustness_test.cpp).
