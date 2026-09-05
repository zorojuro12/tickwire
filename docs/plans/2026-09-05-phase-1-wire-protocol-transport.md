# P1 — Wire Protocol, Serialization & Injectable Transport Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use the `executing-plans` skill to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Freeze a bounds-safe binary wire format carrying the tick/timestamp/ack fields P3 will need, and ship three interchangeable `Transport` implementations — real UDP, in-memory loopback, and a deterministic latency/jitter/loss simulator — so P2 can move bytes without reopening either decision.

**Architecture:** A new static library `libnet` sits on top of `libsim` and inherits its pinned float flags automatically through `PUBLIC` linkage. Inside it, all encoding goes through two bounds-checked cursors (`ByteWriter`/`ByteReader`) whose contract makes an out-of-bounds access structurally impossible, so every codec above them is safe by construction rather than by review. Transport is a C++20 `concept`, not a virtual base: `UdpTransport` and `LoopbackTransport` satisfy it directly, and `SimulatedTransport<Inner>` **wraps** either one — the same class powers deterministic netcode tests (over loopback) and the demo's latency slider (over UDP).

**Tech Stack:** C++20 (GCC 10.5.0), CMake 3.28.4, GoogleTest, POSIX sockets (`socket`/`bind`/`recvfrom`/`sendto`, `O_NONBLOCK`, `MSG_TRUNC`), `std::span`, concepts, `std::mt19937_64`.

**Spec:**
- [`docs/specs/2026-09-04-tickwire-design.md`](../specs/2026-09-04-tickwire-design.md) — the design; P1 row, § Interface contracts, § Injectable transport, § Packet handling
- [`docs/specs/2026-09-04-architecture-resolution.md`](../specs/2026-09-04-architecture-resolution.md) — **authoritative**; Q2 (`libsim` surface), Q3 (transport shape)
- [`CLAUDE.md`](../../CLAUDE.md) — project conventions, auto-loaded
- [`docs/project-history.md`](../project-history.md) — P0 findings this plan builds on

**Plan format:** spec-driven (inline execution). Wire-format byte vectors are given
verbatim — they *are* the deliverable being frozen, not code derived from a contract.

**Suggested branch:** `phase-1-wire-protocol-transport`

## Global Constraints

Everything in `CLAUDE.md` applies. Restated here because a cold executing window
must not have to infer any of it, plus the constraints P1 adds.

**Carried from `CLAUDE.md` (P0-verified):**

- **Never invoke the host `g++`, `cmake`, or `ctest`.** All build/test commands go through `scripts/tw`, which runs them inside `tickwire-dev:gcc10-cmake3.28.4`.
- **TSan runs are wrapped in `setarch -R`**; the capability flags in `scripts/tw` and `setarch -R` are both required, neither alone.
- **Float flags on every target linking `libsim`:** `-march=x86-64`, `-ffp-contract=off`. Never `-march=native`, never `-ffast-math`, never `-march=x86-64-v2` (absent in GCC 10).
- **No `std::format`** — GCC 10 lacks it.
- **`libsim` has no I/O, no wall-clock reads, and no allocation.** Only trivially-copyable POD crosses its boundary.
- **`-Wall -Wextra -Werror`.**
- **Commits:** `type: description`. One commit per checkpoint, chained behind its test with `&&` — never `;`, never a separate line. `git add` names exact paths; **never** `git add -A` or `git add .`.
- **Test scoping:** inside a checkpoint use `-R <regex>`; the full suite only at a task boundary.
- **Coverage:** 80% project-wide; `libsim` at 100%.
- **File size:** 200–400 lines typical, 800 hard maximum.

**Added by P1:**

- **`libnet` is allocation-free too, and holds no wall-clock reads.** Every buffer is a caller-owned `std::span` or a fixed-size member array. `SimulatedTransport` reads no clock — its time comes only from `advanceTick()` calls.
- **Protocol fields are encoded explicitly little-endian, byte by byte.** Never `memcpy` a struct onto the wire and never `reinterpret_cast` a buffer to a struct — both make padding and host endianness part of the format, and the cast is unaligned-access UB that UBSan will flag. Explicit encoding means host endianness never enters; LE is chosen because every target here is LE, so the encode compiles to a no-op move.
- **`_be` suffixes are reserved for values the *kernel* requires in network order** (`Endpoint::addr_be`, `Endpoint::port_be`). Protocol fields carry no suffix and are little-endian. Mixing the two silently is the classic bug in this area; the naming is the guard.
- **Do not use `std::bit_cast`** — libstdc++ ships it from GCC 11. Pun `float`↔`uint32_t` with `std::memcpy`, which is well-defined regardless and compiles to the same register move.
- **Parsing is strict, not lenient.** A decoder rejects rather than normalizes: unexpected trailing bytes, an out-of-range count, or a length field that disagrees with the buffer are all failures. The one documented exception is `InputCommand::fire`, decoded as `u8 != 0` — a `bool` has no invalid bit pattern to exploit, so strictness there buys nothing.
- **Memory safety is in every contract from its first line, never deferred to a later checkpoint.** No checkpoint may knowingly ship an unbounded write and fix it afterwards. Checkpoints that look like "add the bounds check" are instead about the *reporting* semantics (`ok()` stickiness), which is a separate, non-memory behavior.
- **Tests construct transports through `std::make_unique`.** `LoopbackTransport` (~310 KB) and `SimulatedTransport` (~157 KB) carry fixed-size slot arrays; keeping them off the stack avoids frame pressure under ASan.
- **`libnet` links `libsim` `PUBLIC`**, so it inherits `tickwire_sim_flags` — the same build-graph guarantee P0 established, extended one layer.

---

## File Structure

| File | Responsibility |
|---|---|
| `src/net/bytes.h`, `.cpp` | `ByteWriter` / `ByteReader` — bounds-checked little-endian cursors. The only place a byte offset is computed. |
| `src/net/protocol.h`, `.cpp` | `PacketHeader`, `MsgType`, the frozen layouts and their codecs. |
| `src/net/transport.h` | `Endpoint`, `PacketSlot`, `kMaxPacket`, the `Transport` concept. Types only, no implementation. |
| `src/net/loopback.h`, `.cpp` | `LoopbackTransport` — in-memory, point-to-point, deterministic. |
| `src/net/udp.h`, `.cpp` | `UdpTransport` — non-blocking IPv4 POSIX socket, RAII fd. |
| `src/net/simulated.h` | `SimulatedTransport<Inner>` — template, header-only. |
| `src/sim/sim.h` | Gains `kTickHz`, `kMaxPlayers`, `PlayerState`, `InputCommand`, `WorldSnapshot`. Still no behavior beyond `advance()`. |
| `tests/net/bytes_test.cpp` | Cursor semantics. |
| `tests/net/protocol_test.cpp` | Frozen byte vectors, round-trips, every rejection case. |
| `tests/net/loopback_test.cpp` | Loopback delivery, ordering, capacity. |
| `tests/net/udp_test.cpp` | Real localhost datagrams, oversize handling, fd lifetime. |
| `tests/net/simulated_test.cpp` | Latency in ticks, seeded loss, jitter reordering, bounded buffer. |
| `tests/net/robustness_test.cpp` | Truncation and random-byte sweeps against every decoder. |
| `docs/wire-format.md` | The frozen format, as a reference document. |

**Two independent halves.** Tasks 1–3 (protocol) and Tasks 4–6 (transport) share
nothing — the transports move opaque bytes and the codecs never touch a socket.
They may be executed in either order. Task 7 depends on 1–3; Task 8 on everything.

---

## Task 1: Bounds-checked byte cursors

Every decoder in this phase parses bytes from an unauthenticated source. Rather
than bounds-check at each field, all offset arithmetic is confined to this one
pair of classes, so the codecs above cannot get it wrong.

**Files:**
- Create: `src/net/bytes.h`, `src/net/bytes.cpp`, `tests/net/bytes_test.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: nothing from earlier tasks.
- Produces, in `namespace net`:

```cpp
class ByteWriter {
 public:
  explicit ByteWriter(std::span<std::byte> buf) noexcept;
  void u8(uint8_t v) noexcept;
  void u16(uint16_t v) noexcept;
  void u32(uint32_t v) noexcept;
  void f32(float v) noexcept;
  bool   ok() const noexcept;
  size_t size() const noexcept;   // bytes written so far
};

class ByteReader {
 public:
  explicit ByteReader(std::span<const std::byte> buf) noexcept;
  uint8_t  u8() noexcept;
  uint16_t u16() noexcept;
  uint32_t u32() noexcept;
  float    f32() noexcept;
  bool     ok() const noexcept;
  size_t   remaining() const noexcept;   // 0 once ok() is false
};
```

- Produces: CMake target `libnet` (STATIC), linking `libsim` **PUBLIC**.
- Produces: CMake function `tw_add_test(<name> <sources...>)` — adds an executable linking `libnet` and `GTest::gtest_main`, and registers it with CTest under `<name>`.

**Checkpoint 1: values encode to exact little-endian bytes and never write past the end**

- [ ] **Step 1: Write the failing test, then run it**

Write `tests/net/bytes_test.cpp`. Add to `CMakeLists.txt`, after `enable_testing()`, the `libnet` target, the `tw_add_test` function, and `tw_add_test(bytes_test tests/net/bytes_test.cpp)`.

Spec — with a 16-byte buffer wrapped in a `ByteWriter`:
- `u8(0x54)` then `u16(0x0401)` then `u32(0x040004D2)` then `f32(1.0f)` produces exactly `54 01 04 D2 04 00 04 00 00 80 3F` (11 bytes), and `size() == 11`.
- `f32(-0.5f)` on a fresh 4-byte writer produces exactly `00 00 00 BF`.
- Bounds: on a writer over a 3-byte region carved out of a 8-byte array whose trailing 5 bytes are pre-filled with `0xEE`, calling `u32(0xFFFFFFFF)` leaves **all 8 bytes unchanged** and `size() == 0`.

Run:
```bash
scripts/tw cmake -S . -B build/plain -DCMAKE_BUILD_TYPE=RelWithDebInfo && \
scripts/tw cmake --build build/plain -j8 --target bytes_test
```
Expected: FAIL at the build step — `src/net/bytes.h: No such file or directory`.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: `ByteWriter` holds the span and a write cursor. Each writer method appends its value least-significant-byte-first. **A write that would not fit entirely writes nothing and does not advance the cursor** — this is unconditional, not a later addition. `f32` obtains the `uint32_t` bit pattern with `std::memcpy` into a local and then writes it as `u32`. `size()` returns the cursor.

```bash
scripts/tw cmake --build build/plain -j8 --target bytes_test && \
  scripts/tw ctest --test-dir build/plain -R bytes_test --output-on-failure && \
  git add CMakeLists.txt src/net/bytes.h src/net/bytes.cpp tests/net/bytes_test.cpp && \
  git commit -m "feat: add ByteWriter with explicit little-endian encoding"
