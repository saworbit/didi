# Autonomous Pipeline Design

**Status:** design approved. Loop A is built first; Loop B follows. No protocol, tool-count, or capability change.

**Purpose:** turn the manual field trial into a loop that finds Didi's defects, fixes them, and proves the fixes, without spending more than the evidence is worth.

Method and apparatus for a single trial are in [Field Trial Design](FIELD_TRIAL_DESIGN.md). Results are in [Field Trial Results](FIELD_TRIAL_RESULTS.md). This document is about the loop around them.

---

## 1. Two loops, not one

The obvious design runs a trial, fixes the top finding, then re-runs the trial to prove the fix. That design is wrong twice over, and both faults are worth recording so nobody rebuilds it.

**A single trial run cannot gate a change.** Agent runs are stochastic. `pass@k` exists because `pass@1` is unreliable, and unbiased estimators sample repeatedly for the same reason. One run used as a merge gate produces false passes and false failures, and it is the most expensive component in the cycle.

**It also costs the most where it helps least.** Agent evaluation is expensive enough to be a budget line in its own right. The published guidance is to layer: cheap deterministic checks everywhere, expensive checks only where they reduce risk that the cheap ones cannot.

So the work splits by cadence.

| | **Loop A, the fix loop** | **Loop B, the trial** |
| :--- | :--- | :--- |
| Runs | Often, one issue at a time | Rarely, at milestones |
| Cost | One agent run | Three agent runs |
| Gates on | Deterministic checks | Nothing. It measures |
| Output | A draft pull request | A distribution and new issues |
| Answers | Did this change work | Is Didi getting better to build with |

They connect through the issue tracker: Loop B fills the queue, Loop A drains it. That is the continuous-improvement loop, with the expensive half amortised across many fixes instead of paid for each one.

Ten fixes cost ten agent runs plus one three-sample trial, rather than thirty. Cheaper and more statistically sound at the same time.

---

## 2. Loop A: the fix loop

One issue, one agent run, one draft pull request, every gate deterministic.

### Selection

The oldest open issue labelled `agent-ready`. That label is applied by a human and means one specific thing: **the issue carries a minimal reproduction.** It is not a priority marker.

That restriction is the single highest-yield input control. A task with a failing case attached resolves cleanly; a vague one generates debt. It matches what the trials showed directly: #213 and #215 carried exact reproductions and were tractable, while #217 had no reachable mechanism at all and would have burned a cycle proving it.

### Phases

1. **Preflight.** Clean tree, `main` current, build current, Godot present, `claude` present. Any failure stops before spending anything.
2. **Isolate.** A `git worktree` off `main`. The fixer never touches the working checkout.
3. **Fix.** One `claude -p` run in the worktree with the issue body, a fix brief, and `--max-budget-usd` as a hard cost ceiling. `--session-id` is supplied so the transcript path is known rather than guessed.
4. **Gate G1, red then green.** See below.
5. **Gate G2, full verification.** Build, native tests, Python tests, documentation validator, live Godot harness.
6. **Gate G3, diff policy.** See below.
7. **Report.** Draft pull request, cycle artifacts, summary.

### G1: red before green, proven by the pipeline

The fixer must produce a check that fails on the current build. The pipeline runs that check against the pre-fix build and **requires it to fail**, then against the post-fix build and **requires it to pass**. Neither result is taken from the agent's account of itself.

If nothing can be made to fail, the cycle stops at `could_not_reproduce` and never edits engine code.

This gate is not theoretical. During the manual fix of #213 a regression test was written that passed *before* the fix, because it used a Control whose parent was not a Control and so never reproduced the discard. An agent reporting "test added, suite green" would have been telling the truth while shipping a guard that could not catch its own bug. G1 is what catches that, and it is the reason the loop is worth automating at all.

**What G1 does not prove.** It proves that *some* assertion in the patch went from failing to passing. It does not prove that the assertion which flipped is the one the issue describes. A patch that adds one true reproduction alongside several assertions that already passed satisfies the gate on the strength of whichever one was red.

This is not hypothetical either. The fix for #216 passed G1 on an int property refusing a whole-number real, while the float case named in the issue title passes with or without the change — a float property already admitted any JSON number. The change is real and worth having; it is simply not the reported defect. The cycle therefore writes the red run's full output to `gate/red.txt` and puts it in the summary, so a reader can see *which* check was red without auditing the source. Closing the issue remains a human decision, and this is a large part of why.

### G4: the commit carries the patch that was graded

The gates grade the working tree. The pull request carries the index. Those are two different things, and every place they can drift has now drifted at least once:

