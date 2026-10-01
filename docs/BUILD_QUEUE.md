# Build Queue

The order in which Didi's next capabilities are built, and the reason for each.
Status lives here and nowhere else. [ROADMAP.md](ROADMAP.md) groups the same
work into phases by theme; this page says what happens next.

The reasoning behind the order is in [Design Principles](DESIGN_PRINCIPLES.md).
Each item below names the principles it serves.

## How to pick the next item

1. Open `P0` and `P1` issues come first, as [Issue severity](../ISSUE_SEVERITY.md)
   describes. The queue is for capabilities.
2. Otherwise, the next item is the **first row whose status is `PLANNED` and
   whose dependencies are all `COMPLETE`**. A row that is `IN PROGRESS` already
   has someone on it.
3. Read the item's section below, and the principles it cites.
4. If the row has no issue, open one labelled `roadmap` with a `P` label, and
   put its number in the row.
5. If the item adds a tool name, its first change is an entry in
   [Surface Amendments](SURFACE_AMENDMENTS.md).
6. The pull request that starts the work sets the row to `IN PROGRESS`. The
   pull request that meets the item's **Done when** sets it to
   `COMPLETE (#<pull request>)`, closes the issue, and updates the phase status
   in both [ROADMAP.md](ROADMAP.md) and
   [FUTURE_PHASES_DESIGN.md](FUTURE_PHASES_DESIGN.md) if the phase moved.

`tools/validate_documentation.py` checks the table: items numbered in order,
known statuses, a pull request on every `COMPLETE` row, dependencies that
exist, phases the roadmap declares, and the four fields in every item. It also
fails when a phase's status differs between the roadmap and the phase design.

## Queue