```

Expected: PASS, then one commit.

**Checkpoint 2: a failed write is reported and is sticky**

- [ ] **Step 1: Write the failing test, then run it**

Spec — on a `ByteWriter` over a 3-byte buffer:
- `ok()` is `true` before any write.
- `u16(0x1234)` succeeds; `ok()` stays `true`; `size() == 2`.
- `u32(0)` does not fit; afterwards `ok() == false` and `size() == 2`.
- A subsequent `u8(0x7F)` — which *would* fit in the remaining byte — must **not** be written: `ok()` stays `false` and `size()` stays `2`.

The last bullet is the assertion that fails before this checkpoint: a writer that
merely skips oversized writes still accepts the later small one.

Run: `scripts/tw cmake --build build/plain -j8 --target bytes_test && scripts/tw ctest --test-dir build/plain -R bytes_test --output-on-failure`
Expected: FAIL — either `ok()` is not declared (build error) or, once declared, the post-failure `u8` is accepted and `size()` reads `3`.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: add a `bool ok_ = true` member. Every write method returns immediately when `!ok_`. A write that does not fit sets `ok_ = false`. Once false it is never reset — the writer is single-use.

```bash
scripts/tw cmake --build build/plain -j8 --target bytes_test && \
  scripts/tw ctest --test-dir build/plain -R bytes_test --output-on-failure && \
  git add src/net/bytes.h src/net/bytes.cpp tests/net/bytes_test.cpp && \
  git commit -m "feat: make ByteWriter overflow sticky and non-writing"
```

Expected: PASS, then one commit.

**Checkpoint 3: ByteReader round-trips ByteWriter output**

- [ ] **Step 1: Write the failing test, then run it**

Spec — write `u8(0x54)`, `u16(0xBEEF)`, `u32(0xDEADC0DE)`, `f32(3.5f)`, `f32(-0.0f)` into a 15-byte buffer, then read them back through a `ByteReader` over the same 15 bytes in the same order:
- each value compares equal to what was written;
- `-0.0f` round-trips **bit-exactly** — assert on the `uint32_t` obtained by `std::memcpy` from the result, which must be `0x80000000`, since `-0.0f == 0.0f` compares true and would hide a sign-bit loss;
- `remaining() == 0` after the last read, and `ok() == true` throughout.

Run: `scripts/tw cmake --build build/plain -j8 --target bytes_test && scripts/tw ctest --test-dir build/plain -R bytes_test --output-on-failure`
Expected: FAIL at the build step — `ByteReader` is not declared.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: `ByteReader` mirrors the writer — span plus a read cursor, least-significant-byte-first. **A read that would run past the end consumes nothing and returns `0` (or `0.0f`)**; this is unconditional. `f32` reads a `u32` and `std::memcpy`s it into a `float`. `remaining()` returns `buf.size() - cursor`.

```bash
scripts/tw cmake --build build/plain -j8 --target bytes_test && \
  scripts/tw ctest --test-dir build/plain -R bytes_test --output-on-failure && \
  git add src/net/bytes.h src/net/bytes.cpp tests/net/bytes_test.cpp && \
  git commit -m "feat: add ByteReader round-tripping ByteWriter output"
```

Expected: PASS, then one commit.

**Checkpoint 4: an over-read is reported, sticky, and hides the rest of the buffer**

- [ ] **Step 1: Write the failing test, then run it**

Spec — on a `ByteReader` over the 5 bytes `01 02 03 04 05`:
- `u32()` returns `0x04030201`; `ok() == true`; `remaining() == 1`.
- `u32()` again does not fit: it returns `0`, `ok()` becomes `false`, and **`remaining()` returns `0`** even though one physical byte is unread.
- A subsequent `u8()` returns `0` and `ok()` stays `false` — the reader does not resume on a request that would fit.

Run: `scripts/tw cmake --build build/plain -j8 --target bytes_test && scripts/tw ctest --test-dir build/plain -R bytes_test --output-on-failure`
Expected: FAIL — after Checkpoint 3 the second `u32()` returns `0` correctly, but `remaining()` reports `1` and the trailing `u8()` returns `0x05`.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: add `bool ok_ = true`. Every read returns zero immediately when `!ok_`; a read that does not fit sets `ok_ = false`. `remaining()` returns `0` when `!ok_`. Once false it is never reset. `ok()` is therefore the single check a codec makes after a sequence of reads, which is what keeps the codecs above free of offset arithmetic.

```bash
scripts/tw cmake --build build/plain -j8 --target bytes_test && \
  scripts/tw ctest --test-dir build/plain -R bytes_test --output-on-failure && \
  git add src/net/bytes.h src/net/bytes.cpp tests/net/bytes_test.cpp && \
  git commit -m "feat: make ByteReader overrun sticky and bounds-safe"
```

Expected: PASS, then one commit.

**Task boundary:**
```bash
scripts/tw ctest --test-dir build/plain --output-on-failure && \
scripts/tw cmake -S . -B build/asan -DCMAKE_BUILD_TYPE=RelWithDebInfo -DTW_SANITIZER=address,undefined && \
scripts/tw cmake --build build/asan -j8 && \
scripts/tw ctest --test-dir build/asan --output-on-failure
```

---

## Task 2: The packet header, frozen

The design doc requires the header to carry **tick, timestamp and ack** fields at
P1 so P3's clock synchronization does not have to reopen the wire format. The
fields are reserved and transported here; none of the logic that consumes them is
built in this phase.

`seq` and `ack_seq` are reserved on the same principle for the minimal
join/leave reliability channel. That channel needs a session concept, which
arrives with P2's authoritative server — so P1 carries the fields and P2 adds the
retransmit logic.

**Files:**
- Create: `src/net/protocol.h`, `src/net/protocol.cpp`, `tests/net/protocol_test.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `net::ByteWriter`, `net::ByteReader` from Task 1.
- Produces, in `namespace net`:

```cpp
inline constexpr uint32_t kProtocolMagic   = 0x52495754u;  // little-endian bytes spell "TWIR"
inline constexpr uint8_t  kProtocolVersion = 1;
inline constexpr size_t   kHeaderBytes     = 24;

enum class MsgType : uint8_t {
  kInvalid = 0, kInput = 1, kSnapshot = 2,
  kJoinRequest = 3, kJoinAccept = 4, kLeave = 5,
};
inline constexpr uint8_t kMaxMsgType = 5;

struct PacketHeader {
  uint32_t magic        = kProtocolMagic;
  uint8_t  version      = kProtocolVersion;
  MsgType  type         = MsgType::kInvalid;
  uint16_t payload_len  = 0;   // bytes following the header
  uint32_t tick         = 0;   // sender's simulation tick        (P3)
  uint32_t send_time_ms = 0;   // sender's monotonic ms           (P3, RTT)
  uint32_t ack_tick     = 0;   // highest tick seen from the peer (P3, drift)
  uint16_t seq          = 0;   // reliable channel sequence       (P2, join/leave)
  uint16_t ack_seq      = 0;   // reliable channel ack            (P2, join/leave)
};

bool encodeHeader(const PacketHeader& h, ByteWriter& w);
bool decodeHeader(ByteReader& r, PacketHeader& out);
```

**The frozen header layout** — 24 bytes, every multi-byte field little-endian:

| Offset | Size | Field |
|---:|---:|---|
| 0 | 4 | `magic` |
| 4 | 1 | `version` |
| 5 | 1 | `type` |
| 6 | 2 | `payload_len` |
| 8 | 4 | `tick` |
| 12 | 4 | `send_time_ms` |
| 16 | 4 | `ack_tick` |
| 20 | 2 | `seq` |
| 22 | 2 | `ack_seq` |

**Checkpoint 1: a header encodes to an exact 24-byte sequence and decodes back**

- [ ] **Step 1: Write the failing test, then run it**

Write `tests/net/protocol_test.cpp`; add `tw_add_test(protocol_test tests/net/protocol_test.cpp)` to `CMakeLists.txt` and `src/net/protocol.cpp` to `libnet`'s sources.

Spec — the header `{magic = kProtocolMagic, version = 1, type = MsgType::kInput, payload_len = 4, tick = 1234, send_time_ms = 123456, ack_tick = 1200, seq = 7, ack_seq = 9}` encoded into a 28-byte buffer produces, in its first 24 bytes, **exactly**:

```
54 57 49 52 01 01 04 00 D2 04 00 00 40 E2 01 00 B0 04 00 00 07 00 09 00
```

and `w.size() == kHeaderBytes`. Assert byte-for-byte against a literal
`std::array<std::byte, 24>` — this is the freeze, and any layout change must break
this assertion.

Then append four filler bytes `AA BB CC DD`, and decode a `ByteReader` over all 28
bytes: `decodeHeader` returns `true`, every field compares equal to the original,
and `r.remaining() == 4`.

Finally, the short-buffer sweep: for **every** prefix length `0` through `23` of
the golden 24 bytes, `decodeHeader` returns `false` and leaves the caller's
`PacketHeader` — pre-filled with the sentinel `tick = 0xAAAAAAAA` — untouched.
This belongs here rather than in its own checkpoint: `ByteReader`'s sticky `ok()`
from Task 1 makes it a direct consequence of the contract below, so it has no
independent RED.

Run:
```bash
scripts/tw cmake -S . -B build/plain -DCMAKE_BUILD_TYPE=RelWithDebInfo && \
scripts/tw cmake --build build/plain -j8 --target protocol_test
```
Expected: FAIL at the build step — `src/net/protocol.h: No such file or directory`.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: `encodeHeader` writes the nine fields in table order through the writer (`type` as `static_cast<uint8_t>`) and returns `w.ok()`. `decodeHeader` reads them back in the same order into a local `PacketHeader`, returns `false` if `!r.ok()`, and otherwise assigns to `out` and returns `true`. **Assign to `out` only on success** — a rejected packet must never partially populate a caller's header. Field validation arrives in Checkpoints 2 and 3.

```bash
scripts/tw cmake --build build/plain -j8 --target protocol_test && \
  scripts/tw ctest --test-dir build/plain -R protocol_test --output-on-failure && \
  git add CMakeLists.txt src/net/protocol.h src/net/protocol.cpp tests/net/protocol_test.cpp && \
  git commit -m "feat: add the frozen 24-byte packet header codec"
```

Expected: PASS, then one commit.

