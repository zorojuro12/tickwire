---
name: writing-plans
description: Use when you have a spec or requirements for a multi-step task, before touching code
---

# Writing Plans

## Overview

Write comprehensive implementation plans assuming the engineer has zero context for our codebase and questionable taste. Document everything they need to know: which files to touch for each task, code, testing, docs they might need to check, how to test it. Give them the whole plan as bite-sized tasks. DRY. YAGNI. TDD. Frequent commits.

Assume they are a skilled developer, but know almost nothing about our toolset or problem domain. Assume they don't know good test design very well.

**Announce at start:** "I'm using the writing-plans skill to create the implementation plan."

**Save plans to:** `docs/plans/YYYY-MM-DD-<feature-name>.md`
- *(Per-project setting. Adjust to wherever the project keeps plans, and say
  so here when you install this into a project.)*

## Scope Check

If the spec covers multiple independent subsystems, it should have been broken into sub-project specs during brainstorming. If it wasn't, suggest breaking this into separate plans — one per subsystem. Each plan should produce working, testable software on its own.

## File Structure

Before defining tasks, map out which files will be created or modified and what each one is responsible for. This is where decomposition decisions get locked in.

- Design units with clear boundaries and well-defined interfaces. Each file should have one clear responsibility.
- You reason best about code you can hold in context at once, and your edits are more reliable when files are focused. Prefer smaller, focused files over large ones that do too much.
- Files that change together should live together. Split by responsibility, not by technical layer.
- In existing codebases, follow established patterns. If the codebase uses large files, don't unilaterally restructure - but if a file you're modifying has grown unwieldy, including a split in the plan is reasonable.

This structure informs the task decomposition. Each task should produce self-contained changes that make sense independently.

## Task Right-Sizing

A task is the smallest unit that carries its own test cycle and is worth a
fresh reviewer's gate. When drawing task boundaries: fold setup,
configuration, scaffolding, and documentation steps into the task whose
deliverable needs them; split only where a reviewer could meaningfully
reject one task while approving its neighbor. Each task ends with an
independently testable deliverable.

## Bite-Sized Task Granularity

**A task may contain multiple checkpoints — one per distinct behavior/case —
each running its own RED→GREEN cycle in two steps:**
- **Step 1** — write the failing test, then run it. Expect FAIL.
- **Step 2** — implement, then verify-and-commit in one chained command.

The commit never needs a step of its own. Chained behind the verification
command with `&&` it becomes free *and* safer: a red test makes the commit
unreachable rather than merely inadvisable.

A task with one straightforward behavior has one checkpoint (one commit). A
task covering several cases gets one checkpoint per case (several commits) —
don't invent checkpoints that aren't real distinctions, and don't collapse
real ones into a single commit either.

**A checkpoint is one RED→GREEN cycle — that's the test for whether it's real.**
If you can't write a test that fails *before* the implementation exists, it
isn't a checkpoint: fold those test cases into the checkpoint that implements
the behavior they cover. A checkpoint whose test passes the moment it's written
is the signal that granularity has been pushed one notch past where the cycle
actually divides.

