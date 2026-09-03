# Field Trial Design

**Status:** design approved, round one not yet run. No protocol, tool-count, or capability change.

**Purpose:** measure whether an agent that has never heard of Didi can build a working Godot game with it, and capture every place it cannot.

Didi's test suites prove that tools behave as specified. They cannot prove the surface is usable, that the documentation answers the questions an agent actually asks, or that the 91 implemented tools compose into a finished game. A field trial answers those, and it answers them the only honest way: by handing the product to something that does not already know how it works.

---

## 1. Roles

Three roles. Only the middle one is the experiment.

| Role | Who | Responsibility |
| :--- | :--- | :--- |
| **Harness** | Maintainer, before the run | Create the working directory, seed `project.godot`, write the MCP client configuration, record the baseline. |
| **Tester** | A fresh agent session, unattended | Read the briefing, build the game, file issues, keep the ledger. |
| **Review** | Maintainer, after the run | Read the ledger, triage the issues, decide the fixes. |

The tester is unattended by design. The briefing forbids it from asking questions: when blocked it records the block, picks the most reasonable interpretation, and continues. A tester that stops to ask is not a test of anything.

## 2. Cold start

The tester is told three things: the repository is at `D:\didi`, Godot binaries are at their installed paths, and its working directory is `D:\didi-trials\trial-01`. It is not told which document to read.

That is deliberate. Pointing it at `docs/LLM_INSTRUCTIONS.md` would test the capability ceiling and nothing else. Withholding the repository entirely would drown real capability findings in onboarding noise. Handing over the repository without a reading order reproduces the position of someone who has just cloned it, and tests discovery, documentation, and tools in one pass.

The repository is read-only to the tester. It may read every document and source file. It must not write to it and must not run git commands in it.

## 3. Preconditions and seed

Didi exits `2` when `--project` names a directory without a `project.godot`. The working directory must therefore be a valid Godot project before the MCP server can start. That much pre-work is forced by the architecture, not chosen.

Everything past that seed is the tester's job. The harness creates:

1. `D:\didi-trials\trial-01\project.godot` containing only `config_version`, an application name, and the feature list. No addon, no scenes, no autoloads.
2. An MCP client configuration pointing at the built `didi.exe` with `--project` set to the working directory and `--log-level DEBUG`.

The harness does **not** copy `addons/didi`, does not enable the plugin, and does not start the editor. The tester opens on a server where every live tool reports `currentMode: "unavailable"`, and has to work out why. That is where real users fail, it is cheap to test, and it is recoverable: if the run dies there, the finding is recorded and the next round starts warmer.

**Engine:** Godot 4.7.2. It is the newest supported build, and `scene_close` dirty state is documented against 4.5 and 4.6 while 4.7 exposes `get_unsaved_scenes()` that Didi does not yet read. A real build exercises that gap where a synthetic test does not.

**Baseline recorded before the run:** the repository commit, `didi --version`, the Godot build in use, and the tool manifest from `didi --dump-tool-manifest`.

## 4. The build target

A single-screen 2D arena survival slice. The specification is written as game features and never as tool names. Coverage has to fall out of honest work, or the trial only measures whether an agent can tick a checklist.

1. The player is a `CharacterBody2D` with four-way movement bound to **project input actions**, not hardcoded keycodes.
2. The arena is a **TileMapLayer** with solid walls, requiring a TileSet resource that does not exist yet.
3. Three enemies come from a **packed scene**, instanced into the arena, chasing the player, each with an **AnimationPlayer** and at least one animation that plays.
4. Score lives on an **autoload singleton** and reaches a HUD `Control` label by **signal**, not by polling.
5. Clearing all enemies wins, a third hit on the player loses, and both show a screen.
6. **It must actually run.** The tester launches the game, drives the player with injected input, captures frames from both the editor viewport and the running game, and demonstrates the logs are clean.

That set reaches the scene, script, signal, autoload, input map, tilemap, resource, viewport, undo/redo, and runtime clusters, including the game-only tools that never execute when an agent builds a project without ever playing it.