**Checkpoint 2: unknown magic, version, or message type is rejected**

- [ ] **Step 1: Write the failing test, then run it**

Spec — start from the 24 golden bytes and mutate one field per case; each must make `decodeHeader` return `false` and leave the caller's sentinel `PacketHeader` untouched:
- byte 0 changed from `0x54` to `0x55` (wrong magic);
- byte 4 changed from `0x01` to `0x02` (wrong version);
- byte 5 changed from `0x01` to `0x00` (`MsgType::kInvalid`);
- byte 5 changed from `0x01` to `0x06` (one past `kMaxMsgType`);
- byte 5 changed from `0x01` to `0xFF`.

The unmutated 24 bytes must still decode successfully in the same test, so a
blanket rejection cannot pass it.

Run: `scripts/tw cmake --build build/plain -j8 --target protocol_test && scripts/tw ctest --test-dir build/plain -R protocol_test --output-on-failure`
Expected: FAIL — Checkpoint 1's decoder accepts all five, returning `true`.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: after reading the fields and confirming `r.ok()`, `decodeHeader` returns `false` unless all three hold: `magic == kProtocolMagic`, `version == kProtocolVersion`, and the raw type byte satisfies `raw != 0 && raw <= kMaxMsgType`. Only then is the raw byte cast to `MsgType` and `out` assigned.

```bash
scripts/tw cmake --build build/plain -j8 --target protocol_test && \
  scripts/tw ctest --test-dir build/plain -R protocol_test --output-on-failure && \
  git add src/net/protocol.cpp tests/net/protocol_test.cpp && \
  git commit -m "feat: reject unknown magic, version, and message type"
```

Expected: PASS, then one commit.

**Checkpoint 3: a payload_len that disagrees with the packet is rejected**

This is the length-confusion guard. Every payload decoder below trusts that the
bytes following the header are exactly the payload the header claims; that trust
is established here and nowhere else.

- [ ] **Step 1: Write the failing test, then run it**

Spec — build a 28-byte packet: the golden header (`payload_len = 4`) plus four filler bytes. Each of these must make `decodeHeader` return `false`:
- the same 28 bytes with `payload_len` rewritten to `3` (bytes 6–7 → `03 00`) — one byte too few claimed;
- the same 28 bytes with `payload_len` rewritten to `5` — one byte too many claimed;
- the golden header alone (24 bytes) still claiming `payload_len = 4`;
- a header claiming `payload_len = 0xFFFF` followed by four filler bytes.

The unmodified 28-byte packet must still decode successfully.

Run: `scripts/tw cmake --build build/plain -j8 --target protocol_test && scripts/tw ctest --test-dir build/plain -R protocol_test --output-on-failure`
Expected: FAIL — all four are accepted; the decoder does not yet compare `payload_len` against what is left in the reader.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: add two guards before assigning `out`. First, `payload_len > kMaxPacket - kHeaderBytes` is rejected outright (defense in depth — the reader can never hold that much, but the bound is stated rather than inferred; `kMaxPacket` comes from `transport.h`, so `protocol.h` includes it). Second, `r.remaining() != h.payload_len` is rejected — strict equality, so trailing bytes beyond the declared payload are a rejection rather than something a later decoder has to defend against.

> Task 4 creates `src/net/transport.h`. If Tasks 4–6 have not been executed yet,
> create `transport.h` here with only `kMaxPacket`, `Endpoint` and `PacketSlot`,
> and add the `Transport` concept in Task 4 — the two halves of this phase are
> independent, and this constant is the single shared symbol.

```bash
scripts/tw cmake --build build/plain -j8 --target protocol_test && \
  scripts/tw ctest --test-dir build/plain -R protocol_test --output-on-failure && \
  git add src/net/protocol.h src/net/protocol.cpp src/net/transport.h tests/net/protocol_test.cpp && \
  git commit -m "feat: reject a payload_len that disagrees with the packet length"
```

Expected: PASS, then one commit.

**Task boundary:**
```bash
scripts/tw ctest --test-dir build/plain --output-on-failure && \
scripts/tw cmake --build build/asan -j8 && \
scripts/tw ctest --test-dir build/asan --output-on-failure
```

---

## Task 3: Payload types and their codecs

The wire format is only frozen if it is frozen over the actual payload types, so
this task adds the POD structs from the resolution doc § Q2 to `libsim` and
encodes them. **The `World` class and every simulation behavior stay in P2** —
what lands here is data, not physics, and `libsim` gains no executable lines
(so its 100% coverage floor is unaffected).

**Files:**
- Modify: `src/sim/sim.h`, `src/net/protocol.h`, `src/net/protocol.cpp`, `tests/net/protocol_test.cpp`

**Interfaces:**
- Consumes: `net::ByteWriter`, `net::ByteReader`, `net::kMaxPacket`.
- Produces, in `namespace sim`:

```cpp
inline constexpr uint32_t kTickHz     = 60;
inline constexpr uint32_t kMaxPlayers = 32;   // derived from the ~1200 B MTU; see resolution doc Q2
static_assert(kTickDt == 1.0f / static_cast<float>(kTickHz));

struct PlayerState  { uint32_t id; float x, y, vx, vy; float radius; };
struct InputCommand { uint32_t player_id; uint32_t tick; float move_x, move_y; bool fire; };
struct WorldSnapshot {
  uint32_t tick;
  uint32_t count;
  std::array<PlayerState, kMaxPlayers> players;
};
```

- Produces, in `namespace net`:

```cpp
inline constexpr size_t kInputBytes         = 17;
inline constexpr size_t kPlayerStateBytes   = 24;
inline constexpr size_t kSnapshotFixedBytes = 8;   // tick + count
static_assert(kHeaderBytes + kSnapshotFixedBytes
              + sim::kMaxPlayers * kPlayerStateBytes <= kMaxPacket);

bool encodeInput(const sim::InputCommand& in, ByteWriter& w);
bool decodeInput(ByteReader& r, sim::InputCommand& out);
bool encodeSnapshot(const sim::WorldSnapshot& s, ByteWriter& w);
bool decodeSnapshot(ByteReader& r, sim::WorldSnapshot& out);
```

**The frozen payload layouts** — every multi-byte field little-endian:

`InputCommand`, 17 bytes:

| Offset | Size | Field |
|---:|---:|---|
| 0 | 4 | `player_id` |
| 4 | 4 | `tick` |
| 8 | 4 | `move_x` (IEEE-754 binary32) |
| 12 | 4 | `move_y` |
| 16 | 1 | `fire` (`0` or `1`) |

`PlayerState`, 24 bytes: `id` (4), `x`, `y`, `vx`, `vy`, `radius` (4 each, in that order).

`WorldSnapshot`, `8 + count * 24` bytes: `tick` (4), `count` (4), then `count` `PlayerState` records. Slots beyond `count` are **not** transmitted.

**Checkpoint 1: an InputCommand encodes to an exact 17-byte sequence and decodes back**

- [ ] **Step 1: Write the failing test, then run it**

Spec — `sim::InputCommand{.player_id = 3, .tick = 1234, .move_x = 1.0f, .move_y = -0.5f, .fire = true}` encoded into a 17-byte buffer produces **exactly**:

```
03 00 00 00 D2 04 00 00 00 00 80 3F 00 00 00 BF 01
```

with `w.size() == kInputBytes`. Decoding a `ByteReader` over those 17 bytes returns `true` and every field compares equal, with `move_x`/`move_y` compared bit-exactly via `std::memcpy` to `uint32_t`.

Second case: the same bytes with the final byte set to `0x00` decode with `fire == false`; with the final byte set to `0x7F` they decode with `fire == true` — `fire` is deliberately lenient (`u8 != 0`), because a `bool` has no invalid bit pattern to exploit.

Third case, framing — both directions must be rejected, leaving the caller's `InputCommand` (sentinel `player_id = 0xAAAAAAAA`) untouched: every prefix length `0` through `16` of the golden bytes, and the golden 17 bytes plus one extra `0xEE` byte. These belong here rather than in a checkpoint of their own; the entry guard in the contract below covers both, so they have no independent RED.

Run: `scripts/tw cmake --build build/plain -j8 --target protocol_test && scripts/tw ctest --test-dir build/plain -R protocol_test --output-on-failure`
Expected: FAIL at the build step — `sim::InputCommand` is not declared and `net::encodeInput` does not exist.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: add the three POD structs and two constants to `src/sim/sim.h` (include `<array>` and `<cstdint>`; `std::array` does not allocate, so the no-allocation rule holds). `encodeInput` writes the five fields in table order, `fire` as `u8(in.fire ? 1 : 0)`, and returns `w.ok()`. `decodeInput` returns `false` unless `r.remaining() == kInputBytes` **on entry** — strict framing, so a payload with trailing bytes is a rejection — then reads the five fields, and assigns `out` only when `r.ok()`.

```bash
scripts/tw cmake --build build/plain -j8 --target protocol_test && \
  scripts/tw ctest --test-dir build/plain -R protocol_test --output-on-failure && \
  git add src/sim/sim.h src/net/protocol.h src/net/protocol.cpp tests/net/protocol_test.cpp && \
  git commit -m "feat: add sim payload types and the InputCommand codec"
```

Expected: PASS, then one commit.

**Checkpoint 2: a WorldSnapshot encodes to an exact byte sequence and decodes back**

- [ ] **Step 1: Write the failing test, then run it**

Spec — a `sim::WorldSnapshot` with `tick = 1234`, `count = 2`, and

- `players[0] = {.id = 1, .x = 2.0f, .y = 3.0f, .vx = 0.0f, .vy = -1.0f, .radius = 0.5f}`
- `players[1] = {.id = 2, .x = -4.0f, .y = 0.0f, .vx = 1.5f, .vy = 0.0f, .radius = 0.5f}`
- `players[2]` set to a sentinel (`id = 0xDEADBEEF`) that must **not** appear on the wire

encoded into a 56-byte buffer produces **exactly**:

```
D2 04 00 00  02 00 00 00
01 00 00 00  00 00 00 40  00 00 40 40  00 00 00 00  00 00 80 BF  00 00 00 3F
02 00 00 00  00 00 80 C0  00 00 00 00  00 00 C0 3F  00 00 00 00  00 00 00 3F
```

with `w.size() == kSnapshotFixedBytes + 2 * kPlayerStateBytes` (56). Decoding a `ByteReader` over those 56 bytes returns `true`, `tick` and `count` match, and both players compare field-by-field bit-exactly.

Also assert the empty case: `count = 0` encodes to exactly `D2 04 00 00 00 00 00 00` (8 bytes) and round-trips.

