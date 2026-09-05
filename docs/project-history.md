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

<!-- Next section: ## P1 — Wire protocol, serialization, transport -->
