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

**Installed now:** `rules/ecc/common/` + `rules/ecc/cpp/`; the `cpp-coding-standards`
and `cpp-testing` skills; `cpp-reviewer` and `cpp-build-resolver` agents;
`/cpp-build`, `/cpp-test`, `/cpp-review` commands.

**Deliberately not installed** (add when a phase needs them): `docker-patterns`,
`api-design`. Go / React / TypeScript / Postgres / Redis packs are not applicable.

## 4. Implementation (TDD loop)

| Situation | Use |
|---|---|
| **Building a phase (the default)** | `writing-plans` → `executing-plans` |
| Any new feature/bug fix — write-tests-first | `tdd-guide` agent (RED → GREEN → IMPROVE) |
| Fixing a bug — reproduce as a failing test first | `orch-fix-defect` |
| Behavior-preserving refactor | `orch-refine-code` |
| Build/compile errors block progress | `build-fix` skill, or the **`cpp-build-resolver`** agent |
| Removing dead code | `refactor-clean` / `refactor-cleaner` |

## 5. Review (before every merge to `dev`)

| Situation | Use |
|---|---|
| General quality review after writing code | `code-review` skill / `code-reviewer` agent |
| **C++ idiom, RAII, lifetime, move-semantics review** | **`cpp-reviewer` agent** |
| **Any phase touching the network surface** | **`security-reviewer` agent — mandatory.** The UDP deserializer parses untrusted input from an unauthenticated source; a malformed packet reaching a hand-rolled parser is this project's highest-severity surface. |
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
| Scanning for leaked secrets before pushing | `security-scan` skill |

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
| Promoting adapted rules to `~/projects/claude-skills/` | **Not doing it** — user decision, 2026-09-04 | — |

### Skill adaptation record

`writing-plans` here is the **library** version (`~/projects/claude-skills/`) plus
two rules that CallIt invented after that library snapshot was taken and never
promoted back:

1. **2-step checkpoints with `&&` commit chaining** (library still had the 5-step
   form with commit as its own step)
2. **The `## Test Commands` scoping section** (absent from the library entirely)

It also **keeps `Checkpoint falsifiability`**, a self-review check the library has
and CallIt's copy dropped. So this copy is stronger than either source.

One rule was adapted rather than translated: both sources say *"include the flag
that defeats cached results (`-count=1` in Go)."* That doesn't transfer — `go test`
caches, CTest does not — so only the scoping half survives here.