And the out-of-range count, which the contract below rejects rather than clamps — each must return `false` and leave the caller's `WorldSnapshot` (sentinel `tick = 0xAAAAAAAA`) untouched: the golden 56 bytes with `count` rewritten to `33` (bytes 4–7 → `21 00 00 00`), and with `count` rewritten to `0xFFFFFFFF` (`FF FF FF FF`).

Run: `scripts/tw cmake --build build/plain -j8 --target protocol_test && scripts/tw ctest --test-dir build/plain -R protocol_test --output-on-failure`
Expected: FAIL at the build step — `net::encodeSnapshot` does not exist.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: `encodeSnapshot` returns `false` immediately if `s.count > sim::kMaxPlayers` — a caller cannot be trusted to have filled the struct correctly either. Otherwise it writes `tick`, `count`, then `count` `PlayerState` records in field order, and returns `w.ok()`.

`decodeSnapshot` reads `tick` and `count`, then — **before `count` is used to index anything** — returns `false` unless `count <= sim::kMaxPlayers`. The bound is a rejection, not a clamp: a clamp would leave `out.count` describing more players than were filled, which is the same out-of-bounds read one level up in the caller. Only then does it read `count` records, return `false` unless `r.ok()`, and assign `out`. Fields of `out.players` at or beyond `count` are left as the caller had them — the decoder writes only what the wire carried. Strict framing arrives in Checkpoint 3.

```bash
scripts/tw cmake --build build/plain -j8 --target protocol_test && \
  scripts/tw ctest --test-dir build/plain -R protocol_test --output-on-failure && \
  git add src/net/protocol.h src/net/protocol.cpp tests/net/protocol_test.cpp && \
  git commit -m "feat: add the WorldSnapshot codec"
```

Expected: PASS, then one commit.

**Checkpoint 3: a snapshot carrying more bytes than its count declares is rejected**

Checkpoint 2 established that `count` cannot exceed the array. What is still
accepted is a payload *longer* than `count` describes — bytes the decoder
silently ignores. Under-long payloads are already caught by `ByteReader`'s sticky
`ok()`; this closes the other direction, so that "the header's `payload_len`
frames exactly one payload" holds end to end.

- [ ] **Step 1: Write the failing test, then run it**

Spec — each of these must make `decodeSnapshot` return `false` and leave the caller's `WorldSnapshot` (sentinel `tick = 0xAAAAAAAA`) untouched:
- the golden 56 bytes with `count` rewritten to `1` (bytes 4–7 → `01 00 00 00`) — in range, but a second record's 24 bytes still follow;
- the golden 56 bytes with `count` rewritten to `0` — in range, but 48 bytes still follow;
- the golden 56 bytes plus one extra `0xEE` byte, `count` left at `2`.

Also assert, in the same test, that these are still **rejected** (they are, via
`ByteReader`, and the assertions guard against a later refactor loosening it):
every prefix length `0` through `55` of the golden bytes with `count` left at `2`,
and `count` rewritten to `3` with only two records following. The unmodified 56
bytes must still decode successfully.

Run: `scripts/tw cmake --build build/plain -j8 --target protocol_test && scripts/tw ctest --test-dir build/plain -R protocol_test --output-on-failure`
Expected: FAIL on the first three cases — each is accepted, with 24, 48 and 1 trailing bytes respectively ignored.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: in `decodeSnapshot`, after the `count <= sim::kMaxPlayers` guard and before reading any record, return `false` unless `r.remaining() == static_cast<size_t>(count) * kPlayerStateBytes`. The widening to `size_t` is explicit so the product cannot wrap, and it is safe to compute only because the bound was checked first — that ordering is the point.

```bash
scripts/tw cmake --build build/plain -j8 --target protocol_test && \
  scripts/tw ctest --test-dir build/plain -R protocol_test --output-on-failure && \
  scripts/tw cmake --build build/asan -j8 --target protocol_test && \
  scripts/tw ctest --test-dir build/asan -R protocol_test --output-on-failure && \
  git add src/net/protocol.cpp tests/net/protocol_test.cpp && \
  git commit -m "feat: reject snapshots whose payload outruns their player count"
```

Expected: PASS under both configurations, then one commit.

**Task boundary:**
```bash
scripts/tw ctest --test-dir build/plain --output-on-failure && \
scripts/tw cmake --build build/asan -j8 && \
scripts/tw ctest --test-dir build/asan --output-on-failure
```

---

## Task 4: The Transport concept and an in-memory loopback

`Transport` is a **concept**, not a virtual base. The resolution doc § Q3 fixes
its shape; nothing here reopens it. `LoopbackTransport` is the first
implementation because it is the one the deterministic netcode tests and
`SimulatedTransport` both need, and it needs no kernel.

**Files:**
- Create: `src/net/loopback.h`, `src/net/loopback.cpp`, `tests/net/loopback_test.cpp`
- Modify: `src/net/transport.h` (created here, or extended if Task 2 created it first), `CMakeLists.txt`

**Interfaces:**
- Produces, in `namespace net` (`src/net/transport.h`):

```cpp
struct Endpoint {
  uint32_t addr_be = 0;   // IPv4, network byte order
  uint16_t port_be = 0;   // network byte order
  friend bool operator==(const Endpoint&, const Endpoint&) = default;
};

inline constexpr size_t kMaxPacket = 1200;   // stay under the IP fragmentation threshold

struct PacketSlot {
  Endpoint peer;
  uint16_t len = 0;
  std::array<std::byte, kMaxPacket> data{};
};

template <typename T>
concept Transport = requires(T t, const Endpoint& ep,
                             std::span<const std::byte> out, PacketSlot& slot) {
  { t.send(ep, out)    } -> std::same_as<bool>;
  { t.tryReceive(slot) } -> std::same_as<bool>;
};
```

- Produces, in `namespace net` (`src/net/loopback.h`):

```cpp
inline constexpr size_t kLoopbackCapacity = 256;

class LoopbackTransport {
 public:
  explicit LoopbackTransport(Endpoint self) noexcept;
  void connect(LoopbackTransport& peer) noexcept;   // links both directions
  Endpoint self() const noexcept;
  bool send(const Endpoint& to, std::span<const std::byte> payload);
  bool tryReceive(PacketSlot& slot);
  size_t inboxSize() const noexcept;
};
static_assert(Transport<LoopbackTransport>);
```

> **Point-to-point on purpose.** A multi-peer switch is what P2's authoritative
> server will want; P1 needs exactly two parties to test netcode and to give
> `SimulatedTransport` an inner. YAGNI — `connect()` is the extension point.

**Checkpoint 1: a packet reaches the connected peer intact, with the sender's endpoint**

- [ ] **Step 1: Write the failing test, then run it**

Write `tests/net/loopback_test.cpp`; add `tw_add_test(loopback_test tests/net/loopback_test.cpp)` and `src/net/loopback.cpp` to `libnet`.

Spec — two transports built with `std::make_unique`, `a` at `Endpoint{0x7F000001, 0x1F90}` and `b` at `Endpoint{0x7F000001, 0x1F91}`, then `a->connect(*b)`:
- `b->tryReceive(slot)` on an untouched inbox returns `false` and leaves `slot.len` at `0`.
- `a->send(b->self(), payload)` with the 5 bytes `01 02 03 04 05` returns `true`.
- `b->tryReceive(slot)` returns `true`, `slot.len == 5`, the first 5 bytes of `slot.data` equal the payload, and `slot.peer == a->self()`.
- A second `b->tryReceive(slot)` returns `false` — the packet was consumed, not left in place.
- The link is bidirectional: `b->send(a->self(), payload)` is received by `a` with `slot.peer == b->self()`.

Run:
```bash
scripts/tw cmake -S . -B build/plain -DCMAKE_BUILD_TYPE=RelWithDebInfo && \
scripts/tw cmake --build build/plain -j8 --target loopback_test
```
Expected: FAIL at the build step — `src/net/loopback.h: No such file or directory`.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: `LoopbackTransport` holds `Endpoint self_`, a `LoopbackTransport* peer_ = nullptr`, and a fixed `std::array<PacketSlot, kLoopbackCapacity>` inbox with head/tail indices. `connect(peer)` sets both objects' `peer_` to each other.

`send(to, payload)` returns `false` — writing nothing — if any of: `peer_ == nullptr`, `payload.size() > kMaxPacket`, or the peer's inbox is full. **The size guard is unconditional from this first line**, per the Global Constraints; a `memcpy` of an unchecked length into a 1200-byte array is exactly the defect this phase exists to avoid. Otherwise it copies `payload` into the peer's next inbox slot, sets that slot's `len` and `peer = self_`, and returns `true`.

`tryReceive(slot)` returns `false` if the inbox is empty; otherwise it copies the oldest slot into `slot`, advances the head, and returns `true`.

Add `static_assert(Transport<LoopbackTransport>)` at the bottom of the header — the concept is checked at compile time, not by a test, so it never becomes a checkpoint that passes when written.

```bash
scripts/tw cmake --build build/plain -j8 --target loopback_test && \
  scripts/tw ctest --test-dir build/plain -R loopback_test --output-on-failure && \
  git add CMakeLists.txt src/net/transport.h src/net/loopback.h src/net/loopback.cpp tests/net/loopback_test.cpp && \
  git commit -m "feat: add the Transport concept and an in-memory loopback transport"
```

Expected: PASS, then one commit.

**Checkpoint 2: the inbox delivers in FIFO order**

Ordering is a guarantee the netcode tests depend on: `SimulatedTransport`'s
reordering must come from configured jitter alone, never from the inner transport.

- [ ] **Step 1: Write the failing test, then run it**

Spec — with `a` connected to `b`, send three distinct one-byte payloads `AA`, `BB`, `CC` in that order without receiving in between. Then three `b->tryReceive` calls return `true` and yield `AA`, `BB`, `CC` **in that order**; a fourth returns `false`. Repeat once more after the inbox has drained, to prove the indices wrap correctly rather than only working on a fresh object.

Run: `scripts/tw cmake --build build/plain -j8 --target loopback_test && scripts/tw ctest --test-dir build/plain -R loopback_test --output-on-failure`
Expected: FAIL — a single-slot implementation that satisfies Checkpoint 1 either overwrites `AA` with `CC` or returns `false` on the second receive.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: the inbox is a ring — a monotonic `uint64_t` write index and read index, masked by `kLoopbackCapacity - 1` at access time (`kLoopbackCapacity` is a power of two, `static_assert` it). Full is `write - read == kLoopbackCapacity`, empty is `write == read`. This is deliberately the same index discipline the resolution doc § Q4 fixes for `SpscRing`, so P5 swaps in a lock-free ring without changing how callers reason about it — but this one is **single-threaded and non-atomic**; it is not `SpscRing` and must not be described as one.

