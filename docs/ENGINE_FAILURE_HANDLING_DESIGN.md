# Engine Failure Handling Design

Status: design, approved to plan
Date: 2026-09-07

## The problem

When the engine behind a live session stops being usable, a tool call fails with
a transport error. That error already carries facts: `engine` (alive, gone,
unknown), `engine_reason`, `outcome` (`not_started` or `unknown_outcome`), and
`engine_crash`. What it does not carry is what to do about it.

Two costs follow.

**The agent has no next move.** Its options are to call the same tool again,
which fails identically, or to abandon the task. Neither is right when most of
the surface still works without an engine.

**The human is never told.** Didi speaks only to the agent. If the agent retries
quietly and moves on, nobody learns the editor died. #285 was found by reading
CI logs, not because anything reported it.

## What is already true

This design adds a layer over machinery that exists. It invents no new facts.

- `describeProcessInstance` classifies a pid as alive, proven_stale or
  unverifiable, with a reason.
- `findEngineCrashReport` reads the report the in-process capture leaves, and
  says whether a frame of ours was on the faulting stack.
- `normalizeLiveRouteError` is the single funnel every live route already passes
  through when classifying a transport failure.
- `capabilityForTool` (`src/mcp/tool_registry.cpp:15`) already classifies every
  tool as `live`, `offline_fallback`, or both. That is the ground truth for what
  survives an engine loss, and the docs validator already covers it.
- `Binding` makes every call site declare `repeatable`, and
  `RuntimeRouting.MutationIsNeverRepeated` pins that a mutation is never
  replayed.

## Scope

In: engine crashed, engine unreachable, engine hung, session lost.

Out, and deliberately:

**Restarting the editor.** Didi cannot. `runtime_launch` starts a short-lived
headless process for a scene test. The editor is the user's own application,
attached to over IPC. Recovery from a crash goes through a human.

**Retrying the failed call.** The codebase already decided this. Auto-retry
would contradict `MutationIsNeverRepeated` and the `unknown_outcome` quarantine.

**Any new human-facing surface.** `HUMAN_INTERACTION_DESIGN.md` already settles
this: observability belongs in MCP Apps when host coverage is broad enough, an
editor dock is not justified, and log tails already have a home in
`runtime_read_logs` and `runtime_read_output`. This design adds no UI and no
second record.

## Design

### 1. One incident type

`EngineIncident` in `didi::runtime`, built from the process report, the crash
report and the transport outcome. One classification site, so the four routes
cannot disagree.

    enum class EngineIncidentKind { none, crashed, unreachable, hung, session_lost };

    struct EngineIncident {
        EngineIncidentKind kind{EngineIncidentKind::none};
        std::string cause;      // short, readable by a person and a machine
        std::string recovery;   // one imperative sentence the agent can act on
        bool recoverable_without_human{false};
    };

Classification, in order:

| Condition | Kind | Recoverable without a human |
| :--- | :--- | :--- |
| Engine proven gone, crash report found | crashed | no |
| Engine proven gone, no report | crashed | no |
| Engine unverifiable | unreachable | no |
| Engine alive, transport failed | hung | no |
| No session, but a live matching one exists | session_lost | yes |

### 2. Guidance on the error

`normalizeLiveRouteError` adds two fields beside the facts it already sets:

- `incident`: the kind, as a string.
- `recovery`: one sentence. For `crashed` it names the human action and the
  report path. For `session_lost` it names `runtime_attach_session`.

The list of what still works is **not** repeated on every error. It lives behind
the status tool below, and `recovery` points at it. A long list on every failure
is noise, and a second copy of it is a thing that drifts.

### 3. Status the agent can read

Extend `runtime_get_session`, which is already in the `offline` set and so keeps
working with no engine. It gains:

- `engine`: the current state.
- `last_incident`: kind, cause, recovery, and the crash report path if there is
  one.
- `available_tools`: derived by running `capabilityForTool` over the registry and
  keeping everything whose capability includes `offline_fallback`. Derived, never
  hand-listed.

### 4. Auto re-attach, and nothing more

When the incident is `session_lost` and a descriptor for the same project names a
process whose identity matches the one that was attached, Didi re-attaches and
says so in the response. The call that failed is still reported as failed. It is
never replayed.

Every other kind stays explicit. Hangs stay explicit because the IPC layer
already imposes a finite deadline, so waiting longer is the caller's judgement.

### 5. One line for the human

On first detection of a `crashed` incident, emit a single `DIDI_LOG_ERROR`
naming what died and the path to the report. Once per incident, keyed on the
engine pid, so a retry loop cannot flood it.

That is the whole human-facing change. The crash report written next to it is
already the durable record; a summary file beside it would be a second thing to
keep consistent for no new information. When MCP Apps coverage is broad enough,
the status in section 3 is what that surface would render.

## Testing

- Classification table: each row above produces the expected kind and a
  non-empty recovery sentence.
- `available_tools` is derived, not listed: assert it is non-empty and contains
  no tool whose capability is `live` only. A hand-written list would satisfy a
  fixed assertion and then drift; a derived one cannot.
- Auto re-attach: a matching live engine re-attaches; a different process
  identity does not, and reports `crashed` or `unreachable` instead.
- The log line is emitted once for one incident and names the report path.
- Existing guarantees hold unchanged: `MutationIsNeverRepeated` and the
  `unknown_outcome` quarantine tests must still pass.

## Why this shape

The alternative was to put everything in the error payload. Simpler, but it
duplicates the capability list into a second place and repeats it on every
failure. Deriving from `capabilityForTool` means the answer cannot be wrong
without an existing test failing first.
