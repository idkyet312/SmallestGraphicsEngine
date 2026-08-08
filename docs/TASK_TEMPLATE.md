# Task handoff template

Opus writes this; the implementer reads it. Copy to `task.md` in the agent
worktree, fill it in, delete the sections that do not apply.

**Match the detail to the size of the task.** A one-line ask does not need a
design section — over-specifying small work costs more than it saves.

| Size | What Opus writes |
|---|---|
| Tiny — "add a GPU timer around the RT reflection pass" | Just the task line. No document. |
| Medium — "implement ray compaction" | Task + 10-20 lines of design + done criteria. |
| Major — "replace the reflection pipeline with Hi-Z → sparse RT → ReSTIR" | Full document, split into milestones, reviewed at each. |

---

## Task

One sentence. What should exist when this is done.

## Why

The reason this is being built now, and what it unblocks. Keeps the
implementer from optimising the wrong axis.

## Design

Only for medium/major tasks. The approach, not the code. Name the files and
the functions that change. If a specific algorithm was chosen over an
obvious alternative, say which and why — otherwise it will get "improved"
back to the obvious one.

## Constraints

- Default frame stays byte-identical; ship behind a toggle, off by default.
- [Any task-specific invariant: formats, budgets, passes that must not move.]

## Done when

Concrete and checkable, not "it works":

- [ ] `./build.ps1 -Configuration Release -NoRun` clean.
- [ ] `ctest --test-dir build -C Release` — 10/10.
- [ ] [The behaviour that proves the feature: a stat in an expected band, a
      debug view showing the right thing, a parity capture matching.]

## Do not

- [Things that look like reasonable next steps but are out of scope, or
  belong to a later phase. This section prevents most scope creep.]

## Escalate if

Beyond the standing list in [AGENTS.md](../AGENTS.md):

- [Task-specific tripwires — a threshold that comes out wrong, a measurement
  that contradicts the plan's assumption.]

---

## Report back

What landed, what did not, and any measurement the task asked for. State
failures plainly with the output. If something was skipped, say so and why —
scaling the task down is the reviewer's call, not the implementer's.