```bash
scripts/tw cmake --build build/plain -j8 --target loopback_test && \
  scripts/tw ctest --test-dir build/plain -R loopback_test --output-on-failure && \
  git add src/net/loopback.h src/net/loopback.cpp tests/net/loopback_test.cpp && \
  git commit -m "feat: preserve FIFO order in the loopback inbox"
```

Expected: PASS, then one commit.

**Checkpoint 3: a full inbox drops the new packet without disturbing the queued ones**

- [ ] **Step 1: Write the failing test, then run it**

Spec — with `a` connected to `b`, send `kLoopbackCapacity` (256) one-byte payloads whose value is the low byte of their index; all 256 `send` calls return `true` and `b->inboxSize() == 256`. The 257th `send` returns `false`. Then drain: exactly 256 packets come out, in order, with the original values — the drop cost the *newest* packet, not any queued one, and did not corrupt the ring.

Run: `scripts/tw cmake --build build/plain -j8 --target loopback_test && scripts/tw ctest --test-dir build/plain -R loopback_test --output-on-failure`
Expected: FAIL — `inboxSize()` is not declared (build error); once declared, Checkpoint 2's ring wraps the 257th write over the oldest unread slot and the drain yields a wrong first value.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: add `inboxSize()` returning `write_ - read_`. In `send`, check `write_ - read_ == kLoopbackCapacity` before copying and return `false` if so. Dropping the newest rather than overwriting the oldest is the correct choice here — a bounded receive queue that discards fresh arrivals under overload is what a real socket does, and P5's queue benchmark measures against the same semantics.

```bash
scripts/tw cmake --build build/plain -j8 --target loopback_test && \
  scripts/tw ctest --test-dir build/plain -R loopback_test --output-on-failure && \
  git add src/net/loopback.h src/net/loopback.cpp tests/net/loopback_test.cpp && \
  git commit -m "feat: drop loopback sends when the peer inbox is full"
```

Expected: PASS, then one commit.

**Checkpoint 4: sends to anything but the connected peer are rejected**

- [ ] **Step 1: Write the failing test, then run it**

Spec — each must return `false`, and afterwards `b->inboxSize() == 0`:
- `a->send(Endpoint{0x7F000001, 0x2000}, payload)` — a well-formed endpoint that is not `b->self()`;
- `a->send(Endpoint{}, payload)` — the default-constructed endpoint;
- on a freshly built transport `c` that has never been `connect`ed, `c->send(a->self(), payload)`.

And `a->send(b->self(), payload)` in the same test must still return `true`, so a blanket rejection cannot pass.

Run: `scripts/tw cmake --build build/plain -j8 --target loopback_test && scripts/tw ctest --test-dir build/plain -R loopback_test --output-on-failure`
Expected: FAIL on the first two — Checkpoint 1's `send` ignores `to` entirely and delivers to `peer_` regardless. (The third already passes via the `peer_ == nullptr` guard.)

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: in `send`, after the `peer_ == nullptr` check, return `false` unless `to == peer_->self()`. Routing by address rather than by "wherever the link points" is what keeps the loopback substitutable for `UdpTransport`, where a wrong address silently goes nowhere instead of silently going to the peer — a difference that would otherwise only surface at P2.

```bash
scripts/tw cmake --build build/plain -j8 --target loopback_test && \
  scripts/tw ctest --test-dir build/plain -R loopback_test --output-on-failure && \
  git add src/net/loopback.cpp tests/net/loopback_test.cpp && \
  git commit -m "feat: reject loopback sends to an unconnected peer"
```

Expected: PASS, then one commit.

**Task boundary:**
```bash
scripts/tw ctest --test-dir build/plain --output-on-failure && \
scripts/tw cmake --build build/asan -j8 && \
scripts/tw ctest --test-dir build/asan --output-on-failure
```

---

## Task 5: Non-blocking IPv4 UDP transport

Raw POSIX sockets, per the design doc's hard stack requirements — no Boost.Asio,
no libevent. `recvmmsg`/`sendmmsg` are **deferred to P5**: they are batch calls,
and `tryReceive(PacketSlot&)` hands back one packet at a time by design. Batching
belongs with the receiver thread that P5 introduces, where it can be measured.

**Files:**
- Create: `src/net/udp.h`, `src/net/udp.cpp`, `tests/net/udp_test.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `net::Endpoint`, `net::PacketSlot`, `net::kMaxPacket`, `net::Transport` from Task 4.
- Produces, in `namespace net`:

```cpp
class UdpTransport {
 public:
  UdpTransport() noexcept = default;              // unbound; fd == -1
  ~UdpTransport();
  UdpTransport(const UdpTransport&) = delete;
  UdpTransport& operator=(const UdpTransport&) = delete;
  UdpTransport(UdpTransport&&) noexcept;
  UdpTransport& operator=(UdpTransport&&) noexcept;

  // Binds a non-blocking IPv4 UDP socket. port_be == 0 requests an ephemeral
  // port. On failure returns false and the object stays unbound.
  bool bind(uint32_t addr_be, uint16_t port_be);

  Endpoint localEndpoint() const noexcept;   // meaningful only after bind() succeeded
  int      nativeHandle() const noexcept;    // -1 when unbound

  bool send(const Endpoint& to, std::span<const std::byte> payload);
  bool tryReceive(PacketSlot& slot);
};
static_assert(Transport<UdpTransport>);
```

**Checkpoint 1: two bound sockets exchange a datagram on localhost**

- [ ] **Step 1: Write the failing test, then run it**

Write `tests/net/udp_test.cpp`; add `tw_add_test(udp_test tests/net/udp_test.cpp)` and `src/net/udp.cpp` to `libnet`.

Spec — bind two transports to `127.0.0.1` (`addr_be = htonl(INADDR_LOOPBACK)`) with `port_be = 0`:
- both `bind` calls return `true`; each `localEndpoint().port_be` is nonzero and the two differ;
- `b->tryReceive(slot)` before anything is sent returns `false` **and returns promptly** — the socket is non-blocking, so an empty queue is `EAGAIN`, not a hang;
- `a->send(b->localEndpoint(), payload)` with the 5 bytes `01 02 03 04 05` returns `true`;
- polling `b->tryReceive(slot)` in a bounded loop (at most 1000 iterations, no sleep) eventually returns `true` with `slot.len == 5`, matching bytes, and `slot.peer == a->localEndpoint()`.

> The bounded poll, rather than a single call, is because localhost delivery is
> effectively but not formally synchronous. A fixed iteration cap keeps the test
> from hanging if delivery never happens; failing the assertion after 1000 empty
> polls is the correct failure, not a timeout.

Run:
```bash
scripts/tw cmake -S . -B build/plain -DCMAKE_BUILD_TYPE=RelWithDebInfo && \
scripts/tw cmake --build build/plain -j8 --target udp_test
```
Expected: FAIL at the build step — `src/net/udp.h: No such file or directory`.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: `bind` creates the socket with `socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0)` — the flag on creation, not a later `fcntl`, so there is no window in which the socket blocks. It fills a `sockaddr_in` from `addr_be`/`port_be` **without any byte-order conversion**, since both fields are already in network order (that is what the suffix means), calls `bind`, then `getsockname` to learn the ephemeral port, and stores the result. Any failing syscall closes the fd, leaves `fd_ == -1`, and returns `false`.

`send` returns `false` if `fd_ < 0` or `payload.size() > kMaxPacket`, otherwise calls `sendto` and returns whether it wrote the whole payload.

`tryReceive` loops: `recvfrom(fd_, slot.data.data(), kMaxPacket, 0, ...)`. On `-1` with `EINTR` it retries; on `-1` with `EAGAIN`/`EWOULDBLOCK` it returns `false`; on any other error it returns `false`. On success it sets `slot.len`, fills `slot.peer` from the returned `sockaddr_in`, and returns `true`.

Note for the executor: the container has its own network namespace with a working loopback interface, so no host networking flag is needed. If `bind` to `127.0.0.1` fails inside the container, that is a finding — record it in `docs/project-history.md` rather than working around it silently.

```bash
scripts/tw cmake --build build/plain -j8 --target udp_test && \
  scripts/tw ctest --test-dir build/plain -R udp_test --output-on-failure && \
  git add CMakeLists.txt src/net/udp.h src/net/udp.cpp tests/net/udp_test.cpp && \
  git commit -m "feat: add a non-blocking IPv4 UDP transport"
```

Expected: PASS, then one commit.

**Checkpoint 2: oversized datagrams are dropped, not truncated into a slot**

A peer we do not control can send 1500 bytes. Silently handing the caller a
truncated 1200-byte packet would make the header's `payload_len` check — Task 2
Checkpoint 3 — reject it anyway, but one layer too late and with no way to tell a
hostile sender from a bug.

- [ ] **Step 1: Write the failing test, then run it**

Spec — with `a` and `b` bound as in Checkpoint 1, send an oversized datagram from a raw socket the test opens itself (`socket`/`sendto` directly, bypassing `UdpTransport::send`, which would refuse it):
- a 1201-byte datagram to `b`: polling `b->tryReceive(slot)` for 1000 iterations never returns `true`;
- a 1500-byte datagram likewise;
- immediately after each, a normal 5-byte datagram from `a` **is** received — one oversized datagram must not wedge the receive path;
- a datagram of exactly `kMaxPacket` (1200) bytes **is** received, with `slot.len == 1200`.

Assert the send-side counterpart in the same test — it has no independent RED, since Checkpoint 1's contract already guards `payload.size() > kMaxPacket`, but it belongs beside the receive-side cases: `a->send(...)` returns `false` for a 1201-byte and a 4096-byte payload and `true` for exactly 1200 bytes, and only the 1200-byte one is subsequently received.

Run: `scripts/tw cmake --build build/plain -j8 --target udp_test && scripts/tw ctest --test-dir build/plain -R udp_test --output-on-failure`
Expected: FAIL — Checkpoint 1's `recvfrom` truncates silently and returns `true` with `slot.len == 1200`, so the first two cases see a delivery.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: pass `MSG_TRUNC` to `recvfrom`. On a datagram socket Linux then returns the datagram's **real** size even though only `kMaxPacket` bytes were copied out. If that value exceeds `kMaxPacket`, discard the packet and `continue` the loop rather than returning — the datagram is already consumed from the socket queue, so continuing drains toward the next one and cannot spin.

`MSG_TRUNC` on `recvfrom` is a Linux extension; the design doc commits to Linux
(epoll, timerfd, affinity), so this adds no new platform constraint.

```bash
scripts/tw cmake --build build/plain -j8 --target udp_test && \
  scripts/tw ctest --test-dir build/plain -R udp_test --output-on-failure && \
  git add src/net/udp.cpp tests/net/udp_test.cpp && \
  git commit -m "feat: drop oversized datagrams instead of truncating into a packet slot"
