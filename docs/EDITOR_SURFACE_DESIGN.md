# Editor Surface Design

> **Status:** Proposed. Nothing here is implemented. This records a design for
> review, not delivered behavior. Current runtime behavior is in
> [Current Capability Matrix](CAPABILITIES.md) and [Tool Reference](TOOL_REFERENCE.md).

## Context

Didi is already hybrid. `didi.exe` is an external MCP server; `didi_extension`
is a GDExtension living inside the editor process (and, separately, inside a
running game), and the two talk over authenticated named-pipe IPC. The split
exists and carries the whole live tool surface.

What does not exist is any *surface* for the in-editor half. The addon is a stub:

```gdscript
@tool
extends EditorPlugin

func _enter_tree() -> void:
	print("[Didi] Didi Native MCP Editor Plugin active.")

func _exit_tree() -> void:
	print("[Didi] Didi Native MCP Editor Plugin deactivated.")
```

It exists so Godot has a `plugin.cfg` to enable, and does nothing else. The
evidence that it is unowned is that it has quietly rotted: `addons/didi/plugin.cfg`
reads `1.4.0` while `demo/addons/didi/plugin.cfg` reads `1.2.0`, and the
documentation validator checks only the first. The demo copy also carries
pre-arch-suffix macOS library entries, from before the canonical copy gained
per-architecture paths.

(`test_lab_sandbox.tscn` appears only under the canonical addon, but that is
not drift: it is generated on demand by `viewport_create_test_lab` and is
gitignored. Worth noting only because the ignore rule covers the canonical path
and not the demo one.)

So the question is not whether to go hybrid. It is whether to surface the half
that already runs inside the editor, and if so, how much.

## What an editor surface is for

Two things, and only two, justify it. Both are things the external process
cannot do at all.

**Observability.** A user whose agent is not working has no way to see whether
the extension loaded, whether a session is attached, or what the engine has
been saying. Today the answer lives in a log ring reachable only through an MCP
tool call — which requires the very connection that is failing. This is the
difference between "it doesn't work" and "no session is attached."

**Human consent.** Today's confirmation tokens are bound to exact arguments,
project and route with a 120-second TTL. That is a real safety property: it
prevents accidental and misrouted mutation and forces a deliberate second call.
But the *agent* receives the token and echoes it back, so confirmation is
currently the agent confirming to itself. No human sees a mutation before it
lands. The editor is the only place that can change that, and doing so is the
largest available improvement to how much a user can trust Didi with a project
they care about.

## Governing constraint

**The dock renders; the extension decides.**

All logic stays in C++, where the native suite lives. GDScript becomes a thin
renderer of state the extension already exposes. This is not stylistic. A
second implementation of any rule — which tools are read-only, which mutations
need confirmation, what a session is — creates a place where two things can
disagree, and the recent history of this project is almost entirely about
removing such places. It also keeps the untestable surface small: an editor UI
is the least testable code this repository would own, in a language nothing
else here is written in, across three engine versions.

The corollary is that the integration harness can assert the *state* the dock
renders without asserting pixels.

## Tier 1: status and streams (proposed)

A dock showing, all read-only:

- Extension loaded, engine version, session kind, project root.
- Session id and route generation. Never the token.
- Whether engine output capture is active — it degrades to inert on an engine
  without the class-registration interface, and today that is only discoverable
  in a startup log line.
- A live tail of both rings: Didi's own diagnostics and the engine output
  stream added with `runtime_read_output`.

Cost is low and the failure mode is benign. Nothing here can mutate a project.

## Tier 2: human-in-the-loop approval (proposed, needs review)

The valuable half, and the one that changes the security model.

Shape, given that Didi ships MCP revision `2024-11-05`:

1. A mutation that requires confirmation returns `structuredContent` with
   `status: "awaiting_human_approval"` and a request id, instead of a token.
2. The dock shows the pending request: tool, exact arguments, target node or
   file, and the diff where one exists.
3. A human approves or denies in the editor.
4. The agent retries; the retry succeeds or returns a denial.

Three properties make this fit better than it might appear:

- **It adds no tool names.** The poll is the agent retrying the same tool, so
  no surface amendment is required.
- **Coverage lines up.** Destructive tools are overwhelmingly `editor_only`,
  and the dock lives in the editor process. A game session has no dock, and
  needs none.
- **It upgrades cleanly.** This is the server half of elicitation. When Didi
  adopts the `2026-07-28` revision, the same mechanism becomes spec-native
  through `InputRequiredResult` rather than a poll.

### Constraints recorded before any work starts

- **Never block the main thread waiting on UI.** This is the same deadlock class
  as the engine output logger callback. The approval state must be observable
  from the command queue without waiting on it.
- **Opt-in per project, default off.** A required approval that nobody is
  present to give would strand a headless CI agent forever. The default must be
  today's behavior.
- **Decide the interaction with the existing 120-second token TTL.** A human is
  slower than an agent. Either the pending request carries its own longer
  deadline, or approval mints the token at approval time rather than at preview
  time.
- **Decide what happens when the editor closes with a request pending.** The
  honest answer is that the request dies with the session and the agent is told
  so; the alternative is a durable approval queue, which is a much larger thing.
- **A denial must be distinguishable from a timeout.** An agent that cannot tell
  "a human said no" from "nobody was watching" will retry the first.

## Explicitly out of scope

**Buttons that invoke tools from the dock.** That makes the editor a second MCP
client and doubles the surface that has to be kept honest. Every argument for it
is an argument for a person driving Didi by hand, which the MCP client already
does better.

**Any settings UI.** Configuration has one source: the project and the launch
arguments. A second one is a second thing to disagree with.

**Reimplementing any tool logic in GDScript.** See the governing constraint.

## Packaging note

`addons/didi/` is also the distribution unit for the Godot Asset Library, and
there are currently three copies of it in the repository — the canonical one,
the demo project's, and the integration fixture's (which is a deliberately
separate fixture plugin with its own identity, not a copy). Godot projects
cannot reference an addon outside their own `res://`, so the demo copy is
necessary; what is missing is any check that it matches. Bringing the demo copy
back into line and putting it under the documentation validator is worth doing
independently of anything above, and should not wait for this design.

## Open questions for review

1. Is Tier 1 worth shipping on its own? It is small and independently useful,
   and it would give Tier 2 somewhere to live.
2. For Tier 2, is opt-in-per-project the right default, or should approval be
   required whenever an editor is attached and a human is plausibly present?
3. Should the approval record be auditable after the fact — a log of what was
   approved, by whom, when — or is that a compliance feature this project does
   not want?