<!-- build-queue:start -->
| Item | Capability | Phase | Depends on | Issue | Status |
| --- | --- | --- | --- | --- | --- |
| [Q1](#q1-type-every-argument) | Type every argument | 13 | none | #1000 | COMPLETE (#1015) |
| [Q2](#q2-observed-not-asserted) | Observed, not asserted | 13 | none | #1010 | COMPLETE (#1021) |
| [Q3](#q3-contract-snapshots) | Contract snapshots | 13 | none | #1011 | COMPLETE (#1024) |
| [Q4](#q4-shrink-the-tool-list) | Shrink the tool list | 13 | Q3 | #1012 | IN PROGRESS |
| [Q5](#q5-response-economy) | Response economy | 13 | Q3 | #776 | COMPLETE (#1039) |
| [Q6](#q6-next-step-in-every-answer) | Next step in every answer | 13 | none | #1040 | IN PROGRESS |
| [Q7](#q7-typed-object-layer) | Typed object layer | 14 | Q2 | not yet | PLANNED |
| [Q8](#q8-long-work-as-jobs) | Long work as jobs | 11 | none | not yet | PLANNED |
| [Q9](#q9-proof-in-one-call) | Proof in one call | 14 | Q8 | not yet | PLANNED |
| [Q10](#q10-graph-builders-that-round-trip) | Graph builders that round-trip | 9 | Q7 | not yet | PLANNED |
| [Q11](#q11-godots-own-debugger-and-language-server) | Godot's own debugger and language server | 7, 8 | none | not yet | PLANNED |
| [Q12](#q12-didi-setup-and-didi-doctor) | didi setup and didi doctor | 12 | none | not yet | PLANNED |
| [Q13](#q13-project-defined-tools) | Project-defined tools | 12 | Q7, Q8 | not yet | PLANNED |
| [Q14](#q14-a-skill-pack-measured) | A skill pack, measured | 12 | Q12 | not yet | PLANNED |
| [Q15](#q15-change-journal-with-undo) | Change journal with undo | 13 | Q2 | not yet | PLANNED |
| [Q16](#q16-performance-verdicts) | Performance verdicts | 14 | Q9 | not yet | PLANNED |
<!-- build-queue:end -->

The first six items make the surface honest and cheap before it grows. Items 7
to 12 widen what an agent can reach and prove. Items 13 to 16 open the surface
to projects and to the person reviewing the agent's work.

## Items

### Q1. Type every argument

**Principles:** [P2](DESIGN_PRINCIPLES.md#p2-reject-what-cannot-be-interpreted).

**Why:** A client fills a gap in a schema its own way. A Claude host sends any
argument with no declared JSON type as a string, so `project_set_setting` cannot
write an int, a bool or an array, and offline the documented addon bootstrap
writes a quoted string and reports success (#1000, trial 06).
`blackboard_write.value` and `blackboard_task_complete.artifacts` have the same
gap.

**What:** Every top-level argument of every tool declares a JSON type or a list
of types. The three known gaps are closed.

**How:** Declare the type list each argument's values can take. Add a check,
driven by `didi --dump-tool-manifest` or the native schema tests, that fails on
any top-level property without `type`, so no later tool can reintroduce the gap.

**Done when:** The check runs in a required CI job, and a Claude host writes an
int, a bool and an array setting in the live harness.

**Surface amendment:** Not needed.

### Q2. Observed, not asserted

**Principles:** [P1](DESIGN_PRINCIPLES.md#p1-success-means-the-change-was-observed),
[P3](DESIGN_PRINCIPLES.md#p3-one-write-path).

**Why:** False success has been the most serious defect class in the trials:
#213, #215, #217, #327, #330 and #374. Newer tools return the observed
post-state. Nothing makes the older ones do it, and nothing stops a new tool
from regressing.

**What:** One conformance test takes every mutating tool from the manifest and
drives it through the live harness. It fails when an answer carries no observed
post-state, or when that state disagrees with what the engine reports when read
independently.

**How:** Define the shape once, as the fields a mutation answer uses for what it
read back; `scene_set_property`'s `value`, `applied` and `not_applied` are the
model. Put the test in `tests/run_godot_integration.ps1` so it runs on all three
engine lines. A tool that writes files is checked by loading its output in the
editor with `CACHE_MODE_IGNORE`. An exemption list names any tool that cannot be
checked yet and why, and every exemption is a finding to fix, not a place to
stay. The one exception is a tool that claims no state after its call, which is
exempt for good and says why (#1020).

**Done when:** Every implemented mutating tool is either checked or exempted
with a reason, and a new mutating tool with neither fails CI.

**Not in scope:** Changing what the tools do. This finds the ones that
misreport.

**Surface amendment:** Not needed.

### Q3. Contract snapshots

**Principles:** [P8](DESIGN_PRINCIPLES.md#p8-own-the-boundary-stay-in-scope-derive-every-fact),
[P1](DESIGN_PRINCIPLES.md#p1-success-means-the-change-was-observed).

**Why:** Q4 and Q5 change what the tools list and what they answer. A reviewer
should see every such change as a diff in the pull request, not learn of it from
a client that broke.

**What:** For each engine line, a committed snapshot of `tools/list` and of the
answers to a fixed set of read-only calls against a fixed fixture project.
Volatile values such as pids, timestamps, session ids and absolute paths are
replaced by placeholders. CI records the snapshots again and fails on any
difference the pull request did not commit.

**How:** Build on `didi --dump-tool-manifest` and the live harness. Keep the
call set read-only and deterministic. Regenerating is one command, the way
`tools/test_inventory.py` regenerates the test counts.

**Done when:** A change to any tool's schema, or to any snapshotted answer,
fails CI until the snapshot is regenerated in the same pull request.

**Surface amendment:** Not needed.

### Q4. Shrink the tool list

**Principles:** [P4](DESIGN_PRINCIPLES.md#p4-the-agent-pays-only-for-what-it-uses).

**Why:** On 2026-09-27 `tools/list` was about 201 KB for 130 tools, roughly 50
to 60 thousand tokens before an agent does anything. Input schemas were 58% of
it, per-tool `_meta` 12%, descriptions 10%, and annotations and output schemas
6% each. The ten legacy names alone were 12% of the total. Across six trials an
agent reached 35 to 38 distinct tools per run, and 60 implemented tools had
never been called. Some hosts search deferred tool definitions; others load
every definition into every session.

**What:**

- Legacy names are off by default, and on with a flag.
- `_meta` fields that are the same on every tool, such as
  `confirmationsSkipped` and `editorConnected`, move to server-level metadata.
- Descriptions and schema prose that repeat the schema are trimmed.
- A startup profile, `--tools core|full`. `core` is the union of the tools any
  trial reached, and the implemented tools the handshake guide names, so a
  core session can follow its own guide. `full` stays the default until a
  trial on `core` shows no loss.
- A byte budget for each profile, checked in CI.

**How:** Measure the same way the figures above were taken: `initialize`, then
`tools/list`, against `demo/`. The profile is chosen at startup so the tool list
never changes during a session, because a change mid-session throws away the
client's prompt cache. Exceeding a budget fails the build. Raising a budget
takes its own pull request with a reason.

**Done when:** A `core` session's tool list is under half of today's bytes,
`full` is smaller than today, and both budgets are enforced in CI.

**Not in scope:** Folding tools behind a dispatcher. See
[Refusals](DESIGN_PRINCIPLES.md#refusals).

**Surface amendment:** Not needed. Turning legacy names off by default breaks a
client that calls them, so it goes in the changelog under Breaking.

**Delivery:** Two pull requests. The first ships everything that breaks
nothing: the profiles, the budgets, the trimmed prose, and the shared `_meta`
fields moved to the listing inside `core`, which no client used before. The
second moves those fields in `full` too and turns legacy names off by default.
Both break a host that reads what
[API_SPECIFICATION.md](API_SPECIFICATION.md#didi-capability-extension) promises,
so it lands with a major version, and #1012 stays open until it does.

### Q5. Response economy

**Principles:** [P4](DESIGN_PRINCIPLES.md#p4-the-agent-pays-only-for-what-it-uses).

**Why:** #776 measured seven ordinary live calls. Of their 11,834 bytes, 73%
repeated what the caller already had: 42% was the text copy of
`structuredContent`, 26% the session descriptor sent on every call, and 6% the
same `limitation` sentence each time.

**What:** The session descriptor is sent once per route and referenced after
that. A client that reads `structuredContent` can decline the duplicate text
copy. Large reads accept a `fields` selection. Every bounded read carries
`complete` or `truncated`.

**How:** Follow #776. The opt-out is negotiated, in `initialize` or in `_meta`,
so a client that reads only `content` sees no change.

**Done when:** #776's seven-call arc costs under half its bytes for a client
that opts out, and is unchanged for one that does not.

**Surface amendment:** Not needed.

**Delivery:** Two pull requests. #1030 is the negotiated opt-out: a client
that declares the `didi/responseEconomy` extension can decline the text copy
and the session descriptor it already holds, and it met **Done when** on every
engine line. #1039 gives the two largest sectioned reads `fields` and every
bounded read `truncated`, after an audit of all 54 reads' answers, which is
what 11 of the 18 tools taking a bound needed, since they published no
`outputSchema`. Between them, `--session-descriptor once` (#1034) and live
resource reads (#1033) carry the descriptor half to hosts that cannot declare
anything.

### Q6. Next step in every answer

**Principles:** [P5](DESIGN_PRINCIPLES.md#p5-guidance-belongs-where-the-agent-is-already-looking).

**Why:** Trial 06 showed that guidance changes behaviour when it is in front of
the agent: the handshake guide cut failed calls from 22.7% to 3.2%. What the
guide asked for without a call to attach it to did not change: 4 of 35 writes
were read back, and `editor_undo` has gone uncalled for six runs.

**What:**

- Every refusal names the argument or call that fixes it, by extending
  `retry_with` to all of them.
- Every mutation that leaves work undone says so: an unsaved scene, a rescan, a
  restart.
- When the same call fails the same way twice in a session, the answer says so
  and names the alternative.

**How:** A test walks every refusal path and fails on one with no remedy.
Repeat detection keys on the tool, a hash of the arguments and the error code.
It never refuses a call, because polling is legitimate.

**Done when:** The refusal test covers every tool, and a field trial shows no
run of three identical failures.

**Surface amendment:** Not needed.

**Delivery:** Four pull requests. The first gives every error code a remedy,
or a reason it has none, and checks it in CI and on every live refusal. The
second is repeat detection. The third answers as envelopes the 199 failures
that were still plain text, with no `error.data` to carry a remedy, and makes a
new one fail to compile. The fourth is the structured next step on a mutation
that leaves work undone. Field trial 07 measures the second half of
**Done when**.

### Q7. Typed object layer

**Principles:** [P6](DESIGN_PRINCIPLES.md#p6-reach-through-typed-access-not-arbitrary-code),
[P3](DESIGN_PRINCIPLES.md#p3-one-write-path).

**Why:** The trials' fallbacks happen where the surface stops. Scene text was
patched by hand for properties the tools could not write until #210, and
resource files until #380. Real limits remain: one property per call, arrays
refused, and no way to write a property inside a live node's sub-resource, such
as a material's shader parameter or a theme override's StyleBox. Editing an open
scene's file instead is a data-loss risk, because the editor writes its own copy
over the file at the next save.

**What:**

- Batched reads of property paths on nodes and their sub-resources, with each
  property's type, hint, range and enum from ClassDB. Shallow by default, and
  capped in depth and size.
- Batched writes to those paths as one UndoRedo action, each value read back.
- Calls to ClassDB methods. Const methods are free; anything else needs a
  confirmation token.
- Paths in Godot's own grammar, for example
  `Player/Sprite2D:material:shader_parameter/tint`, resolved with `get_indexed`
  and written with `set_indexed` through `EditorUndoRedoManager`.

**How:** Start by extending `scene_get_property` and `scene_set_property` to
take a batch and an indexed path, and add a name only where no existing tool
fits. Values use the JSON forms `scene_set_property` already documents, widened
to arrays and packed arrays. A property whose getter or setter is unsafe to call
is excluded by a written list with the reason for each entry. This subsumes the
batched `scene_get_properties` in the
[realignment plan](REALIGNMENT_IMPLEMENTATION_PLAN.md) (B7).

**Done when:** A field trial finishes with no property patched into scene text,
and the live harness proves on all three engine lines that a batch write undoes
as one step.

**Not in scope:** Fuzzy targeting, or any normalisation beyond the elastic
ingress profile. This widens the contract `scene_set_property` already has. It
is not the reflective mutation the elastic ingress milestone excludes, which was
about turning loose arguments into reflection calls.

**Surface amendment:** Required for any new name. The failing workflow is
setting a shader parameter or a theme override on a node in the edited scene.

### Q8. Long work as jobs

**Principles:** [P7](DESIGN_PRINCIPLES.md#p7-proof-is-a-result-not-a-claim).

**Why:** A live call has a 15-second extension deadline and a 17-second
transport deadline. Work that takes longer has to fit or fail: reimporting many
new scripts on a slow editor (#996), an export, a C# build. A client that times
out and tries again can run a mutation twice.

**What:** A long operation returns a job id at once. Its result is kept and can
be read later. A mutating call may carry a request id that the extension
deduplicates, so a repeat returns the first result instead of running again.

**How:** One job model shared by every long operation, with a bounded store, an
expiry, and a result that says whether the work started, finished or was
abandoned. Where the client supports the protocol's own long-running task
mechanism, use it; the job model must also work without it.

**Done when:** `asset_reimport`, `project_export` and `csharp_check_build` run
as jobs, #996's ten-second ceiling is gone, and a repeated request id returns
the first result.

**Surface amendment:** Required if jobs need tool names of their own.

### Q9. Proof in one call

**Principles:** [P7](DESIGN_PRINCIPLES.md#p7-proof-is-a-result-not-a-claim).

**Why:** Testers have called the pause, inject and step loop the most capable
thing Didi does, and `runtime_explore_scene` is what stopped trial 05 filing a
false bug. Proving a behaviour still takes a dozen calls assembled by hand, and
nothing records what a pass was true for.

**What:**

- A scenario runner. Its steps are declarative: launch, wait frames, press or
  hold an action, assert an expression, assert output seen since the start,
  capture, stop. It runs as a job. A scenario with no assertion is refused, and
  a smoke run can never report a pass. Teardown runs on every path, and a failed
  teardown fails the run. The result records hashes of the scripts and scenes it
  ran, and a later change to them marks the pass stale.
- A test runner that finds GUT or GdUnit4 in the project, runs it headless, and
  returns a result per test.

**How:** Compose `runtime_launch`, `runtime_inject_input`,
`runtime_watch_invariants`, `runtime_read_output` and `viewport_capture_frame`,
adding no new engine access. Assertions use the read-only expression subset.

**Done when:** The standing exercise in
[Surface Amendments](SURFACE_AMENDMENTS.md#how-a-failing-workflow-is-found),
"make the player double-jump and prove it works", ends in one call that returns
a pass with its evidence.

**Surface amendment:** Required.

### Q10. Graph builders that round-trip

**Principles:** [P1](DESIGN_PRINCIPLES.md#p1-success-means-the-change-was-observed),
[P3](DESIGN_PRINCIPLES.md#p3-one-write-path).

**Why:** Godot authors shaders, animation state machines and blend trees as
graphs. `shader_get_visual_graph` can read a VisualShader and nothing can write
one, and AnimationTree has no tools at all. An agent that cannot build a graph
writes the shader in code or leaves the feature out.

**What:** Builders for VisualShader, then AnimationTree blend trees and state
machines, then scene subtrees. Each accepts exactly the format its matching
export returns.

**How:**

- Nodes carry caller-chosen ids, and the answer maps them to what the engine
  created.
- Ports are addressed by name, and a miss lists the valid ones.
- Every connection is checked before commit, and the whole build is one UndoRedo
  action.
- `patch` changes only what the spec names. `rebuild` first reports what it
  would remove.
- No default ever deletes existing content.
- State machine transitions carry Godot's own `advance_expression`.

**Done when:** Export, build, export gives identical output on all three engine
lines, and the harness proves a build undoes as one step.

**Surface amendment:** Required.

### Q11. Godot's own debugger and language server

**Principles:** [P1](DESIGN_PRINCIPLES.md#p1-success-means-the-change-was-observed).

**Why:** The Phase 7 gate marked `runtime_get_call_stack` API-blocked after
probing the engine classes exposed to GDExtension. It did not probe the editor's
Debug Adapter Protocol server. On 2026-09-27 a headless 4.5.1 editor and a
headless 4.7.2 editor both answered a Debug Adapter Protocol `initialize` on
port 6006, and that protocol carries stack frames, scopes, variables and
breakpoints. The same editors run the GDScript language server on port 6005,
which knows the project's autoloads and class names. `script_check_syntax`
currently works around their absence with a heuristic (#383).

**What:** Re-run the Phase 7 gate for `runtime_get_call_stack` against the debug
adapter on all three engine lines, and implement it if the gate says GO. Use the
language server as the diagnostics backend where an editor is running, with the
headless check as the fallback. This is B6 and part of B8 in the
[realignment plan](REALIGNMENT_IMPLEMENTATION_PLAN.md).

**How:** Consuming Godot's own servers is not building a language server, so
the Phase 8 exclusion stands. Read the ports from the editor settings rather
than assuming the defaults. Two editors on one machine contend for one port;
report that instead of guessing which editor answered.

**Done when:** The gate's result is recorded in the
[Phase 7 feasibility evidence](PHASE_7_API_FEASIBILITY.md), and diagnostics for
a script that uses an autoload come from the language server with no heuristic.

**Not in scope:** Breakpoint and stepping tools. They need their own amendment
once the call stack works.

**Surface amendment:** Needed only if the call-stack contract changes.

### Q12. didi setup and didi doctor

**Principles:** [P5](DESIGN_PRINCIPLES.md#p5-guidance-belongs-where-the-agent-is-already-looking),
[P8](DESIGN_PRINCIPLES.md#p8-own-the-boundary-stay-in-scope-derive-every-fact).

**Why:** A new project is where every real user starts, and where trials have
lost the most time: a stale addon beside a new server (#325, #326), an addon
that could not be enabled from the surface (#382), a client configuration
written by hand. The dock's Connect page solves this for someone already inside
the editor, and nobody else.

**What:** `didi setup --project <path>` installs the addon that matches the
binary, enables it, writes the configuration for the named clients, writes an
agent guide into the project's agent instructions file, and waits until the
editor answers. `didi doctor` runs the checks the dock's Diagnostics page runs,
from the command line.

**How:**

- Reuse the dock's configuration writer and the release archive's layout.
- Write the agent guide between `BEGIN didi` and `END didi` markers, so a rerun
  replaces only that block. UTF-8, no byte-order mark.
- Never overwrite an addon that differs without saying which one is newer.

**Done when:** From a directory holding only `project.godot`, one command gives
a working session in each supported client, proved by a Python test and a fresh
install on all three platforms.

**Not in scope:** A self-updater. See [Refusals](DESIGN_PRINCIPLES.md#refusals).

**Surface amendment:** Not needed; these are command-line subcommands, not
tools.

### Q13. Project-defined tools

**Principles:** [P6](DESIGN_PRINCIPLES.md#p6-reach-through-typed-access-not-arbitrary-code),
[P3](DESIGN_PRINCIPLES.md#p3-one-write-path).

**Why:** Every project has operations no general surface should carry: its own
spawners, its own save format, its own debug switches. Without a way to add
them, the only choices are a canonical name for one project, which the amendment
rules refuse, or arbitrary code, which P6 refuses.

**What:** A project declares tools in GDScript, each with a name, a JSON schema,
annotations and a safety class. The extension registers them, and every call
passes through Didi's schema validation, dry-run, confirmation, deadlines and
journal. A running game can declare its own tools, which is how a game exposes
its state for testing (B7 in the
[realignment plan](REALIGNMENT_IMPLEMENTATION_PLAN.md)).

**How:**

- Project tool names carry a fixed prefix, so they can never collide with a
  canonical name.
- Capability metadata marks them as project-defined, so an agent knows Didi does
  not vouch for what they do.
- A project tool that mutates must say how to undo it, or it is always
  confirmed.
- Phase 12's rule holds: an extension cannot bypass project containment,
  authentication, route policy, dry-run or confirmation.

**Done when:** A sample project declares one editor tool and one game tool, and
the live harness proves both are validated, previewed, confirmed and journaled
the way canonical tools are.

**Surface amendment:** Required for the mechanism. It adds no canonical name.

### Q14. A skill pack, measured

**Principles:** [P5](DESIGN_PRINCIPLES.md#p5-guidance-belongs-where-the-agent-is-already-looking).

**Why:** The handshake guide says how to use Didi. It does not say how to build
a Godot game with it: the order to wire a TileSet in, how the InputMap meets the
runtime tools, what a UI layout needs before a capture means anything. Clients
that load skills read one when a task matches it, which is the moment the guide
cannot reach.

**What:** Six to ten skills, one per workflow: scenes, resources and TileSets,
UI layout, animation, input and runtime testing, shaders, export, performance.
Plus one generated from the project: engine version, autoloads, input actions,
main scene, addons and test framework. `didi setup` installs them.

**How:** Skills describe workflows and the traps in them. They never restate a
schema. A fixed set of task prompts checks that each prompt reaches the right
skill, run against more than one model size.

**Done when:** The routing check passes, and a field trial on a new task uses at
least one skill without being told to.

**Surface amendment:** Not needed.

### Q15. Change journal with undo

**Principles:** [P1](DESIGN_PRINCIPLES.md#p1-success-means-the-change-was-observed),
[P3](DESIGN_PRINCIPLES.md#p3-one-write-path).

**Why:** Six trials on two vendors, and `editor_undo` has never been called,
though every mutation reports `undo_redo_registered: true`. Agents want undo
honoured, not driven. The person reviewing what an agent did is the one who
needs it, and today nothing tells them which entries in the editor's history
were the agent's.

**What:** Every mutation is recorded with its tool, its target, the values
before and after, the files it touched and its undo reference. The dock and the
Control Room show the journal, with undo on each entry that can still be undone.

**How:** Record in the shared write path, not per tool. The journal is bounded,
redacts secrets by key name, and is written atomically. An entry that can no
longer be undone, because later history depends on it, says so.

**Done when:** The live harness makes a sequence of mutations, reads the journal
back, undoes one entry from it, and confirms the engine state.

**Surface amendment:** Required if the journal is read through a new tool
rather than a resource.

### Q16. Performance verdicts

**Principles:** [P7](DESIGN_PRINCIPLES.md#p7-proof-is-a-result-not-a-claim).

**Why:** `runtime_read_profiler` returns monitor samples. An agent asked why a
game is slow needs to know first whether the frame is bound on the CPU, the GPU
or physics, because work on the wrong one changes nothing.

**What:** Beside the samples, a verdict: CPU bound, GPU bound, physics bound or
contested, with a confidence and the next thing to look at. A self-check injects
a known stall and confirms the verdict sees it.

**How:** Combine the `Performance` monitors with the viewport's measured render
times. Probe the binds on all three engine lines before designing the contract,
as the Phase 7 gate did.

**Done when:** The verdict is right for three fixture scenes built to be CPU,
GPU and physics bound, on all three engine lines.

**Surface amendment:** Not needed; this extends `runtime_read_profiler`.

## Paused

Left out of the queue on purpose. Each resumes when its condition is met.

- **Phase 10, Gogo parallel orchestration.** Resume after Q9, when one agent can
  prove a behaviour in one call. Running many agents multiplies whatever a
  single agent still gets wrong.
- **Further agent coordination (Phase 9a).** The blackboard stays as it is. In
  the first five trials it was written in two and read back in one. Resume when
  a trial shows coordination as the thing that blocked it.

## Changing the queue

- **Adding an item:** add a row and a section with **Why**, **What**, **How**
  and **Done when**. The why cites evidence: a trial, an issue, a measurement.
- **Reordering:** move the rows in a pull request that says why.
- **Pausing:** set the row to `PAUSED` and say what would resume it.