This matters beyond tidiness. Every checkpoint's Step 1 says "expect FAIL", so
a checkpoint that expects PASS contradicts its own template — and inline
execution requires stopping on any mismatch between an instruction and reality.
A cold executor hits that, halts, and may "fix" a correct test until it fails.
(Observed in practice: four such checkpoints in one plan — regression pins for
behavior an earlier checkpoint's implementation already satisfied.)

**Name the observable signal, at the interface the test actually calls.** A
checkpoint can also fail to RED for a second, unrelated reason: the behavior it
specifies is real, but the tested interface can't *see* it. Before writing a
checkpoint, answer — what value, error, or side effect changes at the public
surface this checkpoint's test calls? Not "the script sets status X internally,"
but "the wrapper returns `ErrAlreadyLocked`." If no such signal can be named,
the checkpoint is unfalsifiable by construction, and no reordering fixes it.

Two ways out, both decided while writing the plan rather than discovered
mid-execution:

1. **The distinction doesn't matter to callers** → merge the checkpoint into a
   neighbor that does have an observable delta.
2. **The distinction does matter** → make "extend the interface to surface this
   case" its own earlier checkpoint, then checkpoint the behavior against it.

This is the failure mode of any layer whose lower level has more states than its
wrapper exposes — a wrapper over a script, a client over a protocol, an ORM over
a stored procedure. (Observed in practice: a lock routine's `ALREADY_LOCKED`
case was black-box indistinguishable from its unconditional-OK predecessor at
the wrapper's return type. Cost a full unwind — revert the implementation,
re-run the test to prove it still passed, then recombine three planned
checkpoints into one commit.)

## Plan Document Header

**Every plan MUST start with this header:**

```markdown
# [Feature Name] Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use the `executing-plans` skill to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** [One sentence describing what this builds]

**Architecture:** [2-3 sentences about approach]

**Tech Stack:** [Key technologies/libraries]

**Spec:** [path to the spec/design doc this plan implements — the plan
argues from the spec, so the spec travels with it; executors read both]

## Global Constraints

[The spec's project-wide requirements — version floors, dependency limits,
naming and copy rules, platform requirements — one line each, with exact
values copied verbatim from the spec. Every task's requirements implicitly
include this section.]

---
```

## Task Structure

**Pick the format to match the execution mode — they are not interchangeable.**

- **Subagent-driven execution** (a fresh, cold-context subagent per task, seeing
  only its own task): **pre-write full test and implementation code.** The
  subagent transcribes and verifies rather than re-deriving, which is what lets
  a cheaper model execute reliably. This is upstream's default template.
- **Inline execution** (the same context that wrote the plan executes it):
  **specify the exact behavior precisely; let execution write the code.**
  Pre-writing full code buys nothing here — the executor derives essentially the
  same code either way — and it inflates plans badly. (Measured on one phase: 8
  tasks, 35 checkpoints, 61 code blocks, ~88 lines per checkpoint, 3000+ lines
  total. The spec-driven rewrite of a *larger* phase came to 1,591 lines with no
  precision lost.)

The template below is the **inline/spec-driven** form. If tasks go to subagents,
use upstream's code-heavy form for those tasks instead.

````markdown
### Task N: [Component Name]

**Files:**
- Create: `exact/path/to/file.py`
- Modify: `exact/path/to/existing.py:123-145`
- Test: `tests/exact/path/to/test.py`

**Interfaces:**
- Consumes: [what this task uses from earlier tasks — exact signatures]
- Produces: [what later tasks rely on — exact function names, parameter
  and return types. Kept exact regardless of execution mode: this is what
  keeps cross-task types consistent, e.g. not `clearLayers()` in Task 3 vs
  `clearFullLayers()` in Task 7.]

**Checkpoint 1: [specific behavior or case this checkpoint covers]**

- [ ] **Step 1: Write the failing test, then run it**

Spec: [exact input(s) → exact expected output or error, stated precisely
enough that two different implementers would write the same test — e.g.
"input `[]`, pool empty → returns `ErrEmptyPool`", not "handle the empty
case." Show a code block only if a subtle assertion detail needs pinning
down (a specific float tolerance, an exact error message string) — not by
default.]

Run: [exact scoped command — see Test Commands below]
Expected: FAIL with [exact expected failure reason]

- [ ] **Step 2: Implement, then verify-and-commit in one command**

Contract: [the behavior in 1-2 lines, using the exact signature from
Interfaces above. Not a function body — the executor writes that against
this contract and the test from Step 1.]

```bash
[exact scoped test command] && \
  git add [exact paths] && \
  git commit -m "[exact type: description]"
```

Expected: PASS, then one commit.

**Checkpoint 2: [next behavior or case, if this task has one]**

[Same two steps. Omit Checkpoint 2+ entirely when the task genuinely has only
one behavior — a single checkpoint is a complete, valid task, not a truncated
one.]
````

Chain the commit with `&&`, never `;` and never separate lines — the commit
must be unreachable when the test fails. `git add` names exact paths; never
`git add -A` or `git add .`.

## Test Commands

- **Inside a checkpoint:** scope to the target or test under test
  (`ctest --test-dir build -R <regex>`), and to the single test when the
  target is slow.
- **At a task boundary:** the full suite once, chained into one call.
- **Never** put the full-suite command inside a checkpoint. Under this
  project's three sanitizer configurations a full run is expensive; a phase
  with 30 checkpoints that runs it at every one wastes hours.
- Unlike `go test`, **CTest does not cache results**, so no cache-defeating
  flag is needed — scoping is the whole of the rule here.

## No Placeholders

Every step must contain the actual content an engineer needs. These are **plan failures** — never write them:
- "TBD", "TODO", "implement later", "fill in details"
- "Add appropriate error handling" / "add validation" / "handle edge cases"
- "Write tests for the above" (without an exact input→output/error spec)
- "Similar to Task N" (repeat the full spec — the executor may work tasks out of order)
- "Handle the edge case" without saying which edge case and what the exact
  expected behavior is
- References to types, functions, or methods not defined in any task's
  Interfaces block

**Under spec-driven (inline) execution, a precise behavior spec satisfies this
rule — full code is not required** (see Task Structure above). The bar is: could
two different implementers, given only this step, write the same test and the
same implementation? If yes, it's specific enough. "Reject values below the
minimum" fails that bar (which values? what happens instead?); "input `50`,
minimum `100` → returns `ErrBelowMinimum`, balance untouched" passes it.

## Self-Review

After writing the complete plan, look at the spec with fresh eyes and check the plan against it. This is a checklist you run yourself — not a subagent dispatch.

**1. Spec coverage:** Skim each section/requirement in the spec. Can you point to a task that implements it? List any gaps.

**2. Placeholder scan:** Search your plan for red flags — any of the patterns from the "No Placeholders" section above. Fix them.

**3. Type consistency:** Do the types, method signatures, and property names you used in later tasks match what you defined in earlier tasks? A function called `clearLayers()` in Task 3 but `clearFullLayers()` in Task 7 is a bug.

**4. Checkpoint falsifiability:** For each checkpoint, can you name the test
assertion that fails before it and passes after, using only the public surface
that checkpoint's test calls? Merge or restructure any checkpoint that can't
answer.

If you find issues, fix them inline. No need to re-review — just fix and move on. If you find a spec requirement with no task, add the task.

## Where a Plan Stops

**Don't write merge, push, or PR steps into a plan.** `executing-plans` hands
off to `finishing-a-development-branch`, which verifies tests and presents the
merge/PR/keep menu — it owns integration, and deliberately keeps that decision
with the user. A plan whose final task also merges gives the executor two paths
for one merge, and pre-empts a choice that isn't the plan's to make.

A plan's final task ends at **"branch is green and verified."** Project-specific
wrap-up that neither skill covers — amending a parent plan, recording a
convention's outcome, writing a journal entry — belongs in that final task. The
merge does not.

## Execution Handoff

Before handing off, **commit the plan to the integration branch.** A plan
executed in a different session is untracked at handoff, and the executing
session's first act is to cut a feature branch — so an uncommitted plan follows
onto that branch and lands in the feature's history as though it were feature
work. Commit any other stray files at the same time, so the executor starts from
a clean tree.

Then report the plan's path and confirm before executing:

**"Plan complete and saved to `<path>`. Review it, then I'll execute inline with
the `executing-plans` skill — task by task, committing at each task boundary,
stopping if I hit a blocker. Ready?"**

- **REQUIRED SUB-SKILL:** Use the `executing-plans` skill
- Inline execution in this session, with checkpoints for review

**Executing in a separate session** — including a different model — is equally
valid and needs no conversation history, provided the project's `CLAUDE.md`
auto-loads and the plan carries its own Global Constraints plus any amendments
it makes to a parent plan or spec.

(Upstream also offers a subagent-driven mode — a fresh subagent per task with
two-stage review. If you adopt it, switch those tasks to the code-heavy template
per Task Structure above.)
