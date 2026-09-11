# Tickwire — Dev Workflow Guide: What to Reach For, When

Adapted from CallIt's guide (`~/projects/call_it/docs/dev-workflow-guide.md`)
using that guide's own migration test:

> **Did we invent a rule, or set a value? Rules travel; values don't.**
> **Verdicts are project-specific; the reasoning is portable.**

So the rules below are carried; every verdict was re-derived against Tickwire's
conditions rather than inherited. Where a verdict happens to match CallIt's, it
matches because the conditions match — solo developer, no collaborators — not
because it was copied.

Project design doc: [`docs/specs/2026-09-04-tickwire-design.md`](specs/2026-09-04-tickwire-design.md)

---

## 1. Design & spec work

| Situation | Use |
|---|---|
| Open-ended exploration of an approach | `brainstorming` skill — writes to `docs/specs/` |
| Recording a decision that closes off alternatives | `architecture-decision-records` skill |
| Understanding an unfamiliar area before changing it | `code-explorer` agent / `codebase-onboarding` |

## 2. Turning a spec into an implementation plan

**Two layers, don't confuse them.** `/impl-plan` is the *architectural* layer: run
once per project (or per major subsystem) to resolve open design questions and
produce the phase table. `writing-plans` is the *execution* layer: run once per
phase, right before that phase's branch starts, to break one phase into numbered
tasks with exact files, interfaces, and test → implement → verify → commit steps.
**Don't re-run `/impl-plan` per phase.**

> **Status: `/impl-plan` has NOT been run for Tickwire.** The P0–P7 table currently
> exists only as a table inside the design doc. It needs to become a real
> implementation plan that resolves the open architectural questions before P0's
> phase plan is written — `libsim`'s exact API surface and link model, fixed-point
> vs floats, the transport interface shape, and the queue's memory-ordering
> contract. Those are architecture, not execution, and answering them inside a
> phase plan is how they get answered badly.

Plans live in **`docs/plans/`**, named `YYYY-MM-DD-phase-N-slug.md`.