## 5. The Didi-first mandate

Every action is attempted through Didi first, including actions where a direct file edit would obviously be faster.

When a tool fails or cannot do the job, the tester may fall back to editing files directly or driving Godot by hand. The fallback is a recorded event, not a shrug. The mandate exists because an agent given free choice abandons an unfamiliar tool surface early and quietly, and the resulting transcript cannot tell you where it stopped trusting the product.

## 6. The ledger

`TRIAL_LOG.md` in the working directory, append-only, one entry per friction point:

```
## [timestamp] Short title
Intent:    what I was trying to achieve
Attempt:   tool name and exact arguments
Result:    exact response or error
Verdict:   worked | worked-with-friction | failed
Fallback:  what I did instead, or none
Issue:     issue number, or none and why
```

The ledger is expected to be more valuable than the issues. Issues capture defects. The ledger captures friction, and friction decides whether anyone keeps using the product after the defects are fixed.

Separately, the briefing instructs the tester to record architectural decisions and node paths on the blackboard, exactly as [LLM Agent Instructions](LLM_INSTRUCTIONS.md) advises. That exercises the coordination tools through their intended use rather than through a contrived checklist.

## 7. Issue protocol

Filing is deliberately expensive. Before opening anything the tester must:

1. Re-read the relevant section under `docs/`. Behaviour that is documented and wrong is still a finding, under a different label.
2. Search existing issues for a duplicate.
3. Reduce the problem to a minimal reproduction: exact tool, exact arguments, exact response.
4. Classify it. `bug` when behaviour contradicts the documentation, `documentation` when the documentation is wrong or absent, `enhancement` when the capability is simply not there.

One issue per root cause, never one per occurrence. Bodies follow the fields already required by the bug report template. A soft cap of twenty filed issues stops a single systemic fault flooding the tracker; past the cap findings go to the ledger only, and the ledger says the cap was reached.

Every issue carries an additional `field-trial` label. That is the operational requirement: it is how a run's output is reviewed or closed in bulk later without picking through the tracker by hand.

Issues only. No commits, no branches, no pull requests. Issue text is written in the maintainer's own voice, first person, plain sentences, and never attributes the work to a model or a tool.

## 8. Measurement

The ledger is the tester's account of itself and is not trusted for coverage.

The server log cannot supply that account either. `handleRequest` logs `Method: tools/call` at `DEBUG` and the tool name appears only in the `TOOL_EXEC` line written when a call throws, so the log can say how many tool calls happened but not which tools they were. Counting coverage from it would silently under-report every tool that worked.

Coverage therefore comes from the client transcript, where every invocation is recorded as a `tool_use` block named `mcp__didi__<tool>` carrying its arguments. `tools/field-trial/coverage.py` reads that transcript and compares it against the manifest captured at seed time, giving the distinct tools called, the total invocations, and the set of implemented tools that never occurred to the tester at all. That last set is the interesting one.

The server still runs at `--log-level DEBUG`, because the log remains the best record of what the server thought was happening when something failed.

That the tool name is absent from every non-error log line is a real observability gap. It is recorded here rather than fixed, because changing the server to measure the server immediately before testing it is the wrong order.

The run produces five artifacts: the ledger, the coverage report, the captured server log, the working directory tree, and the list of issues filed.

## 9. Out of scope

- No parallel execution. The bench farm in [Gogo Design](GOGO_DESIGN.md) stays design-only and is not a dependency of this trial.
- No 3D. `gridmap_set_cells`, 3D navigation queries, and the 3D camera and raycast paths are a second round with a separate fresh tester.
- No changes to Didi during the run. Fixes are decided at review, after the run ends.
- No CI integration. Round one is run by hand.

## 10. What round one produces

A seed, a briefing, and a scoring method, all versioned under `tools/field-trial/`. Re-running the identical seed after fixes and diffing the two ledgers is the improvement loop, and running it by hand once is the first turn of the crank that [Gogo Design](GOGO_DESIGN.md) eventually automates.