- Checking `git diff` while committing with `git add -A` shipped ten build artifacts through a policy that never saw them.
- Staging the allowed paths up front did not survive G1: the red run stashes `src`, `include` and `docs`, and `git stash pop` restores the working tree **without restoring the index**. The source half of a fix returned unstaged and the commit carried the tests alone. #223 opened with a new test and nothing for it to pass against, while every gate reported green — honestly, because the tree it graded held the whole change.

So the index is re-staged after G1 and compared against the approved bytes at the moment of the commit, which is the only moment that matters. A mismatch fails the cycle and names the files that came or went. The regression test drives a real repository through the same stash round trip, because the bug was in what git does to the index and no amount of testing our own string handling would have found it.

### G3: the diff policy

Reward hacking in coding agents is documented and specific: editing the test suite, hardcoding expected values, replacing assertions with trivially passing statements. The mitigation is to remove the opportunity.

A patch fails the gate if it:

- modifies or deletes any existing test assertion. The fixer may **add** tests; it may not weaken one.
- touches `.github/workflows/`.
- changes anything outside `src/`, `include/`, `tests/`, `docs/`.
- deletes a file.

G2 additionally runs the suite from a **clean checkout of `main` with the patch applied**, not from the agent's worktree, so a locally doctored test or build artifact cannot influence the result.

### What Loop A never does

Never merges. Never pushes to `main`. Never force-pushes. Never closes an issue. Never retries the agent: a second attempt is a second opinion, not a fix, and it doubles the bill. The pull request is opened as a draft and a human merges it.

A failed cycle keeps its worktree and branch, writes why it failed, and stops.

---

## 3. Loop B: the trial

Three samples of the existing trial at one commit, reported as a distribution rather than a verdict.

Three is a compromise, not a statistically satisfying number. It yields a median and a spread, which is enough to separate a real regression from run-to-run noise, and it is affordable. Its purpose is to find new issues and show a trend, never to gate a change.

Loop B runs at milestones: after a batch of fixes merges, or before a release.

### Metrics

Two trials showed which numbers move and which do not.

**Primary, because they respond to Didi's quality:** friction events per completed feature, fallbacks per completed feature, and features completed. Between trials 01 and 02 these moved from 18 events to 23 while the editor-crash cost disappeared, which is the real signal.

**Secondary, diagnostic only:** tool coverage. It sat at 39.6% then 40.7% across two runs. It is a property of the task, not of Didi, and treating it as a headline invites optimising the wrong thing. Its value is the uncalled set, which is evidence about what agents never reach for.

---

## 4. Components

Each is separately testable, and two already exist.

| Component | State | Responsibility |
| :--- | :--- | :--- |
| `seed_trial.py` | exists | Write the bare project, client config and baseline |
| `coverage.py` | exists | Score a transcript against the seeded manifest |
| `editor.py` | Loop B only | Start Godot, wait for a session descriptor, relaunch on death, stop cleanly |
| `runner.py` | new | Launch one `claude -p` with budget, session id and MCP config; return the transcript path |
| `gates.py` | new | G1, G2 and G3 as pure functions over paths and diffs |
| `cycle.py` | new | Orchestrate Loop A's phases and write the artifacts |

The editor supervisor belongs to Loop B and is not built with Loop A. The fix loop's live verification calls `run_godot_integration.ps1`, which already starts and stops its own editor, so Loop A needs no supervisor at all. Loop B does: trial 01 lost its editor four times, and with #203 fixed a run should survive on one, but a loop that assumes it will is a loop that stalls overnight.

---

## 5. Artifacts and failure handling

`D:\didi-trials\cycle-NNNN\` holds `fix/` (patch and agent transcript), `verify/` (every log), `cycle.json` (machine-readable, so cycles compare) and `CYCLE.md` (what a human reads).

Every phase records its status. A failure stops the cycle, preserves everything, and names what to look at. Only the editor is retried, because its failures are mechanical rather than semantic.

---

## 6. Testing the pipeline

The pure parts, being issue selection, gate evaluation, diff policy and `cycle.json` shaping, are unit-tested in `tests/test_field_trial.py`, which CI already runs.

The two agent launches are stubbed behind `--dry-run`, so the whole orchestration can be exercised without spending a token. That mode is what makes this maintainable: a pipeline that can only be tested by running it is a pipeline nobody will change.

---

## 7. Out of scope

- No scheduled runs. Loop A and Loop B are both invoked deliberately.
- No GitHub Actions. The runner is local; the interface is a callable script, so wrapping it in a workflow later is small.
- No parallel execution. [Gogo Design](GOGO_DESIGN.md) stays design-only.
- No automatic merging or issue closing, ever.