| Situation | Use |
|---|---|
| Spec is approved, need the phase table and the open architectural questions resolved | `/impl-plan` command — restates requirements, assesses risk, produces a step-by-step plan, **waits for CONFIRM before touching code** (renamed from `/plan` to avoid colliding with Claude Code's built-in Plan Mode) |
| About to start a phase from the plan's phase table, need it broken into committable tasks | `writing-plans` skill → saves to `docs/plans/` → hands off to `executing-plans` |
| Building a standalone feature that was never one of the phases | `writing-plans` directly — no need to route through `/impl-plan` unless it raises genuinely new architectural questions |
| Plan needs deeper multi-file architectural reasoning first | `planner` agent, or `code-architect` once there's code to pattern-match against |
| End-to-end gated build as one wrapped flow, no standalone plan artifact wanted | the `orch-*` commands — the alternative to `writing-plans` when a reviewable plan isn't needed |

**Undecided, deliberately:** the spec-driven vs code-driven plan format. CallIt
decided spec-driven because it executes inline. Tickwire has `delegating-plan-tasks`
installed, and delegated tasks want pre-written code — so this is a per-plan fork,
not a project-wide default. `writing-plans` states it as a fork; keep it that way
until a phase forces the choice.

### 2a. The two-model loop: plan in Opus, execute in Sonnet

| Window | Model | Does |
|---|---|---|
| Planning | Opus | `writing-plans` — resolves open questions, argues amendments, produces the phase plan |
| Executing | Sonnet | `executing-plans` — works the plan task by task, commits per checkpoint |

**The executing window needs no conversation history.** This is a constraint on the
*plan*, not a hope about the executor: everything it needs must be written into the
plan itself — Global Constraints, environment gotchas, and any amendment to the
spec. If a plan can only be executed by someone who watched it being written, it
isn't finished.

**This matters more here than it did in CallIt.** Tickwire is being built while
learning C++, so the executor will be leaned on harder and has less shared context
to fall back on.

**Mechanics:**

1. **Commit the plan to `dev` before handing off.** The executing window's first act
   is `git checkout -b <slug> dev`; an uncommitted plan follows onto the feature
   branch and lands in that phase's history as though it were phase work.
2. Point the Sonnet window at the plan path. **Don't create the branch by hand** —
   `executing-plans` Step 1 does it.
3. Expect questions before it starts; Step 1 requires a critical review pass. That's
   the skill working, not a defect in the plan.
4. **Only one window edits `.claude/skills/` at a time.** Both sessions reach the
   same files and won't see each other. (CallIt lost work to this on 2026-08-23.)

## 3. Setting project-level conventions

| Situation | Use |
|---|---|
| Writing Tickwire's `CLAUDE.md` | **After P0 exists**, not from the spec — writing it before real code means documenting guesses. Run `/project-init` for a verified, command-checked scaffold, then layer in invariants manually. |
| Installing stack-specific rule packs | Copy from `/home/chikara/projects/ECC/rules/<pack>` into `.claude/rules/ecc/`. **Stagger per-phase** — rule dirs load as always-on full text into every turn, unlike skills. |
| Pulling in stack-specific skills | Cheap (one line in the listing until invoked) — install eagerly, project-locally. |

**Installed now (as of P0):** `rules/ecc/common/` + `rules/ecc/cpp/`; the 9 Bucket-1
staple skills (`agentic-engineering`, `ai-first-engineering`,
`architecture-decision-records`, `codebase-onboarding`, `continuous-learning-v2`,
`error-handling`, `git-workflow`, `security-review`, `verification-loop`) plus
`brainstorming`, `writing-plans`/`executing-plans`, `delegating-plan-tasks`,
`dispatching-parallel-agents`, `finishing-a-development-branch`, `journal`,
`using-git-worktrees`, `writing-skills`, and the C++ pack (`cpp-coding-standards`,
`cpp-testing`); the 15 Bucket-1 staple agents plus `cpp-reviewer` and
`cpp-build-resolver`; the 14 Bucket-1 staple commands plus `/cpp-build`,
`/cpp-test`, `/cpp-review`, `/project-init`.

### 3a. Phase-by-phase import map

Cross-referenced against `~/projects/ecc-survey.md`'s three-bucket skill/agent/
command inventory and the P0–P7 table in the design doc. Lists only what's **not**
already installed, so this table shrinks over time rather than restating 3's
"installed now" list.

| Phase | Not yet imported | Import trigger |
|---|---|---|
| P1 — wire protocol, serialization, transport | Nothing new. `security-review` (already installed) is the applicable skill the moment untrusted-input parsing exists — no ECC skill covers binary-protocol design specifically. | — |
| P2 — authoritative server, raylib client, first demo | `docker-patterns` skill | Once the server is packaged as a container for the demo/README, not before — P0's `Dockerfile` is a *dev toolchain* image, not the shipped artifact, so this wasn't a P0 need. |
| P2 — raylib integration specifically | `documentation-lookup` skill + `docs-lookup` agent (Context7 MCP) | First external-library integration in the project (raylib). Pull in when starting that work, not before — nothing to look up yet. |
| P3 — prediction/reconciliation/clock-sync | Nothing available. No ECC skill covers netcode-specific prediction/reconciliation; the design doc already names the correct references (Gaffer On Games, Valve Source Multiplayer Networking) instead of a packaged skill. | — (confirmed gap, not an oversight) |
| P4 — entity interpolation, snapshot delta | Nothing available, same reason as P3. | — |
| P5 — threading + lock-free SPSC queue benchmark, headline numbers table | **`benchmark`, `benchmark-methodology`, `benchmark-optimization-loop` skills** (Bucket 3, meta/orchestration) | Start of P5 — this is the clearest concrete gap in the whole map. P5's entire purpose is producing the tick-jitter/throughput numbers table; `performance-optimizer` (already installed) profiles, but these three skills are what structure the benchmark methodology and its optimization loop. |
| P6 — lag compensation | Nothing available — domain-specific, external references again. | — |
| P7 — stretch (io_uring / WebSocket gateway) | Nothing available — no ECC skill for io_uring or raw WebSocket protocol work. | — |
| Any phase, once `.claude/` surface has grown | `security-scan` **skill** (distinct from the already-installed `/security-scan` **command** — the skill audits `.claude/` config itself for misconfig/leaked secrets; the command audits code) | Optional hygiene layer, not urgent — worth adding once the agent/skill/command surface is large enough that a misconfiguration would be easy to miss by eye. |

**Confirmed not applicable — don't re-litigate these:** `api-design` (no REST
surface, raw UDP), `e2e-testing`/`e2e-runner` (Playwright, browser-only — the
client is native raylib), `accessibility` (no web UI), `design-system` (no web
UI), `kubernetes-patterns` and `deployment-patterns` (design doc: "Live UDP
hosting unsupported on most PaaS — Accepted. Local demo plus recorded video."),
`database-reviewer`/`a11y-architect` (no DB, no web UI). Go / React / TypeScript
/ Postgres / Redis rule packs are likewise not applicable to this stack.

**Optional, not phase-gated:** `cost-tracking` skill (Claude Code spend
monitoring — a personal-workflow choice, not a project need); `/loop-start`/
`/loop-status` (only if long-running benchmark or CI-watch loops at P5 make
autonomous looping worth it).

### 3b. `CLAUDE.md` shape — findings from ECC's example templates

Surveyed `/home/chikara/projects/ECC/examples/*.md` (9 English-language project
templates: generic, Next.js/SaaS, Django, Rust API, Go microservice, Rails,
Laravel, HarmonyOS, plus a user-level example) and ECC's own real-world
`CLAUDE.md`. None targets C++ or a game/netcode stack — Tickwire has no
template to crib from directly, which matches the "write it after P0 exists"
decision above being self-originated rather than copied.

**Consistent shape across every project-level example:** Project Overview →
(often) Prompt Defense Baseline → Critical Rules (language conventions, error
handling, code style) → File Structure → Key Patterns (short code snippets per
architecture layer — handler/service/repository in the Rust and Go examples) →
Environment Variables → Testing Strategy → ECC Workflow (a table mapping
lifecycle stages to slash commands) → Git Workflow.

**What Tickwire's current `CLAUDE.md` has vs. the template shape:** it has
Critical Rules (build/container/float-flag/language/interface-boundary
constraints), a condensed Testing section, and a condensed Git section — all
*verified*, per the P0 plan's mandate. It deliberately omits Project Overview,
File Structure, Key Patterns, Environment Variables, and an ECC Workflow
command table, because none of those existed as verified reality at the end of
P0 (one `.cpp`/`.h` pair, no architecture layers, no env vars, no
phase-specific command usage pattern yet).

**When to add each, following the same "verified reality only" rule that
governed writing it in the first place:**

| Section | Add when | Not before, because |
|---|---|---|
| Project Overview | Any time — low risk, it's a one-paragraph restatement of the design doc's opening | — |
| ECC Workflow (command table) | Any time — commands are already installed and stable | — |
| File Structure | After P1 — once `src/` holds more than `src/sim/` (transport, protocol/serialization land) | Before P1 it's just `src/sim/`, not worth a tree diagram |
| Key Patterns | After P2 — once there's a real client/server architectural split to show a snippet of | P0/P1 have no layered architecture yet to pattern-match |
| Environment Variables | Only if/when actual env-driven config appears (e.g. server port, log level) — plausibly P2 | Nothing reads an env var yet; `TW_SANITIZER` is a CMake cache var, not one |

**Prompt Defense Baseline:** present in every generic/meta example (including
ECC's own `CLAUDE.md`) but absent from Tickwire's. It's boilerplate against
prompt injection from untrusted repo content or external contributors — lower
priority for a solo, local-only project than for a multi-contributor plugin
repo like ECC itself. **Revisit if the repo ever goes public with external
issues/PRs**, not before.

## 4. Implementation (TDD loop)

| Situation | Use |
|---|---|
| **Building a phase (the default)** | `writing-plans` → `executing-plans` |
| Any new feature/bug fix — write-tests-first | `tdd-guide` agent (RED → GREEN → IMPROVE) |
| Fixing a bug — reproduce as a failing test first | `/orch-fix-defect` command |
| Behavior-preserving refactor | `/orch-refine-code` command |
| Build/compile errors block progress | `/build-fix` command, or the **`cpp-build-resolver`** agent |
| Removing dead code | `/refactor-clean` command / `refactor-cleaner` agent |

## 5. Review (before every merge to `dev`)

| Situation | Use |
|---|---|
| General quality review after writing code | `/code-review` command / `code-reviewer` agent |
| **C++ idiom, RAII, lifetime, move-semantics review** | **`cpp-reviewer` agent** |
| **Any phase touching the network surface** | **`security-review` skill and/or the `security-reviewer` agent — mandatory.** The UDP deserializer parses untrusted input from an unauthenticated source; a malformed packet reaching a hand-rolled parser is this project's highest-severity surface. |
| Swallowed errors / bad fallbacks | `silent-failure-hunter` agent |
| Type/struct design enforcing invariants | `type-design-analyzer` agent |
| Comment accuracy/rot | `comment-analyzer` agent |

## 6. Testing & coverage

**80% minimum project-wide** (`.claude/rules/ecc/common/testing.md`).

**`libsim`'s floor is 100%, not 80%.** Carried directly from CallIt's rule for
`internal/domain`: there is no wiring code in a pure, no-I/O simulation core to
excuse a gap, and it's the place correctness bugs hide. Same rule, different target.

**Sanitizers are CI gates, not diagnostics.** ASan/UBSan and TSan configurations run
the full suite; a phase does not merge with any of them red. This is the direct
analogue of CallIt's `-race` discipline.

**Test command scoping** (see `writing-plans` § Test Commands):
- inside a checkpoint → `ctest --test-dir build -R <regex>`, scoped
- at a task boundary → full suite, once
- **never** the full suite inside a checkpoint — under three sanitizer configs that
  wastes hours for no additional signal

Unlike `go test`, **CTest does not cache results**, so no cache-defeating flag is
needed.

## 7. Docs & knowledge capture

| Situation | Use |
|---|---|
| End-of-session log | `journal` skill, or hand-written — decide per session |
| A decision got made, reversed, or something surprised us running the toolchain | [`docs/project-history.md`](project-history.md) — the skimmable cross-phase timeline. Distinct from the journal (session narrative) and the specs (full rationale) — this is the short version, one entry per load-bearing event, linking back to the source doc instead of restating it. |
| Updating README/codemaps | `doc-updater` agent |

## 8. Git & shipping

**Branch-per-phase, no PR ceremony.** Re-derived, not inherited: still a solo
project, so PR ceremony buys nothing. **Revisit immediately if a collaborator joins.**

Branch off `dev` before starting a phase (`git checkout -b p3-prediction dev`).
Commit incrementally as each checkpoint reaches GREEN — **not** one squashed commit
at the end. Merge into `dev` with `--no-ff` once the phase's tests pass, so the
phase stays a visible unit; delete the branch.

Sub-task-level branching was considered and rejected as overhead for solo work.

| Situation | Use |
|---|---|
| Starting a phase | `git checkout -b <phase-slug> dev` — `executing-plans` Step 1 does this |
| Committing | `type: description` per `.claude/rules/ecc/common/git-workflow.md`, one commit per checkpoint |
| Finishing a phase | `finishing-a-development-branch` skill |
| Opening a PR | Not used — solo, self-merge |
| Scanning for leaked secrets before pushing | `/security-scan` command |

**One commit per checkpoint, chained behind its test with `&&`** so a red test makes
the commit unreachable rather than merely inadvisable. Never `;`, never separate
lines. `git add` names exact paths — never `git add -A`.

## 9. Decisions & tradeoffs

Re-derived against Tickwire's conditions. **Reasoning is portable; verdicts are not
— re-run these if conditions change.**

| Decision | Verdict | Revisit when |
|---|---|---|
| Branch granularity | Per phase, not per sub-task | A phase stops being reviewable as one unit |
| PR vs self-merge | Self-merge to `dev`, no PR | A collaborator joins |
| Rule packs vs skills import timing | Skills eagerly; rule dirs staggered per-phase | A rule pack is small enough that staggering costs more attention than it saves |
| `CLAUDE.md` timing | After P0, not from the spec | Never |
| Spec-driven vs code-driven plans | **Undecided — per-plan fork** | A phase delegates tasks; delegated tasks want pre-written code |
| `subagent-driven-development` | Declined — the objection is its ceremony, not delegation. `delegating-plan-tasks` covers delegation without it | The ceremony starts paying for itself |
| `continuous-learning-v2` | Installed but **dormant** | Re-read CallIt's guide §9 before enabling |
| Promoting adapted rules to `~/projects/claude-skills/` | **Not doing it** — user decision, 2026-09-04 | A rule is provably lost translating into a *new* project. Happened once already (2026-09-11, the false-green rule below) — but it was lost translating *out of* CallIt, which promotion would not have prevented, so the verdict stands. Re-run it if a rule is lost translating out of the **library** itself |

### Skill adaptation record

`writing-plans` here is the **library** version (`~/projects/claude-skills/`)
plus rules CallIt invented after that library snapshot was taken and never
promoted back, plus one this project earned.

**Record each rule by the hazard it prevents, not by its wording.** This is the
form, and it exists because the previous form failed: the `-count=1` entry below
used to read *"doesn't transfer — `go test` caches, CTest does not — so only the
scoping half survives."* That is accurate, and the rule was still lost. Stated as
a wording diff, "caching doesn't apply here" ends the thought. Stated as a
hazard — *the runner reports green without having run the new test* — it forces
the next question, "then what is **this** runner's route to a false green?", and
the answer (a stale binary, because `ctest` does not build) is a rule worth as
much as the original. Carrying these to a future project means re-deriving the
**Prevents** column against that stack, not editing the **Rule** column.

| Rule | Prevents (the hazard) | Translating it elsewhere |
|---|---|---|
| 2-step checkpoints, commit chained with `&&` | A commit landing on a red test. As its own step a commit after a failure is merely inadvisable; chained, it is unreachable | Any VCS and runner. Chain, never sequence — `;` and separate lines both lose the property |
| `## Test Commands` scoping | Hours burned running a full suite inside every one of ~35 checkpoints | Whatever the runner's scoping flag is (`-R`, `-k`, a package path) |
| **No false green** — always `cmake --build` before `ctest` | The runner reporting PASS without ever executing the newly written test. Two known mechanisms: a **cached** result (Go — hence `-count=1`), and a **stale binary** because the runner does not build (CTest). A third, CTest-specific: `ctest -R` matching no target at all still exits 0 | Ask what this runner's route to a false green is. Enumerate caching, stale build artifacts, and no-match-exits-zero before concluding there isn't one |
| Keeps `Checkpoint falsifiability` (library has it; CallIt's copy dropped it) | Checkpoints that are unfalsifiable by construction, which contradict their own "expect FAIL" step and stall a cold executor | Universal — it is about test design, not tooling |

The false-green rule was found on 2026-09-11 while writing the P4 plan, after
the P3 plan had used a bare `ctest` at all 35 of its checkpoints while asserting
"Expected: FAIL — compile error", an outcome bare `ctest` cannot produce. P3's
committed code was verified green against freshly built binaries, so nothing was
actually broken — what was lost was the evidence that its red steps were ever
red. The rule now lives in `CLAUDE.md`'s Build and test section (so every session
gets it, not just plan-writing ones) and in this project's copy of the skill.