```

Expected: PASS, then one commit.

**Checkpoint 3: the socket is closed on destruction and the type is move-only**

An fd leak in a server that accepts connections for hours is a real outage, and
it is invisible to every functional test.

- [ ] **Step 1: Write the failing test, then run it**

Spec:
- construct a `UdpTransport` in an inner scope, `bind` it, record `nativeHandle()` in an outer variable, let the scope end; then `::fcntl(fd, F_GETFD)` returns `-1` with `errno == EBADF`;
- move-construct `UdpTransport y{std::move(x)}` from a bound `x`: `y.nativeHandle()` is the original fd, `x.nativeHandle()` is `-1`, and `y` can still send and receive;
- after `y` is destroyed, that fd is closed exactly once — assert `::fcntl` reports `EBADF`, and confirm the moved-from `x` destructing first did not close it (order the scopes so `x` dies before `y` and the send-through-`y` assertion runs in between);
- `static_assert(!std::is_copy_constructible_v<UdpTransport> && !std::is_copy_assignable_v<UdpTransport>)`.

Run: `scripts/tw cmake --build build/plain -j8 --target udp_test && scripts/tw ctest --test-dir build/plain -R udp_test --output-on-failure`
Expected: FAIL at the build step — `~UdpTransport`, the move constructor, and the move assignment operator are not declared, and `nativeHandle()` does not exist.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: the destructor calls `::close(fd_)` when `fd_ >= 0`. The move constructor takes the source's `fd_` and `local_` and sets the source's `fd_` to `-1`. Move assignment closes its own fd first, then does the same, and self-assignment is a no-op. Copy operations stay `= delete`. `nativeHandle()` returns `fd_`.

```bash
scripts/tw cmake --build build/plain -j8 --target udp_test && \
  scripts/tw ctest --test-dir build/plain -R udp_test --output-on-failure && \
  git add src/net/udp.h src/net/udp.cpp tests/net/udp_test.cpp && \
  git commit -m "feat: close the UDP socket in the destructor and make the type move-only"
```

Expected: PASS, then one commit.

**Task boundary:**
```bash
scripts/tw ctest --test-dir build/plain --output-on-failure && \
scripts/tw cmake --build build/asan -j8 && \
scripts/tw ctest --test-dir build/asan --output-on-failure
```

---

## Task 6: `SimulatedTransport` — one wrapper, two payoffs

The design doc claims "one abstraction, two payoffs"; the resolution doc § Q3
supplies the mechanism — **composition**, not a sibling implementation. Wrapping
`LoopbackTransport` gives fully deterministic netcode tests; wrapping
`UdpTransport` gives the demo's latency slider over real localhost traffic. The
class is written once.

Determinism needs three things, all of them specified rather than left to the
implementer: a **seeded** `std::mt19937_64` (never `random_device`), an **injected
tick source**, and **milliseconds converted to ticks at construction time** so the
delivery schedule is integer tick arithmetic. If milliseconds survive into the
schedule, wall-clock dependence re-enters through the back door.

**Files:**
- Create: `src/net/simulated.h`, `tests/net/simulated_test.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `net::Transport`, `net::Endpoint`, `net::PacketSlot`, `net::LoopbackTransport`; `sim::kTickHz`.
- Produces, in `namespace net`:

```cpp
struct SimConfig {
  uint32_t latency_ms    = 0;
  uint32_t jitter_ms     = 0;
  uint32_t loss_permille = 0;   // 0..1000; integer so no float enters the schedule
  uint64_t seed          = 0;
};

inline constexpr size_t kDelayCapacity = 128;

// Milliseconds to ticks, rounded to nearest. Integer arithmetic only.
constexpr uint32_t msToTicks(uint32_t ms) noexcept {
  return (ms * sim::kTickHz + 500u) / 1000u;
}

template <Transport Inner>
class SimulatedTransport {
 public:
  SimulatedTransport(Inner& inner, const SimConfig& cfg) noexcept;
  bool     send(const Endpoint& to, std::span<const std::byte> payload);
  bool     tryReceive(PacketSlot& slot);
  void     advanceTick() noexcept;
  uint64_t tick() const noexcept;
  uint64_t droppedByLoss() const noexcept;
  uint64_t droppedByCapacity() const noexcept;
};
```

**The delivery algorithm, specified exactly** — two implementers must produce the
same delivery sequence for a given seed, or "deterministic" is a claim the code
cannot honor:

`send(to, payload)` is a **pure pass-through** to `inner_.send(to, payload)`. Delay
is applied on the receive side only. Each endpoint therefore delays its own
inbound traffic, so a test with a `SimulatedTransport` at both ends produces a
round-trip delay equal to the sum of the two — which is what a latency slider
labelled "RTT" means.

`tryReceive(slot)` does two phases in order:

1. **Drain.** Loop calling `inner_.tryReceive(tmp)` until it returns `false`. For each packet, draw **exactly two** values from the PRNG, always and in this order: `r_loss = rng_()`, then `r_jitter = rng_()`. Drawing unconditionally keeps the stream independent of configuration branches. Then:
   - if `r_loss % 1000u < cfg_.loss_permille`, discard the packet and increment `droppedByLoss` (with `loss_permille == 0` this is never true; with `1000` it is always true);
   - otherwise compute `delivery_tick = tick_ + latency_ticks_ + (r_jitter % (jitter_ticks_ + 1))` and enqueue `{delivery_tick, seq_++, packet}`. If the delay buffer is already full, discard and increment `droppedByCapacity` — **but keep draining `inner_`**, so a full buffer never backs traffic up into the inner transport.
2. **Deliver.** Among buffered entries with `delivery_tick <= tick_`, select the one with the smallest `delivery_tick`, breaking ties by the smallest `seq`. Copy it into `slot`, free its buffer entry, and return `true`. If there is none, return `false`.

The `seq` tie-break makes ordering total and reproducible: **reordering comes from
configured jitter alone**, never from buffer scan order.

`advanceTick()` increments `tick_`. **Nothing in this class reads a clock** — that
is what "injected tick source" means here, and it is why the same seed plus the
same call sequence reproduces the same delivery pattern exactly.

**Checkpoint 1: with everything zeroed, the wrapper is transparent**

- [ ] **Step 1: Write the failing test, then run it**

Write `tests/net/simulated_test.cpp`; add `tw_add_test(simulated_test tests/net/simulated_test.cpp)`.

Spec — two `LoopbackTransport`s `a` and `b` connected, `b` wrapped in a `SimulatedTransport` with `SimConfig{}` (all zeros):
- `tick() == 0` on construction;
- `a->send(b->self(), payload)` with 5 bytes, then `sim_b.tryReceive(slot)` returns `true` **on the same tick**, with matching `len`, bytes, and `slot.peer == a->self()`;
- a second `tryReceive` returns `false`;
- `droppedByLoss() == 0` and `droppedByCapacity() == 0`;
- `sim_b.send(a->self(), payload)` reaches `a` directly — the send path is not delayed;
- `msToTicks(0) == 0`, `msToTicks(16) == 1`, `msToTicks(100) == 6`, `msToTicks(200) == 12`.

Run:
```bash
scripts/tw cmake -S . -B build/plain -DCMAKE_BUILD_TYPE=RelWithDebInfo && \
scripts/tw cmake --build build/plain -j8 --target simulated_test
```
Expected: FAIL at the build step — `src/net/simulated.h: No such file or directory`, and `sim::kTickHz` is undeclared unless Task 3 has already run.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: implement the class per the algorithm above, with the delay buffer as `std::array<Entry, kDelayCapacity>` plus a parallel occupancy flag and a linear scan for selection (128 entries — a heap would be premature). `latency_ticks_` and `jitter_ticks_` are computed **in the constructor** via `msToTicks` and stored; `cfg_.latency_ms` is never consulted again.

If Task 3 has not run yet, add `sim::kTickHz` (and its `static_assert` against `kTickDt`) to `src/sim/sim.h` here, and note in the commit that Task 3 will find it present.

```bash
scripts/tw cmake --build build/plain -j8 --target simulated_test && \
  scripts/tw ctest --test-dir build/plain -R simulated_test --output-on-failure && \
  git add CMakeLists.txt src/net/simulated.h src/sim/sim.h tests/net/simulated_test.cpp && \
  git commit -m "feat: add SimulatedTransport as a pass-through Transport wrapper"
```

Expected: PASS, then one commit.

**Checkpoint 2: latency delays delivery by an exact number of ticks**

- [ ] **Step 1: Write the failing test, then run it**

Spec — `SimConfig{.latency_ms = 200, .jitter_ms = 0, .loss_permille = 0, .seed = 1}`, so `latency_ticks_ == 12`:
- `a->send(...)` once at tick 0;
- then for `t = 0 .. 19`: call `sim_b.tryReceive(slot)`, record whether it returned `true`, then `advanceTick()`;
- the recorded sequence is `false` for `t = 0..11` and `true` at exactly `t == 12`, and `false` for `t = 13..19`. Assert the delivered bytes match.

A second case pins that the delay is measured from arrival, not from construction: send a second packet at tick 5 (i.e. after five `advanceTick()` calls), and assert it is delivered at exactly tick 17.

Run: `scripts/tw cmake --build build/plain -j8 --target simulated_test && scripts/tw ctest --test-dir build/plain -R simulated_test --output-on-failure`
Expected: FAIL — Checkpoint 1's implementation delivers on tick 0 in both cases.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: enqueue with `delivery_tick = tick_ + latency_ticks_ + (r_jitter % (jitter_ticks_ + 1))` and deliver only entries whose `delivery_tick <= tick_`, smallest first with `seq` as tie-break.

```bash
scripts/tw cmake --build build/plain -j8 --target simulated_test && \
  scripts/tw ctest --test-dir build/plain -R simulated_test --output-on-failure && \
  git add src/net/simulated.h tests/net/simulated_test.cpp && \
  git commit -m "feat: schedule delivery in integer ticks converted from milliseconds"
```

Expected: PASS, then one commit.

**Checkpoint 3: loss is probabilistic, and identical for identical seeds**

The determinism claim itself. Asserting a hard-coded delivered count would pin a
libstdc++ implementation detail; asserting *reproducibility* pins the property the
project actually needs.

- [ ] **Step 1: Write the failing test, then run it**

