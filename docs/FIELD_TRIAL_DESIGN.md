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

Coverage therefore comes from the client transcript, where every invocation is recorded along with its arguments. `tools/field-trial/transcripts.py` reads that transcript for whichever client hosted the run, and `tools/field-trial/coverage.py` compares the result against the manifest captured at seed time, giving the distinct tools called, the total invocations, and the set of implemented tools that never occurred to the tester at all. That last set is the interesting one.

The two shapes are not alike. The Claude client files a transcript of message records whose content holds a `tool_use` block named `mcp__didi__<tool>` and, in a later record, the matching `tool_result`; correlating those by id rather than scanning loose is what stops a tester that reads the server log into a `Read` result from manufacturing evidence about a call it never made. `codex exec --json` prints an event per line and delivers a completed MCP call as one record carrying the server, the tool and the result together, with an earlier `item.started` announcing the same call before it has an answer. Counting both events doubles every number in the run.

A call with no recorded answer still counts as an invocation. Coverage asks what the tester reached for, and dropping the answerless ones flatters the uncalled set at exactly the point where the run went wrong.

The server still runs at `--log-level DEBUG`, because the log remains the best record of what the server thought was happening when something failed.

That the tool name is absent from every non-error log line is a real observability gap. It is recorded here rather than fixed, because changing the server to measure the server immediately before testing it is the wrong order.

The run produces five artifacts: the ledger, the coverage report, the captured server log, the working directory tree, and the list of issues filed.

## 9. Out of scope

- No parallel execution. The bench farm in [Gogo Design](GOGO_DESIGN.md) stays design-only and is not a dependency of this trial.
- No 3D. `gridmap_set_cells`, 3D navigation queries, and the 3D camera and raycast paths are a second round with a separate fresh tester.
- No changes to Didi during the run. Fixes are decided at review, after the run ends.
- No CI integration. A trial is stochastic and costs real money, so it cannot gate a pull request; `tools/field-trial/cycle.py` is the cheap deterministic loop that does. Rounds one through three were run by hand; see [Running one unattended](#11-running-one-unattended).

## 10. What round one produces

A seed, a briefing, and a scoring method, all versioned under `tools/field-trial/`. Re-running the identical seed after fixes and diffing the two ledgers is the improvement loop, and running it by hand once is the first turn of the crank that [Gogo Design](GOGO_DESIGN.md) eventually automates.

## 11. Running one unattended

`tools/field-trial/trial.py` is that crank turned by machine. It seeds, briefs a fresh tester in its own client session, and scores what the tester did without asking the tester:

```
python tools/field-trial/trial.py --compare <previous>/coverage.json
```

Five phases, each recorded in `trial.json` and rendered into `TRIAL.md`: **preflight** (the client exists and is signed in, checked before the run costs anything, because the tester reads its own credential store and this machine being signed in says nothing about it), **seed**, **run**, **score**, and **bridge**. `--dry-run` performs the preflight and the seed and launches nothing, which is how the orchestration is exercised without spending.

Three things it does deliberately.

**It scores the bridge, not just the server.** This is the finding trial 03 paid for. A trial is seeded against a server binary and scored against that binary's manifest, while the live half of every call is served by a different file that the tester installs by hand and no artifact recorded. That run spent about an hour concluding a shipped capability did not exist, because the GDExtension answering it was six days older than the server. The seed now records the server's build id, read from `didi --version`, and the SHA-256 of the addon the tester is meant to install alongside the one lying in the repository's own `addons/didi`, which is gitignored, written by no build step, and indistinguishable by eye. After the run, `bridge.py` reads the pairing the server reported on every live session call and returns one of three verdicts. `matched` and `mismatched` are the obvious two. The third is `not_observed`, and it is not a pass: a run in which no call ever reported a pairing does not say which build served it, and its live findings are unaccounted for rather than clean.

**It never trusts the tester's account of itself.** Coverage comes from the transcript, as in section 8, and so does the bridge verdict. The ledger is read by a human afterwards and is not an input to any number here.

**It can hand the seed to a different tester.** `--engine claude` and `--engine codex` run the same seed and the same briefing under different clients, and the summary names which one so nobody has to infer it later. Trials 01 through 03 were all Claude, and across three runs they agreed that an agent reaches for about 40% of the implemented surface. That figure is either a fact about agents or a habit of one client, and there is no way to tell those apart from three runs of the same client. Trial 04 is the first run by another engine and exists to separate them.

Two differences to carry into any comparison. Codex configures MCP servers in `config.toml` rather than from a file, so the seed's `.mcp.json` is translated into `-c` overrides and one seed artifact still describes the server under test for both engines; `--ignore-user-config` is this engine's `--strict-mcp-config`, and it discards the model along with everything else, which is why the model is named on the command line. And Codex has no equivalent of `--max-budget-usd`, so on that engine the timeout is the only ceiling on a run that has stopped finishing. That is worth knowing before walking away from one rather than after.

**It decides nothing.** No fix is applied, no issue is closed, nothing merges. One stochastic run cannot grade a change, and the value is the diff between two runs of the same seed rather than either run alone. The comparison in `TRIAL.md` therefore carries both denominators: trial 03 read as a regression at 32.1% against trial 01's 39.6% purely because the implemented surface grew from 91 to 112 underneath it, and a table showing the percentage without the surface it is a percentage of invites exactly that reading.
