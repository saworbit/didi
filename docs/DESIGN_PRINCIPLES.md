# Design Principles

The rules Didi's tool surface follows, and the reasons for them. Each principle
says how it is enforced today and, where it is not enforced yet, which
[Build Queue](BUILD_QUEUE.md) item will do it.

The evidence is Didi's own. Most of it comes from the
[field trials](FIELD_TRIAL_RESULTS.md), where an agent that has never seen Didi
builds a small game with it, and from the issues those trials and ordinary use
have filed.

A change that breaks a principle needs its reason in the pull request. A change
to a principle is made here first, with the evidence that the old reason no
longer holds.

## P1. Success means the change was observed

**Rule.** A mutation's answer reports the state it read back after the write,
not the value it was sent. A read never describes its result as something it is
not.

**Why.** An agent has no eyes. The response is its whole model of the project,
so a false success corrupts every decision made after it. A crash is loud and
recoverable by comparison. The trials found this class in every run from 02 to
05: #213, #215, #217, #327, #330 and #374. A false failure costs the same thing
from the other side. #383 reported an error for every script that named an
autoload, and a check that cries wolf teaches the agent to stop reading it.

**In practice.**

- Return the observed post-state. It is cheaper than a type-aware equality
  check, and it cannot be wrong in the way a comparison can.
- Check a file writer by loading its output through the engine. A reader
  written beside the writer shares the writer's assumptions, so it cannot be
  the witness.
- For a batch, verify the aggregate rather than each record, or verification
  costs more than the write.
- Say what was not verified. `unknown_outcome`, `limitation` and `not_applied`
  exist for exactly this.