Spec — a helper that runs one trial: given a `SimConfig`, build a fresh loopback pair and wrapper, then for `i = 0 .. 999` send a 4-byte payload encoding `i`, drain `tryReceive` until it returns `false` recording each delivered `i`, and `advanceTick()`. It returns the vector of delivered indices.

- `loss_permille = 0`, seed 7: all 1000 indices delivered, in order; `droppedByLoss() == 0`.
- `loss_permille = 1000`, seed 7: zero delivered; `droppedByLoss() == 1000`.
- `loss_permille = 250`, seed 7: the delivered count is strictly between `0` and `1000` (loss is neither all nor nothing), the delivered indices are strictly increasing (no reordering without jitter), and `droppedByLoss() + delivered == 1000`.
- Two trials with `loss_permille = 250, seed = 7` return **identical** vectors.
- A trial with `loss_permille = 250, seed = 8` returns a vector **different** from the seed-7 one.

Run: `scripts/tw cmake --build build/plain -j8 --target simulated_test && scripts/tw ctest --test-dir build/plain -R simulated_test --output-on-failure`
Expected: FAIL — Checkpoint 2's implementation delivers all 1000 in every case, so the `1000`-permille and strictly-between assertions both fail.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: seed `std::mt19937_64 rng_{cfg.seed}` in the constructor. In the drain phase draw `r_loss` then `r_jitter` unconditionally, and discard when `r_loss % 1000u < cfg_.loss_permille`, incrementing `dropped_by_loss_`. Never call `std::random_device`, and never use `std::uniform_int_distribution` or `uniform_real_distribution` — distribution objects are not specified to produce the same sequence across implementations, which would silently break the reproducibility this checkpoint asserts.

```bash
scripts/tw cmake --build build/plain -j8 --target simulated_test && \
  scripts/tw ctest --test-dir build/plain -R simulated_test --output-on-failure && \
  git add src/net/simulated.h tests/net/simulated_test.cpp && \
  git commit -m "feat: drop packets from a seeded PRNG at a configured rate"
```

Expected: PASS, then one commit.

**Checkpoint 4: jitter reorders delivery, reproducibly**

Reordering is the fourth impairment the design doc names, and it is what makes P4's
entity interpolation testable without a real network.

- [ ] **Step 1: Write the failing test, then run it**

Spec — `SimConfig{.latency_ms = 100, .jitter_ms = 100, .loss_permille = 0, .seed = 42}`, so `latency_ticks_ == 6` and `jitter_ticks_ == 6`. Send indices `0 .. 49`, one per tick, draining and advancing each tick for 80 ticks:
- all 50 are delivered (jitter must not lose packets);
- the delivered sequence is **not** sorted ascending — at least one inversion;
- two runs with `seed = 42` produce identical delivered sequences;
- a run with `seed = 43` produces a different one.

> The inversion is overwhelmingly likely rather than guaranteed: with jitter drawn
> uniformly from `[0, 6]` and one send per tick, an adjacent pair inverts when
> `jitter_i > jitter_{i+1} + 1`, which has probability `15/49 ≈ 0.31` per pair over
> 49 pairs — no inversion at all is on the order of `10^-8`. If seed 42 somehow
> produces none, substitute another fixed seed and **record which** in
> `docs/project-history.md`; do not switch to a random seed.

Run: `scripts/tw cmake --build build/plain -j8 --target simulated_test && scripts/tw ctest --test-dir build/plain -R simulated_test --output-on-failure`
Expected: FAIL on the inversion assertion — Checkpoint 3's implementation applies a constant delay, so the delivered sequence is strictly increasing.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: use `r_jitter % (jitter_ticks_ + 1)` as the per-packet jitter, and select for delivery by `(delivery_tick, seq)` ascending. The modulo carries a negligible bias for these ranges and is deterministic, which is the property that matters here; do not reach for a distribution object to remove it.

```bash
scripts/tw cmake --build build/plain -j8 --target simulated_test && \
  scripts/tw ctest --test-dir build/plain -R simulated_test --output-on-failure && \
  git add src/net/simulated.h tests/net/simulated_test.cpp && \
  git commit -m "feat: reorder delivery with seeded per-packet jitter"
```

Expected: PASS, then one commit.

**Checkpoint 5: the delay buffer is bounded and reports what it dropped**

An unbounded delay buffer is an allocation, and this phase declares none. A
bounded one that drops silently is worse than one that counts.

- [ ] **Step 1: Write the failing test, then run it**

Spec — `SimConfig{.latency_ms = 200, .jitter_ms = 0, .loss_permille = 0, .seed = 3}` (12 ticks of delay), with `kDelayCapacity == 128` and `kLoopbackCapacity == 256`:
- at tick 0, send 200 distinct one-byte payloads through `a` (all accepted, since 200 ≤ 256);
- still at tick 0, call `sim_b.tryReceive(slot)` once; it returns `false` (nothing is due yet) but has drained all 200 from the inner transport;
- immediately afterwards `droppedByCapacity() == 72` and `droppedByLoss() == 0`;
- `b->inboxSize() == 0` — the full delay buffer did not leave packets stranded in the inner transport;
- advancing 20 ticks and draining each yields exactly **128** packets, and they are the first 128 sent (`0 .. 127`), since the buffer filled in arrival order;
- the whole test runs clean under ASan.

Run: `scripts/tw cmake --build build/plain -j8 --target simulated_test && scripts/tw ctest --test-dir build/plain -R simulated_test --output-on-failure`
Expected: FAIL — `droppedByCapacity()` is not declared (build error); once declared, Checkpoint 4's implementation either writes past the 128-entry array (ASan reports a buffer overflow) or stops draining at 128 and strands 72 in the loopback.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: add `dropped_by_capacity_`. In the drain loop, when no buffer slot is free, discard the packet, increment the counter, and **continue the loop** rather than breaking — draining to completion is what keeps the inner transport from backing up, and it is the difference between "the simulator dropped it" and "the simulator wedged".

```bash
scripts/tw cmake --build build/plain -j8 --target simulated_test && \
  scripts/tw ctest --test-dir build/plain -R simulated_test --output-on-failure && \
  scripts/tw cmake --build build/asan -j8 --target simulated_test && \
  scripts/tw ctest --test-dir build/asan -R simulated_test --output-on-failure && \
  git add src/net/simulated.h tests/net/simulated_test.cpp && \
  git commit -m "feat: bound the delay buffer and count capacity drops"
```

Expected: PASS under both configurations, then one commit.

**Task boundary:**
```bash
scripts/tw ctest --test-dir build/plain --output-on-failure && \
scripts/tw cmake --build build/asan -j8 && \
scripts/tw ctest --test-dir build/asan --output-on-failure
```

---

## Task 7: Malformed-input robustness, and the mandatory security review

`CLAUDE.md` makes `security-review` **mandatory** for any phase touching the
network surface. This is that phase: the decoders parse bytes from an
unauthenticated source over UDP, where an attacker chooses every byte and needs no
handshake to deliver them.

Tasks 2 and 3 test the malformed cases someone *thought of*. This task tests the
ones nobody thought of, by generating them.

**Files:**
- Create: `tests/net/robustness_test.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `net::decodeHeader`, `net::decodeInput`, `net::decodeSnapshot`, `net::ByteReader`, and the golden byte vectors from Tasks 2–3.
- Produces: no new production symbols. This task adds tests only.

**Checkpoint 1: every truncation of a valid packet is rejected without crashing**

- [ ] **Step 1: Write the failing test, then run it**

Spec — build three complete, valid packets, each as header-plus-payload in one buffer:
- header `type = kInput`, `payload_len = 17`, plus the golden `InputCommand` payload — 41 bytes;
- header `type = kSnapshot`, `payload_len = 56`, plus the golden 2-player `WorldSnapshot` payload — 80 bytes;
- header `type = kSnapshot`, `payload_len = 776`, plus a full 32-player snapshot payload — 800 bytes, the largest packet the format can produce (assert that against `kMaxPacket`).

For each packet and for **every** prefix length from `0` up to one byte short of the full length, run the full decode path — `ByteReader` over the prefix, `decodeHeader`, and on success the matching payload decoder — and assert:
- the overall decode reports failure (either `decodeHeader` or the payload decoder returns `false`);
- no assertion in the decoder is reached with an out-of-range index — i.e. the run is clean under ASan and UBSan;
- the caller's output structs, pre-filled with sentinels, are untouched.

Each full-length packet must decode successfully, so a decoder that rejects everything cannot pass.

Run:
```bash
scripts/tw cmake -S . -B build/plain -DCMAKE_BUILD_TYPE=RelWithDebInfo && \
scripts/tw cmake --build build/plain -j8 --target robustness_test
```
Expected: FAIL at the build step — `tests/net/robustness_test.cpp` is not yet registered in `CMakeLists.txt`, and once registered the file does not exist.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: this checkpoint's "implementation" is the test itself plus its CMake registration. **Every assertion is expected to pass against the decoders as Tasks 2–3 left them** — that is the claim being made, and the value is in proving it by exhaustion rather than by argument.

If any case fails, that is a genuine defect in a decoder: fix the decoder, not the
test, add the specific failing input as a named case in `protocol_test.cpp` so it
is pinned at the layer that owns it, and record the finding in
`docs/project-history.md`.

```bash
scripts/tw cmake --build build/plain -j8 --target robustness_test && \
  scripts/tw ctest --test-dir build/plain -R robustness_test --output-on-failure && \
  scripts/tw cmake --build build/asan -j8 --target robustness_test && \
  scripts/tw ctest --test-dir build/asan -R robustness_test --output-on-failure && \
  git add CMakeLists.txt tests/net/robustness_test.cpp && \
  git commit -m "test: prove every truncation of a valid packet is rejected without crashing"
