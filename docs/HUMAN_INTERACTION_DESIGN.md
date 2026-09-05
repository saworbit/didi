# Human Interaction Design

> **Status:** Partly implemented. Steps 1, 2 and 4 of the recommended order have
> shipped; step 3 remains conditional and unstarted. Step 4 shipped larger than
> this document recommended, and [What was built, and where it exceeded
> this](#what-was-built-and-where-it-exceeded-this) records the difference rather
> than editing the recommendation to match. This supersedes the
> editor-dock proposal previously recorded as `EDITOR_SURFACE_DESIGN.md`, whose
> central recommendation was wrong; see [What changed and why](#what-changed-and-why).
> Current runtime behavior is in [Current Capability Matrix](CAPABILITIES.md) and
> [Tool Reference](TOOL_REFERENCE.md).

## The question

Didi mutates scenes, scripts and project settings on a user's behalf. Two things
follow, and neither is currently answered well:

- **Consent.** Confirmation tokens are bound to exact arguments, project and
  route with a 120-second TTL. That is a real safety property — it prevents
  accidental and misrouted mutation and forces a deliberate second call. But the
  *agent* receives the token and echoes it back, so confirmation today is the
  agent confirming to itself. No human necessarily sees a mutation before it lands.
- **Observability.** A user whose agent is not working cannot easily see whether
  the extension loaded, whether a session is attached, or what the engine said.

The question is where those surfaces belong.

## What changed and why

The earlier version of this document proposed building both in a Godot editor
dock, on the reasoning that "the editor is the only place that can become real
human-in-the-loop approval." That reasoning was never checked against the
protocol, and it is wrong. MCP had already solved it.

This is the same failure mode as the defects found while delivering the signal
tools: a confident claim, plausible on its face, that nobody had tested. It is
recorded here rather than quietly replaced, because the reasoning error is more
instructive than the conclusion.

## Consent belongs in the client, via elicitation

MCP defines [elicitation](https://modelcontextprotocol.io/specification/draft/client/elicitation):
a server returns an `InputRequiredResult` carrying an `elicitation/create`
request, the **client** presents it to the user, and the client reissues the
original call with the response. The spec's stated goal is that this "allows
clients to maintain control over user interactions and data sharing."

It already answers every constraint the editor-dock design listed as open:

| Constraint the dock design raised | How elicitation resolves it |
| :--- | :--- |
| Must never block the main thread waiting on UI | Multi round-trip: the server returns and is called again with `requestState`. Nothing blocks, so the logger-callback deadlock class does not arise. |
| The 120-second token TTL is shorter than human latency | No server-held token. State is echoed back on the retry. |
| What happens if the editor closes mid-request | The interaction is not in the editor. |
| A denial must be distinguishable from a timeout | Three-action model: `accept`, `decline`, `cancel` — dismissal is explicitly distinct from refusal. |

Building a second consent path in the editor would also be worse than redundant.
It would only work while the editor is open, it would be invisible to an operator
working in a terminal, and it would compete with the surface the client already
owns.

**Recommendation.** Replace agent-echoed confirmation with `elicitation/create`
for the confirmation-gated tools. Keep the existing token mechanism underneath —
it binds intent to exact arguments, which elicitation does not do on its own.

Two details worth carrying into implementation:

- Clients declare support in `_meta.io.modelcontextprotocol/clientCapabilities`,
  and a server **must not** send a mode the client did not declare. Behavior when
  a client declares nothing must be decided deliberately: falling back to today's
  agent-echoed confirmation is the honest default, and it must be visible in the
  result rather than silent.
- Form mode **must not** be used for secrets. Didi requests no credentials, so
  this does not currently bind, but it constrains any future auth work.

## Observability belongs in MCP Apps, not GDScript

[MCP Apps](https://github.com/modelcontextprotocol/ext-apps/blob/main/specification/2026-01-26/apps.mdx)
(`io.modelcontextprotocol/ui`) lets a server ship interactive HTML that the host
renders in a sandboxed iframe. It is an optional extension, negotiated through
the standard extensions mechanism, and it **works over stdio**, which is Didi's
transport. Hosts that do not support it fall back to text.

This is a better vehicle than an editor dock on every axis that matters here:

- It is cross-client, rather than only serving users who happen to have the Godot
  editor open.
- It needs no GDScript, so there is no UI to maintain across three engine
  versions — which was the strongest objection to the dock in the first place.
- The HTML is a resource served by the C++ server, so it stays on the side of the
  boundary that has tests.

**Recommendation.** When client support is broad enough to be worth it, expose a
status and log view as an MCP App. Treat this as *next*, not *now*: the extension
dates from January 2026 and host coverage is still spreading.

## What the editor still uniquely justifies

One case, and it is narrow: **when the MCP connection itself is broken, no
client-side surface can tell the user anything.** A person whose agent cannot
reach Didi has no way to distinguish "extension not loaded" from "no session
attached" from "engine output capture inactive."

That justifies a status line, not a dock. Log tails do not belong there — they
already have a home in `runtime_read_logs` and `runtime_read_output`, and a
client that can reach those does not need the editor.

**Recommendation.** Build this only if the broken-connection case is actually
reported by users. It is cheap, but it is not urgent, and every line of GDScript
is a line outside the tested boundary.

## What was built, and where it exceeded this

The condition was met in the strongest available form: the maintainer asked for
an in-editor console directly, which is a stronger signal than a bug report,
and asked for it to be dashboard-shaped. What shipped is larger than the status
line recommended above, and the differences are worth naming.

**A main screen, not a status line and not a dock.** The three states this
section named -- extension not loaded, no session attached, engine reachable --
are what the dashboard's lights distinguish, and each carries the path, pid or
session behind it plus a button for the next step. A status line can report the
first half and not the second. The main screen was chosen over the bottom panel
on evidence rather than taste: a bottom-panel button renders text only, and an
icon set on one is not drawn, including an icon taken from the editor's own
theme. The main screen is the only surface in Godot that shows a plugin's mark.

**It can close and open the bridge.** The endpoint belongs to the GDExtension,
so the switch unloads and reloads the extension and reports the status Godot
returns, including the one that means *not without a restart*. This is the one
place the console acts on the engine rather than reporting on it, and it acts on
the extension's lifecycle -- not through the tool surface.

**It has a log page, which this section argued against.** The objection was that
log tails already have a home in `runtime_read_logs` and `runtime_read_output`,
and a client that can reach those does not need the editor. That still holds,
and the console shows neither of them. Its two sources are the console's own
record of the state changes it watched, and the log Godot writes for the last
*run* of the project. Both are readable precisely when the MCP connection is
down, which is the case this section exists to serve, and neither is reachable
through a tool at all. Didi's own native logging goes to the process's standard
error, which an editor started from a desktop shortcut has nowhere to show; the
page says so rather than presenting an empty view.

**The GDScript cost is real and was not waved away.** The addon went from one
script to eight. What covers it: `tests/test_editor_console.py` holds the CMake
manifest, the CI staged-addon check and the demo copy to the addon directory
itself, proves every `preload` resolves, proves the shipped marks are
byte-identical to `docs/brand/svg`, and fails the build if any addon script ever
names the descriptor token field. Behaviour that Python cannot assert was
verified by running the addon in a fresh project on Godot 4.5.1, 4.6.2 and
4.7.2: the plugin loads, the mark rasterises, all five tabs build, and the
extension unloads and reloads without taking the editor with it.

## The prerequisite: protocol revision — done

Didi shipped MCP revision `2024-11-05` when this was written, and elicitation
and the modern result shapes required a newer one, so the revision upgrade
gated everything above. It has since been done: Didi is dual-era, serving
`2026-07-28` alongside `2024-11-05`.

The cost was lower than the changelog suggested, because Didi is a **stdio**
server and several breaking changes are Streamable-HTTP concerns:

| Change | Applies to Didi |
| :--- | :--- |
| `initialize` replaced by `server/discover` | Yes |
| Protocol metadata moves into `_meta` on every request | Yes |
| `resultType` on results (`complete` / `input_required`) | Yes — and this is the elicitation carrier |
| `ttlMs` and `cacheScope` on list results | Yes |
| `Mcp-Session-Id` removed | No — HTTP transport only |
| `Mcp-Method` / `Mcp-Name` request headers | No — HTTP transport only |

Didi already emits `annotations`, `outputSchema` and `structuredContent`, so it
is partly modernized already.

Both open questions from the original draft are now answered. `2026-07-28` is
final, published 28 July 2026 -- it was a release candidate when this was
written. And it deprecates Roots, Sampling and Logging, none of which Didi uses,
so that category cost nothing. Deprecated features carry a twelve-month minimum
window, so a staged migration remains comfortable.

## Recommended order, and where it stands

1. **Protocol revision upgrade.** *Done.* `server/discover`, per-request version
   negotiation, dual-era gating, and the modern result shapes.
2. **Elicitation for confirmation-gated mutations.** *Done.* A client that can
   ask a person is given the real dry-run preview to show them. A client that
   cannot is not silently downgraded: every mutation records whether it was
   confirmed by a `human`, by an `agent` echoing a token, or `skipped` entirely
   under YOLO mode.
3. **MCP Apps status and log view.** Still conditional on host support being
   broad enough to be worth it. Not started, and should not start on a schedule
   -- start it when a client people actually use can render it.
4. **Editor status line.** *Done, and larger than this list described.* Shipped
   as a main screen console: a red/amber/green dashboard over the published
   session descriptors, a switch that opens and closes the bridge, generated
   client configuration, a log page, and diagnostics that name the path or pid
   behind every check. See [What was built, and where it exceeded
   this](#what-was-built-and-where-it-exceeded-this).

Steps 3 and 4 were written conditionally on purpose. Step 3's condition -- host
support for MCP Apps being broad enough to be worth it -- has still not been met,
and building it anyway would be manufacturing work rather than following the
reasoning that produced the list.

## Explicitly out of scope

**Buttons in the editor that invoke tools.** That makes the editor a second MCP
client and doubles the surface that must be kept honest. This still holds and the
console holds it: its buttons load and unload the extension, write a client
configuration file, and run `didi --version`. None of them calls a tool, and the
console speaks no part of the IPC protocol.

**A settings UI that configures the server.** The original wording was "any
settings UI", and the console has a Settings page, so the line is restated rather
than quietly dropped. What it protects is that configuration has one source: the
project and the launch arguments. That is intact. The page composes the launch
arguments a client will start Didi with and shows the resulting configuration
before anything is written; the server still reads nothing but its own arguments
and the project. Preferences that are the editor's own live in `EditorSettings`
under `didi/`, stored with the editor rather than inside `res://`, so a
preference governing an assistant is not a file that assistant can rewrite.
Nothing the console stores is read by the server at runtime.

**Reimplementing tool logic in GDScript.** Whatever editor surface exists, the
rule holds: the UI renders, the extension decides. The console reads published
descriptors and reports them; where its view is weaker than the extension's it
says so rather than guessing. Liveness is the example: Godot can only answer
whether a process it started itself is running, so the console does not claim a
foreign session is alive or dead, and defers to the verification Didi performs
when a client attaches.

## Packaging note

`addons/didi/` is the distribution unit for the Godot Asset Library. A Godot
project cannot reference an addon outside its own `res://`, so the demo project
ships a copy; that copy had drifted two minor versions and is now held in line by
the documentation validator. Any editor-side work inherits that copy and should
not add a third.

The validator's parity check named three files by hand, which was fine while the
addon was three files. It now enumerates the addon directory instead, so a file
added to the addon is covered the moment it exists rather than the moment someone
remembers the list. The same directory is the reference for the CMake manifest
and for the staged-addon check in CI, and `tests/test_editor_console.py` fails
the build when any of the three disagree with it.