**Enforced by.** `tests/observed_post_state.json`, from
[Q2](BUILD_QUEUE.md#q2-observed-not-asserted). Every mutating tool either names
the answer fields that carry what it read back, which the live harness compares
with the engine on all three engine lines, or is exempted with the issue that
tracks the gap. A tool that claims no state after its call, such as one that
runs project code, is exempted for good and says why. `tests/test_observed_post_state.py` fails the build when a
mutating tool has neither. The exemptions are findings: #1020 lists the ones
no case drives yet. The tools #1019 found answering with the request all read
back now.

## P2. Reject what cannot be interpreted

**Rule.** Unknown arguments are refused, every top-level argument declares its
JSON type, and a supplied locator is authoritative.

**Why.** A tolerated typo or an ignored argument becomes a write to the wrong
target, and the agent believes it succeeded. Clients also fill a gap in a
schema their own way. A Claude host sends any argument with no declared type as
a string, so `project_set_setting` could not write an int, a bool or an array
(#1000).

**In practice.**

- Keep `additionalProperties: false` and exact types on every schema.
- When a name is ambiguous, refuse and list the candidates. Never pick one.
- Normalise input only with an explicit warning, and only under the opt-in
  [elastic ingress](ELASTIC_INGRESS.md) profile.
- A refusal names the argument that fixes it, in `retry_with`.

**Enforced by.** Strict schemas, the refusal checks from #784 and #902, and
`tool_input_schema.every_argument_typed` from
[Q1](BUILD_QUEUE.md#q1-type-every-argument), which fails the native suite when
a top-level argument of any tool declares no JSON type, or one that leaves out a
value the rest of its schema accepts.

## P3. One write path

**Rule.** Every mutation goes through the same primitives, whichever tool,
builder, workflow or project-defined tool asked for it. Those primitives record
UndoRedo, honour `dry_run` and confirmation, read back, and journal.

**Why.** A guarantee each handler implements for itself drifts. Undo, preview
and read-back end up depending on which tool the agent happened to call, and
nobody can say what the surface as a whole guarantees. One path is also the
only place a contract such as P1 can be enforced once instead of tool by tool.

**In practice.**

- A new mutating tool calls the shared primitives. It does not call the
  engine's setters directly.
- Higher-level tools, such as builders, scenarios and project-defined tools,
  are compositions of those primitives. They inherit the guarantees instead of
  reimplementing them.

**Enforced by.** Review, and for property writes the path [Q7](BUILD_QUEUE.md#q7-typed-object-layer)
began: a single write, a path into a sub-resource and a batch all go through one
prepare-then-commit pipeline that checks, records UndoRedo, honours `dry_run` and
reads back, and the live harness proves a batch undoes as one step. The rest
of Q7 widens that layer to every value and to ClassDB methods, and
[Q13](BUILD_QUEUE.md#q13-project-defined-tools) routes project-defined tools
through it. From [Q15](BUILD_QUEUE.md#q15-change-journal-with-undo), every
mutating call is journalled where every call passes, in dispatch, with the
undo step the bridge read off the editor's history around the command, which
it does for every command on the main thread rather than per handler. The live
harness reads the journal back, undoes one entry from it and checks the engine.

## P4. The agent pays only for what it uses

**Rule.** Every byte in the tool list and in every answer is context the agent
cannot spend on the project, so both have a budget.

**Why.** On 2026-09-27 `tools/list` was about 201 KB for 130 tools. Across six
trials an agent reached 35 to 38 distinct tools per run, and 60 implemented
tools had never been called by any of them. On a live authoring arc, 73% of the
bytes repeated what the caller already had (#776). `project_list_input_actions`
once answered with about 69 KB, and 85 of its 90 actions were the engine's own
defaults (#775).

**In practice.**

- Keep each tool individually named, schema'd and annotated. That is how a
  client grants permission one tool at a time, and a capability the agent cannot
  see is a capability it does not use. Make each tool cheaper to carry rather
  than hiding it.
- Send session facts once, not on every answer.
- Default to the project's own data, with built-ins on request.
- Every bounded read says whether it is complete. A short answer must never
  look like a whole one.

**Enforced by.** `tests/tool_list_budgets.json`, from
[Q4](BUILD_QUEUE.md#q4-shrink-the-tool-list): each tool profile's `tools/list`
has a byte budget, CI fails a listing over it, and raising one takes its own
pull request with a reason. From [Q5](BUILD_QUEUE.md#q5-response-economy),
the live harness runs #776's seven-call arc for a client that declared
`didi/responseEconomy` and for one that did not, on every engine line, and
fails unless the first costs under half the bytes of the second.
`tests/bounded_reads.json` classifies every read as bounded or unbounded, and
CI and the live harness fail a bounded read's answer that does not carry
`truncated`.

## P5. Guidance belongs where the agent is already looking

**Rule.** What the agent needs to know arrives in the handshake, or in the
answer to the call it just made.

**Why.** Trial 06 was the first run against a server that returns its guide in
the handshake. Failed calls fell from 22.7% of all calls in trial 03 to 3.2%,
no failing call was sent twice, and no legacy name was used. The same guide's
standing requests, which are not tied to any one call, changed nothing: 4 of 35
writes were read back, and `editor_undo` has not been called in six runs. A
document an agent has to go and find is read by people, not by agents.

**In practice.**

- Every refusal carries the argument or the call that fixes it.
- Every mutation names the follow-up it still needs, as a `follow_up` step: a
  save or a restart. A rescan is not one, because it does not refresh an
  editor's copy of a file it already holds (#1047).
- When the same call fails the same way twice, the answer says so and names the
  alternative. A repeated call is never refused outright, because polling is
  legitimate.
- Skills carry workflows and traps, never signatures. The schemas are the
  contract.

**Enforced by.** The handshake guide (#962), the test that pins what it claims
(#998), and the `retry_with` checks. From Q6, `src/mcp/refusal_remedies.cpp`
gives every error code a remedy or a reason it has none, CI fails a code the
source emits that it does not cover, and the live harness fails any refusal
that names no fix. `tests/follow_ups.json` says what work every mutating tool
can leave, `src/mcp/follow_ups.cpp` names it as a `follow_up` step, and the
live harness fails a step a tool did not declare. [Q6](BUILD_QUEUE.md#q6-next-step-in-every-answer) and
[Q14](BUILD_QUEUE.md#q14-a-skill-pack-measured).

## P6. Reach through typed access, not arbitrary code

**Rule.** The long tail of properties and methods is reached through typed,
bounded, undoable access described by ClassDB. The agent does not send code
for the editor to run.

**Why.** No surface can name every property of every class, and where the
surface stops, the agent falls back to editing files. The trials show where
that leads: scene text patched by hand until #210, and resource files written
by hand until #380. In Godot that fallback is also a data-loss risk, because an
editor holding a scene open writes its own copy over the file at the next save.

Running arbitrary code would close the gap and give up every guarantee Didi
makes: undo, preview, typed validation, bounded time, and a record a person can
read. A persistent interpreter is also shared state that any one call can
corrupt. Godot does not force that trade. ClassDB describes every property's
type, range and hint, `get_indexed` and `set_indexed` address nested properties
by path, and `EditorUndoRedoManager` can record any of it.

**In practice.**

- Generic access is typed by ClassDB, bounded in depth and size, read back, and
  undoable.
- Reads are shallow by default. Deep reads are opt-in and capped.
- Const methods may be called freely. Anything else needs confirmation.
- A project widens the surface with project-defined tools: reviewed code that
  lives in the project, behind the same guards.
- `eval_gdscript` stays a read-only expression subset.

**Enforced by.** The expression sandbox, and for properties the paths and batches
of [Q7](BUILD_QUEUE.md#q7-typed-object-layer): every step of a path is checked against
ClassDB's property list, a path steps only into resources, a read reports the
declared type and constraint, and writes the layer refuses are a written list with
a reason each.
[Q7](BUILD_QUEUE.md#q7-typed-object-layer) and
[Q13](BUILD_QUEUE.md#q13-project-defined-tools).

## P7. Proof is a result, not a claim

**Rule.** "It works" is a scenario or test result that carries its evidence.

**Why.** Trial 05's best moment was a bug that was not one. Four rounds had
convinced the tester that input injection was broken. `runtime_explore_scene`
showed the player moving once it was no longer wedged between three enemies,
and the report was never filed. A check with no assertion proves nothing, and a
pass is only true for the files it ran against.

**In practice.**

- A scenario with no assertion is refused, and a smoke run can never report a
  pass.
- Teardown runs on every path, and a failed teardown fails the run.
- A result records what it ran against, and goes stale when those files change.
- Long work runs as a job with a stored result, so a client timeout never runs
  a write twice.

**Enforced by.** [Q8](BUILD_QUEUE.md#q8-long-work-as-jobs), which runs
`project_export`, `csharp_check_build` and `asset_reimport` as jobs: a
`request_id` repeated after a client timeout reads the job instead of running
the work again, a native test counts the runs, and the live harness checks that
a repeated reimport job reimports nothing. [Q9](BUILD_QUEUE.md#q9-proof-in-one-call) is the rest.

## P8. Own the boundary, stay in scope, derive every fact

**Rule.** Didi keeps its own process, transport, timeouts and retry policy. It
builds what serves Godot development in general. It states no fact a machine
could derive.

**Why.** The out-of-process server, the deadlines and per-route quarantine are
what turn a wedged or crashed editor into a structured error instead of a hang.
Every feature costs context, tests and maintenance for as long as it exists.
Hand-kept counts go stale; the amendment log's own count did (#990).

**In practice.**

- stdio to the client, authenticated local IPC to the engine, and nothing
  listening on a network.
- Counts come from the manifest. A gate that fails is fixed, or the change is.
  A budget is never raised just to let a build through.
- A feature that serves one game, one genre or one side business stays out of
  the core.

**Enforced by.** `tools/validate_documentation.py`, the test inventory gate, the
[Surface Amendments](SURFACE_AMENDMENTS.md) record, and the contract snapshots
in `tests/contract_snapshots/`, from
[Q3](BUILD_QUEUE.md#q3-contract-snapshots). What a client is shown, offline and
with an editor attached on each engine line, is recorded by a tool rather than
written by hand, and CI fails when it changes without the snapshot changing in
the same pull request.

## Refusals

Things Didi does not build. Each was tempting for a good reason, which is why
the reason for refusing is written down. Reopening one needs evidence that the
reason no longer holds, recorded here first.

| Refusal | Why |
| --- | --- |
| One tool that takes an action name and dispatches to many operations | The client can no longer validate arguments per operation, and annotations and permissions collapse to the whole group, so a destructive operation shares a grant with a read. At Didi's size it saves little. See P2 and P4. |
| Arbitrary code execution as a primary interface | It gives up undo, preview, typed validation, bounded time and a readable record (P6). If it is ever added, it is an operator opt-in flag, one call per snippet with no state kept between calls, always confirmed and always journaled. |
| A network listener, or hosting Didi inside another server | It inherits someone else's authentication, timeouts and failure modes, and widens who can reach the editor (P8). |
| Tool schemas generated from engine types | A schema is a wire contract, judged by the strictest client. Generated references named after engine types fail validators in ways each client reports differently. Schemas stay hand-authored, plain JSON Schema (P2). |
| Heavy work inside the editor process | The editor process belongs to the user. Indexing and analysis run offline or read metadata only, and never change engine state they did not create (P8). |
| Whole-object reads by default | Serialising a live object recursively is unbounded in size and can reach getters that crash the engine. Reads are shallow and paged (P4, P6). |
| An empty tool list while the engine is away, or a blind retry of a mutation | `tools/list` always answers, with per-tool availability. A mutation is retried only when it provably never reached the engine (P1, P7). |
| A self-updater | An updater runs as the old installed version, so a bug in it cannot be fixed by the release that follows. Upgrades go through the release archive and its installer. |
| An in-editor chat, a hosted model or a cloud relay | Didi is a local bridge for whichever client the user already runs. A hosted service turns it into an account and billing product (P8). |
| Features for one game, one genre or one side business | Every user pays for them in context and maintenance, for one user's benefit (P8). |
| Renaming a canonical tool | Names are the contract. Add a name, keep the old one as a legacy alias, and remove it only in a major version. |