```

Expected: PASS under both configurations, then one commit.

> This checkpoint is deliberately of a different kind from the rest: its RED is
> "the test does not exist", not "the behavior is missing". A sweep that confirms
> an already-established invariant by exhaustion is worth its own commit precisely
> because it is the artifact a reviewer checks, and it cannot be folded into a
> behavior checkpoint without losing that.

**Checkpoint 2: arbitrary byte buffers never crash a decoder**

- [ ] **Step 1: Write the failing test, then run it**

Spec — a seeded `std::mt19937_64{0xC0FFEE}` drives 20,000 trials. Each trial:
- picks a length uniformly in `[0, kMaxPacket]` (via `rng() % (kMaxPacket + 1)`, no distribution object, so the corpus is reproducible);
- fills that many bytes with `rng()` output;
- with probability roughly one in four, overwrites the first six bytes with a valid magic, version, and a valid `type` — otherwise almost every trial is rejected at the magic check and the payload decoders are never reached;
- runs the full decode path and asserts: the call returns cleanly either way, and **when `decodeHeader` returns `true`, `h.payload_len` equals the bytes remaining after the header** — the invariant every payload decoder relies on;
- when a payload decoder returns `true` for a snapshot, asserts `out.count <= sim::kMaxPlayers`.

Also assert the corpus is doing work: at least one trial in the run reaches a payload decoder, and at least one payload decode returns `true`. A test that silently degenerates to 20,000 magic-check rejections proves nothing, and this assertion is what catches that.

Run: `scripts/tw cmake --build build/plain -j8 --target robustness_test && scripts/tw ctest --test-dir build/plain -R robustness_test --output-on-failure`
Expected: FAIL at the build step — the new test case does not exist yet.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: the test as specified. Must run under ASan and UBSan, where a stray read or a signed-overflow in the length arithmetic becomes a failure rather than a silent pass. Keep the trial count at 20,000 — enough to exercise the branches, fast enough not to slow the sanitizer suites that run on every task boundary from here on.

```bash
scripts/tw cmake --build build/plain -j8 --target robustness_test && \
  scripts/tw ctest --test-dir build/plain -R robustness_test --output-on-failure && \
  scripts/tw cmake --build build/asan -j8 --target robustness_test && \
  scripts/tw ctest --test-dir build/asan -R robustness_test --output-on-failure && \
  git add tests/net/robustness_test.cpp && \
  git commit -m "test: prove random byte buffers never crash the decoders"
```

Expected: PASS under both configurations, then one commit.

**Task boundary — the mandatory security review.** Not a checkpoint; it produces
findings, not a commit, and it gates Task 8.

- [ ] Run the full suite under both configurations:
```bash
scripts/tw ctest --test-dir build/plain --output-on-failure && \
scripts/tw cmake --build build/asan -j8 && \
scripts/tw ctest --test-dir build/asan --output-on-failure
```
- [ ] Invoke the **`security-review` skill** (or the `security-reviewer` agent) over `src/net/` and `tests/net/`, with the review question stated as: *an unauthenticated attacker controls every byte of every datagram and can send them at any rate; what can they cause?*
- [ ] Address every **CRITICAL** and **HIGH** finding before starting Task 8. Each fix follows the normal cycle — a failing test in `protocol_test.cpp` naming the specific input, then the fix, then one commit.
- [ ] Record every finding in `docs/project-history.md` under P1, including findings judged not worth fixing and **why** — a dismissed finding with no recorded reason gets re-litigated in P2.
- [ ] Also run the **`cpp-reviewer` agent** over the same surface for RAII and lifetime issues; `UdpTransport`'s fd ownership and `SimulatedTransport`'s reference to its inner transport are the two places a lifetime bug would hide.

---

## Task 8: Freeze the format in writing

The design doc says the wire format is frozen at P1. It is frozen in code by the
golden byte vectors in Tasks 2–3; this task writes it down, so P2 and P3 read a
specification rather than reverse-engineering an encoder.

**Files:**
- Create: `docs/wire-format.md`
- Modify: `CLAUDE.md`, `docs/project-history.md`

**Interfaces:**
- Consumes: everything. Produces no code.

**Checkpoint 1: the format is documented and the conventions are recorded**

- [ ] **Step 1: Write the failing test, then run it**

Run: `test -f docs/wire-format.md && grep -q "0x52495754" docs/wire-format.md && grep -q "little-endian" CLAUDE.md`
Expected: FAIL — exit 1; neither file exists in that state.

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract — `docs/wire-format.md` states:
- the three layout tables (header, `InputCommand`, `WorldSnapshot`) exactly as this plan gives them, with offsets and sizes;
- the magic constant `0x52495754` and the note that its little-endian bytes spell `TWIR` in a hexdump;
- the endianness rule and the `_be`-versus-unsuffixed distinction;
- **which fields are reserved and for what**: `tick`, `send_time_ms`, `ack_tick` for P3's clock sync and RTT estimation; `seq`, `ack_seq` for P2's join/leave reliability channel. Say plainly that P1 transports them and implements none of the logic;
- the maximum packet size derivation: `24 + 8 + 32 × 24 = 800 ≤ 1200`, and that P4's snapshot delta is what buys room above `kMaxPlayers = 32`;
- that every decoder is strict, with the single documented exception of `fire`;
- a pointer to `tests/net/protocol_test.cpp` as the authoritative golden vectors — **the document describes the format, the test defines it**, and a disagreement between them is a bug in the document.

`CLAUDE.md` gains:
- a **File Structure** section — the workflow guide § 3b gates this on "after P1, once `src/` holds more than `src/sim/`", which is now true. Show `src/sim/` and `src/net/` with one line each, plus `scripts/`, `tests/`, `tools/`, `docs/`;
- the two P1 conventions a cold session could otherwise violate: protocol fields are explicitly little-endian and `_be` is reserved for kernel-order values, and `std::bit_cast` is unavailable so float punning goes through `std::memcpy`;
- `docs/wire-format.md` added to the Specs list.

`docs/project-history.md` gains a `## P1 — Wire protocol, serialization, transport`
section replacing the placeholder comment at the end of the file, with one entry
per load-bearing decision: little-endian protocol encoding with the reasoning;
`ByteReader`/`ByteWriter` as the single place offsets are computed; strict framing
(`payload_len` must equal the remaining bytes) and the `fire` exception;
`SimulatedTransport` as a receive-side delay with the round-trip consequence;
`loss_permille` as an integer so no float enters the schedule; `recvmmsg`/`sendmmsg`
deferred to P5 with the reason; the point-to-point limit on `LoopbackTransport`;
the join/leave reliability logic deferred to P2 with the fields reserved now. Plus
any findings from Task 7's security review.

```bash
test -f docs/wire-format.md && grep -q "0x52495754" docs/wire-format.md && \
  grep -q "little-endian" CLAUDE.md && \
  scripts/tw bash scripts/ci.sh && \
  git add docs/wire-format.md CLAUDE.md docs/project-history.md && \
  git commit -m "docs: freeze the wire format and record P1's structure"
```

Expected: PASS — all three configurations plus the toolchain assertions green, then one commit.

**Task boundary:** `scripts/tw bash scripts/ci.sh` — the whole phase, green. **The branch is now green and verified.**

Hand off to `finishing-a-development-branch` for the integration decision. Do not
merge or push from within this plan.

---

## Self-Review

**Spec coverage.** The design doc's P1 row asks for three things. *Wire protocol* —
Task 2 (header) and Task 3 (payloads), with the tick/timestamp/ack fields the row
explicitly demands. *Serialization* — Task 1 (cursors) and Tasks 2–3 (codecs).
*Injectable transport* — Tasks 4–6, with `SimulatedTransport` as the composition
wrapper the resolution doc § Q3 specifies rather than a sibling implementation.
The doc's "the wire format is frozen here" is discharged by the golden byte
vectors (Tasks 2–3) plus `docs/wire-format.md` (Task 8). Resolution doc § Q2's POD
surface lands in Task 3; § Q3's `Endpoint`/`PacketSlot`/`Transport` shapes land
verbatim in Task 4. `CLAUDE.md`'s mandatory network-surface security review is
Task 7's boundary.

**Deferred deliberately, and where to:** the `World` class and all simulation
behavior → P2. The join/leave reliability *logic* → P2 (fields reserved here).
Clock sync, RTT estimation, drift correction → P3 (fields reserved here).
`recvmmsg`/`sendmmsg` batching → P5, where the receiver thread makes batching
measurable. `SpscRing` itself → P5; Task 4's inbox uses the same index discipline
deliberately but is single-threaded and non-atomic, and the plan says so, so it
cannot be mistaken for the lock-free ring the benchmark compares. Multi-peer
loopback routing → P2. `epoll`/`timerfd` → P2's server loop.

**Type consistency.** `ByteWriter`/`ByteReader` (Task 1) are used unchanged by
Tasks 2, 3 and 7. `kMaxPacket` is declared once in `transport.h` and consumed by
Task 2's `payload_len` bound, Task 4's `send` guard, and Task 5's `recvfrom`
length. `Endpoint`, `PacketSlot` and the `Transport` concept (Task 4) are consumed
unchanged by Tasks 5 and 6. `sim::kTickHz` (Task 3, or Task 6 if the transport half
runs first — both tasks say so) feeds `msToTicks`. `sim::kMaxPlayers` bounds both
`WorldSnapshot::players` and Task 3 Checkpoint 2's guard. `encodeInput`/`decodeInput`
and `encodeSnapshot`/`decodeSnapshot` keep those names throughout.

**Checkpoint falsifiability.** Each checkpoint names an assertion that fails
before it and passes after, at the public surface its own test calls: T1C1/C3, all
Task-2 and Task-3 first checkpoints, T4C1, T5C1, T6C1 and T7 fail at the *build*
step on a symbol that does not exist. T1C2 fails on a post-overflow `u8` being
accepted; T1C4 on `remaining()` reporting `1` after a failed read. T2C2 on five
mutated headers being accepted; T2C3 on four length-mismatched packets being
accepted. T3C3 on three over-long payloads being accepted. T4C2 on a second
receive; T4C3 on `inboxSize()` being undeclared then the ring overwriting;
T4C4 on a wrong-address send being delivered. T5C2 on a truncated 1500-byte
datagram being handed back; T5C3 on the fd staying open past its scope. T6C2 on
same-tick delivery; T6C3 on nothing ever being dropped; T6C4 on a strictly
increasing delivery order; T6C5 on `droppedByCapacity()` being undeclared. T8C1
on `test -f`.

**Three checkpoints were removed during review for passing when written** —
short-buffer header rejection, `InputCommand` framing, and the MTU-send guard.
Each is real behavior, but each is a *consequence* of an earlier checkpoint's
contract (`ByteReader`'s sticky `ok()`, the `remaining() == kInputBytes` entry
guard, the `payload.size() > kMaxPacket` guard) rather than a separate RED→GREEN
cycle. Their assertions were folded into the checkpoint that establishes them, so
the coverage is kept and the false cycle is not. Task 7's two checkpoints are the
deliberate exception, flagged inline: their RED is the absence of the test, and
they are worth separate commits because an exhaustive sweep is the artifact a
security reviewer reads.

**One risk worth stating.** Task 5 is the only task that depends on the container
having a working loopback interface. If `bind` to `127.0.0.1` fails inside
`tickwire-dev`, Task 5 stalls while Tasks 1–4 and 6–8 remain executable — the plan
is ordered so that is a detour, not a stop. Record the failure in
`docs/project-history.md` before working around it; a container networking
constraint discovered here would also constrain P2's demo.
