# Didi API & Protocol Specification

This document defines the complete technical specifications for the JSON-RPC 2.0 Stdio transport, Model Context Protocol (MCP) data contracts, error codes, and internal Named Pipe IPC framing used by Didi.

---

## Experimental argument profile

A default-disabled build option adds explicit per-call argument normalization
for three read-only tools. Published input schemas remain canonical. Clients
must support the custom metadata and alternate inputs; existing client support
is not assumed. See the [wire contract](ELASTIC_INGRESS.md) and
[decision record](ELASTIC_INGRESS_DECISION.md).

## 1. JSON-RPC 2.0 Stdio Transport

Didi listens on `stdin` and responds on `stdout`. Log output is strictly routed to `stderr`.

### Framing

Didi accepts exactly one JSON-RPC object per line, terminated by `\n` or `\r\n`. HTTP-style `Content-Length` framing is unsupported. A detected `Content-Length` header returns `-32700` and closes the stdio session so a following payload cannot be interpreted as a separate request.

### Standard Request Schema:
```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "tools/call",
  "params": {
    "name": "query_project_resources",
    "arguments": {
      "search_path": "res://"
    }
  }
}
```

Requests require `jsonrpc: "2.0"` and a string `method`. When present, `id` must be a string or a number and must not be `null`, which is where MCP narrows JSON-RPC: `null` is how a response marks a request whose id could not be read, so a request carrying one can never be matched to its answer. `params` must be an object or array. JSON syntax/conversion failures, including numeric overflow, return `-32700`; a parsed value that violates this request shape returns `-32600` and echoes a legal request ID when available.

### Standard Success Response:
```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": {
    "content": [
      {
        "type": "text",
        "text": "..."
      }
    ],
    "isError": false
  }
}
```

### Standard Error Response:
```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "error": {
    "code": -32602,
    "message": "Params must be a JSON object"
  }
}
```

### JSON-RPC error codes

| Code | Constant | Description |
| :--- | :--- | :--- |
| `-32700` | `ParseError` | Invalid JSON received by the server |
| `-32600` | `InvalidRequest` | JSON payload is not a valid JSON-RPC 2.0 request |
| `-32601` | `MethodNotFound` | Requested method does not exist |
| `-32602` | `InvalidParams` | Method parameters are invalid or malformed |
| `-32603` | `InternalError` | Internal engine or server exception |

### Bridge error codes

The internal extension and local session envelopes can use `400` (invalid argument), `401` (runtime token rejected), `404` (missing object/property/session), `408` (cooperative expression deadline exceeded), `409` (protocol/mode/state conflict), `413` (bounded payload exceeded), `415` (unsupported expression result), `422` (parse/execution rejection), `423` (runtime session locked by another MCP client), `429` (this server already holds as many runtime sessions as it will), `500` (Godot/bridge failure), `501` (unimplemented), `503` (not connected/ready), or `504` (deadline exceeded). At the extension's 15-second main-thread deadline, a still-pending command is atomically cancelled and returns `outcome: "not_started"` with `route_quarantine: false`; a started but unresolved command returns `outcome: "unknown_outcome"` with `route_quarantine: true`. Public live tools and the runtime-log resource use a finite 17-second outer transport deadline. An explicit quarantine response or transport timeout quarantines that exact route; clients must not blindly retry a mutation whose outcome is unknown. A transport failure on a call that changes nothing is sent once more, on a new connection to the same session, before it is reported: repeating such a call is the same as making it, so asking again is what settles whether the engine ran it. The repeat happens before the quarantine, since quarantining retires the route. It happens once, not in a loop, and never for a mutation or for `editor_render_ghost_preview` with `replace: false`, which accumulates rather than replaces. A result that took two attempts carries `transport.repeats`, and a failure that was asked twice and answered neither time carries `transport.repeated` in `error.data`. A transport failure additionally carries `error.data.transport` with `request_started`, `outcome_unknown` and `timed_out`, plus `reason` and `waited_ms` when the cause was established. `reason` is `peer_closed` when the other end hung up, `deadline` when this side ran out of time, `io_error`, or `stopped`. The two are worth separating: a closed connection and an expired deadline used to share one message and one false `timed_out` flag, so a failure could report a timeout it had not had. `waited_ms` is how long that operation actually waited, which is what distinguishes an operation ended by its own deadline from one ended by something else. A transport failure on a route with a known session also carries `error.data.engine`: `alive`, `gone`, or `unknown`. It answers the question the transport itself cannot, since a peer closing the pipe does not say why it went, and an engine that crashed reads identically to one that is alive and merely stopped answering. `unknown` is a real answer and is kept distinct from `gone`, because a process that could not be queried is not a process that has ended. It also says why: `error.data.engine_reason` is `open_denied` when the process could not be opened for a reason other than there being no such process, with the operating system's own number in `engine_os_error`, or `running_but_unidentified` when something with that pid is running and could not be confirmed as the process the session opened. `unknown` on its own leaves a reader where they started, because an engine that crashed and a query that was refused are different problems. The check compares the recorded start time as well as the pid, so a recycled pid reads as `gone` rather than as the session that used to own it. Public `tools/call` converts these failures into MCP content with `result.isError: true`; clients should use the returned text and structured error data rather than expecting a top-level JSON-RPC code.

### Refusal remedies

A refusal names what fixes it, in `error.data`:

| Field | Meaning |
| :--- | :--- |
| `retry_with` | The arguments to send again, as an object to merge into the call. |
| `field` | The argument at fault, by name. `argument`, `parameter`, `missing` and `did_you_mean` say the same thing on the refusals that already used them. |
| `next_call` | `{"tool", "arguments", "reason"}`: a call to make first, one that changes the state the refusal was about. `arguments` is present when the fix knows them. |
| `restart_with` | What the server or its environment needs at startup, which no call can change. |
| `retry_after_ms` | A transient state: the same call, after about that long. |
| `no_remedy` | Why nothing the caller can send fixes it, so the call is not retried. |

A call site that knows the remedy says it, and the error floor leaves it
alone. Where a site said none, the floor fills the remedy its `code` carries,
chosen by the tool where one code means different things in different tools,
and by what the message names where one tool can miss more than one kind of
thing: a `not_found` about a node names `scene_get_hierarchy`, about a file
`project_list_resources`, about a blackboard key `blackboard_list_keys`, and
about an evicted capture `viewport_capture_frame`. A failure with status 500 or above other than 503
and 504 is a fault in the server or what it read, and carries none.

`didi --dump-tool-manifest` publishes the table as `refusals.remedied`, and
`refusals.without_remedy` with the reason for each code that has none. CI holds
it against every code the source emits, and the live harness fails any refusal
it sees without one of the fields above. Every failure is an envelope: a tool
cannot answer one as plain text, which would have no `error.data` at all.

When the same call fails the same way twice in a row, the second refusal says
so in `error.data.repeated`: `count`, how many times in a row, `follow`, the
remedy field above to act on, and a `note`. The same call is the tool and the
arguments as sent, less `confirmation_token`; the same way is `data.code`. A
success of that call, or a different failure, starts the count again. A
repeated call is never refused, because polling is legitimate. A `2024-11-05`
client is counted across the process; a modern request within the runtime
session it named.

### Follow-ups

A successful mutation that leaves work undone names it under `follow_up`, a
list of steps:

| Field | Meaning |
| :--- | :--- |
| `work` | `save`, `restart`, or `poll` for a job still running (see [Jobs](#jobs-and-the-tasks-extension)). |
| `tool` | The call that does it, when one can: `editor_save_scene` for a save. Nothing Didi sends restarts an editor, so a restart names none. |
| `reason` | What is undone, in a sentence. |

Every step comes from a fact the answer already carries, and the prose beside
it stays:

- `scene_saved: false`, on every live edit of the open scene, is a save. None
  is named when managed recovery's receipt says it saved the scene.
- `requires_editor_restart: true`, on a new or removed autoload and on a bus
  layout moved under an attached editor, is a restart.
- `project.godot` written with no editor attached (`written_to`, offline) is a
  restart, and so is `export_presets.cfg` written with `editor_reloaded: false`.

An answer that names its own `follow_up` is left as it is. There is no rescan
step: a file written behind an attached editor leaves the editor's cached copy
stale, and a rescan does not refresh it (#1047).

`tests/follow_ups.json` says, for every mutating tool, the work it can leave,
why it leaves none, or the issue that tracks what it does not say yet. CI fails
a mutating tool with no entry, and the live harness fails a step a tool does
not declare and a fact that arrives without its step.

---

## 2. MCP Methods

Didi is **dual-era**, serving both `2026-07-28` and `2024-11-05`. A legacy
client opens with `initialize` and is served legacy semantics; a modern client
declares its version in `_meta["io.modelcontextprotocol/protocolVersion"]` on
every request and is served statelessly. A request carrying a supported version
is self-contained and needs no prior `initialize`.

`initialize` reads the `protocolVersion` it is sent. The field is a required
string, so a missing key or a non-string is refused with `-32602`, carrying
`supported` and `requested` in `error.data`. A revision this server serves is
answered with itself; any other string is answered with `2024-11-05`, which is
how a client tells a version it was granted from one it was refused.

`initialize` is accepted once. A second `initialize` on an initialized session
is refused with `-32600` and `error.data.initialized: true`, whatever
`clientInfo` it carries, and the session goes on serving the client that
opened it: nothing is renegotiated and no confirmation token is dropped. A
client that wants a fresh session starts a fresh server process.

A modern request must carry `_meta["io.modelcontextprotocol/clientCapabilities"]`
as well as the version, and its `id` must be a string or an integer. A request
that declares a version and leaves out the capabilities, or sends a null or
fractional `id`, is refused with `-32602` before the method runs. Capabilities
are read from the request that carries them and never from an earlier one, so
two clients sharing one process cannot see each other's declarations.
`server/discover` is exempt: it is how a client finds out what the server
speaks, so it answers whatever it is sent.

### Server operational instructions

Successful `initialize` replies include the MCP `InitializeResult.instructions`
string at `result.instructions`, beside `protocolVersion`, `capabilities` and
`serverInfo`. `server/discover` returns the same string at `result.instructions`,
including when called before initialization. It is not a capability flag, tool
result, prompt resource or member of `_meta`.

The guide covers canonical scene-tree/property, project-setting and GDScript
routes; schema and availability inspection; discovery before node access;
mutation previews; and boundaries with direct file inspection and
`godot --headless` fallbacks. It forbids brute-force node probing. Rendering and
gameplay remain Godot's responsibility, and binary asset editing is outside the
server's general-purpose surface; the advertised snapshot/runtime tools still
retain their documented capabilities.

Hosts should make the returned guide available to the consuming model before
planning tool calls. It is static guidance, not a session selection or a
promise that a tool is currently live. `tools/list` input schemas and
`_meta.didi` remain authoritative; modern clients must still send the runtime
session metadata described below. The guide is identical across clients,
projects and UI/confirmation modes and contains no project-specific secrets.

This is an additive current-source/Unreleased field; older installed builds may
omit it. The field does not change version negotiation, duplicate-initialize
refusal, tool names, schemas or tool counts. The expanded human-readable guide
is [LLM Operating Instructions](LLM_INSTRUCTIONS.md); the shared compiled text
is `kServerInstructions` in `src/mcp/mcp_server.cpp`.

### Naming the runtime session

A Godot session is state that spans requests, so a modern request names the one
it means in `_meta["didi"]["runtime_session_id"]`. Discover the ids with
`runtime_list_sessions`.

A modern request is only ever served on the session it named. Naming nothing
gets no live route at all: a tool that can answer offline does and says
`offline_fallback` when it had a live path to fall back from and `local` when
it never did, and a live-only tool is refused with `400` naming the field
to set. Availability follows the same rule, so `tools/list` will not report a
tool live because an unrelated request opened a route to an editor.

`resources/read` is scoped the same way. `godot://editor/state` and
`godot://runtime/logs` read from a live session, so a modern read that named no
session gets the offline payload rather than another task's editor.
`godot://project/journal` reads its file either way, and only the judgement of
which entries can still be undone needs the session.

Naming a session the server is not already routed to opens a route to it. That
route is held alongside any others, so two tasks interleaving requests on one
stdio process each drive their own editor and neither sees the other's. Opening
a route does not move the process selection, so a legacy client keeps whatever
it attached.

One process holds at most eight routes at once. Each one keeps a connection and
an ownership lock, and a lock held here is a session refused to every other Didi
process, so the count is bounded rather than left to grow. Routes whose engine
has gone are dropped before the limit is consulted; if the limit is genuinely
reached the request is refused with `429` naming what to do. Every held route is
released on shutdown, not only the selected one.

Legacy clients are unchanged. `runtime_attach_session` still sets the process
selection and later legacy requests still inherit it, which is the lifecycle
that era was written against. Detaching releases the selected route and leaves
any others alone.

`server/discover` reports the supported versions without a handshake, because it
is the probe a modern stdio client sends first. A version Didi does not serve
returns `-32022 Unsupported protocol version` carrying the list to retry with,
rather than silence.

Discovery advertises only revisions Didi actually serves. That is enforced
rather than asserted: a test drives a real request at every version discovery
advertises and requires it to succeed, so the list cannot outrun the
implementation.

### Human confirmation

Didi's confirmation tokens bind intent to exact arguments, project and route.
That is a real property, but the *agent* receives the token and echoes it back,
so on its own confirmation means the agent confirming to itself.

When a client declares the `elicitation` capability, a confirmation-gated tool
called without a token returns an `input_required` result instead of `428`:

- `inputRequests` carries an `elicitation/create` in form mode. The message names
  the tool and the target, and the schema is a single `confirm` boolean.
- `_meta.didi.mutation_preview` carries the same dry-run preview the tool would
  return, so a client can show a person the tool, the target and the project it
  is bound to. The preview does not open the target, so it names what would be
  changed and not what the change would be; `preview_kind` says so.
- `requestState` binds the offer to the tool it was minted for. A retry naming a
  different tool is refused, so one approval cannot authorise a different act.

The client reissues the call with `inputResponses`. `accept` executes; `decline`
and `cancel` both refuse and stay distinguishable, because an agent that cannot
tell refusal from dismissal will retry the one it should not. Both answer `403`
with `data.code: "not_approved"` and `data.action`. A decline carries
`no_remedy` and `retryable: false`; a dismissal was never answered, so it
carries `retry_after_ms: 0` and `retryable: true`.

**A client that cannot elicit is not silently downgraded.** The specification
forbids sending a mode the client did not declare, so the token flow remains and
still returns `428`. What Didi will not do is let that path look like human
approval: every confirmed mutation records `_meta.didi.confirmation` as `human`
or `agent`, so a caller can tell what the confirmation was actually worth.

### YOLO mode

An unattended agent cannot answer an elicitation, and the dry-run/echo-token
dance is friction with no safety value once a human has decided to let it run
alone. `--yolo` (or `DIDI_YOLO=1`) turns the confirmation requirement off.

It is a **launch flag only**. Nothing reachable from a tool call can set it: an
agent that can authorise its own bypass makes the confirmation system
decorative, and a test asserts no tool exposes such an argument.

What it does *not* change: the explicit project root, session authentication,
mutation classification and `annotations`, or validation. Skipping confirmation
is not skipping checks -- a call that could not run still reports why.

It is visible in four places, because a gate that is open quietly is worse than
one that is closed:

- A warning at startup.
- `initialize` and `server/discover` both report
  `_meta.didi.confirmationsSkipped`, so a client can see the gate is open
  *before* it acts rather than after. Both, because `server/discover` is a
  method a 2024-11-05 client never calls, and for such a host the whole
  published surface used to be identical in either mode.
- Every tool entry in `tools/list` carries `_meta.didi.confirmationsSkipped`
  beside `currentMode`, which is where a host looks when it is deciding whether
  to put its own guard in front of a destructive call. Under `--tools core` the
  listing states it once, in its own `_meta.didi` (see
  [Tool profiles](#tool-profiles)).
- Every affected result records `_meta.didi.confirmation` as `skipped` --
  distinct from `human` and `agent`, because nobody confirmed anything.

### Result shapes and caching

Every result carries `resultType`. Cacheable operations also carry `ttlMs` and
`cacheScope`, and the values are deliberately conservative:

| Operation | `ttlMs` | `cacheScope` | Why |
| :--- | :--- | :--- | :--- |
| `server/discover` | 3600000 | `public` | Supported versions, capabilities and identity are compile-time constants |
| `prompts/list` | 3600000 | `public` | Prompt definitions carry no session state |
| `resources/templates/list` | 3600000 | `public` | A URI shape does not depend on the attached session or on which boards exist |
| `tools/list` | 0 | `private` | Embeds live availability, which flips when an editor starts or stops |
| `resources/list` | 0 | `private` | Same live availability metadata |
| `resources/read` | 0 | `private` | Live project and editor state |

A `ttlMs` of `0` means immediately stale, and is the honest value wherever a
result reflects the current session. A freshness window there would let a client
keep reporting a tool unavailable long after it became available -- a cache that
serves a stale claim is worse than no cache.

| Method | Direction | Description |
| :--- | :--- | :--- |
| `server/discover` | Client to Server | Reports supported protocol versions, capabilities, server identity and operational `instructions`. Answers without a handshake, since it is the probe a modern client sends first. |
| `initialize` | Client $\rightarrow$ Server | Initializes the session, returns operational `instructions`, and advertises implemented tool, resource, and prompt capabilities. Logging is omitted until `logging/setLevel` exists. |
| `notifications/initialized` | Client $\rightarrow$ Server | Notification acknowledging initialization |
| `ping` | Client $\rightarrow$ Server | Liveness check; returns `{"resultType":"complete"}` |
| `tools/list` | Client $\rightarrow$ Server | Lists all registered tools with JSON input schemas and Didi capability metadata |
| `tools/call` | Client $\rightarrow$ Server | Executes a tool by name with arguments |
| `resources/list` | Client $\rightarrow$ Server | Lists all available static and dynamic resources |
| `resources/templates/list` | Client $\rightarrow$ Server | Lists the parameterised resource shapes, currently `blackboard://{board}/state` and `blackboard://{board}/tasks` |
| `resources/read` | Client $\rightarrow$ Server | Retrieves contents of a specific resource URI (`godot://...`), the change journal `godot://project/journal` among them |
| `prompts/list` | Client $\rightarrow$ Server | Lists all registered prompt templates |
| `prompts/get` | Client $\rightarrow$ Server | Evaluates a prompt template with provided arguments |
| `tasks/get` | Client $\rightarrow$ Server | Reads a task this server answered a `tools/call` with, and its result once completed. See [Jobs and the tasks extension](#jobs-and-the-tasks-extension). |
| `tasks/cancel` | Client $\rightarrow$ Server | Asks a task to stop, and acknowledges; the helper it runs is killed with everything under it |
| `tasks/update` | Client $\rightarrow$ Server | Acknowledges and does nothing, since no task here asks for input |

Only `notifications/*` methods may omit `id`. Request-only methods such as `tools/call`, `resources/read`, and `prompts/get` are ignored when sent as notifications and cannot execute mutations. For calls and prompt retrievals, `name`/`uri` must be strings and an `arguments` member, when present, must be an object; violations return `-32602` without terminating the server. `prompts/get` arguments are closed the way tool arguments are: an argument the prompt does not declare returns `-32602` naming the property, with `argument`, `prompt` and the accepted names in `data`. The `description` on a `prompts/get` result is the same one `prompts/list` publishes for that prompt.

A `tools/call` naming a tool no registration carries returns `-32602` with the name, which the specification calls a protocol error rather than a tool result. `tools/list`, `resources/list` and `prompts/list` each answer in one page and return no `nextCursor`, so this server issues no cursor; a `cursor` sent to any of them is refused with `-32602`.

### Didi capability extension

Each tool and resource definition includes a namespaced `_meta.didi` object:

```json
{
  "executionModes": ["live"],
  "implemented": true,
  "currentMode": "live",
  "liveAvailable": true,
  "editorConnected": false,
  "sessionKind": "game"
}
```

`executionModes` and `implemented` describe the registration. `currentMode`, `liveAvailable`, `editorConnected`, and optional `sessionKind` are evaluated when the list request is handled. `sessionKind` identifies the selected `editor`/`game` route. `editorConnected` is true only for a connected editor route. `liveAvailable` additionally requires that the exact tool/resource allow the selected kind: runtime logs/tree/evaluation allow both kinds, pause/step/stop are game-only, and other live definitions are editor-only by default. A connected wrong-kind definition reports `currentMode: "unavailable"`; otherwise `currentMode` is `live`, `offline_fallback`, `local`, `local_status`, `local_session_management`, `unavailable`, or `unimplemented`. `currentMode` and `executionModes` use the definition's own answer vocabulary, so a tool or resource with no live path advertises `local` (or `local_status`, or `local_session_management`) rather than `offline_fallback`; only one that can go live advertises the fallback. The same word appears in the answer, because both come from the registration. A `resources/list` entry additionally carries `subscribable`, which says whether `resources/subscribe` accepts that URI. A non-empty `reason` is included for unimplemented definitions.

### Tool profiles

`--tools full|core` chooses, at startup, which tools a session lists. It is
fixed for the life of the process, because a tool list that changed mid-session
would throw away the client's prompt cache. `initialize` and `server/discover`
report the choice as `_meta.didi.toolProfile`.

- `full` is the default and the published surface: every canonical tool and
  every legacy name, each with the `_meta.didi` object above. Its listing
  carries no listing-level `_meta`.
- `core` lists the tools the field trials reached, recorded in
  `tools/field-trial/reached_tools.json`, plus every implemented tool this
  server's handshake guide names, so a core session can follow its own guide.
  It lists no legacy name. `confirmationsSkipped`, `editorConnected` and
  `sessionKind` are the same on every entry, so a core listing states them once
  in the listing's own `_meta.didi`, beside `toolProfile: "core"`, and its
  entries omit them. Every other `_meta.didi` field stays on the entry.

A `tools/call` naming a registered tool the session's profile does not list is
refused with `-32602`, like any name the client was not shown, but its message
and `error.data` say why: `name`, `toolProfile`, and `restart_with:
"--tools full"`. It is `restart_with` and not `retry_with`, because no argument
to the call can fix it. The published counts and `didi --dump-tool-manifest`
describe the full surface whatever a session lists; the manifest carries the
core names as `names.core`.

Each profile's `tools/list` has a byte budget in `tests/tool_list_budgets.json`,
checked in CI. The full profile's per-entry `confirmationsSkipped`,
`editorConnected` and `sessionKind`, and its legacy names, stay until a major
version, because hosts read them where this document promises them.

Tool execution failures use MCP `result.isError: true` with explanatory text. JSON-RPC top-level errors remain reserved for malformed requests, unknown JSON-RPC methods, and other protocol-level failures.

### Response economy

A client that reads `structuredContent` can decline what it already has by
declaring the `didi/responseEconomy` extension in its capabilities. A
`2024-11-05` client declares it in `initialize`, and it then applies to every
legacy request on the process; any request may also declare it in its own
`_meta["io.modelcontextprotocol/clientCapabilities"]`, for that request. A
modern request is answered from its own declaration alone, so a legacy
handshake on the same process never changes a modern request's answer. This is
where the `io.modelcontextprotocol/ui` extension is negotiated too.
`server/discover` and `initialize` declare the extension with every value this
server honours:

```json
"extensions": {"didi/responseEconomy": {"omit": ["textCopy", "sessionDescriptor"]}}
```

| `omit` value | Effect on a successful `tools/call` result |
| :--- | :--- |
| `textCopy` | A `content` text item that is byte for byte the serialised `structuredContent` is left out. Anything else in `content`, such as an image or its caption, stays. A result with nothing else has `content: []`. |
| `sessionDescriptor` | A live answer's `session` is `{"session_id", "kind"}` when the client already holds that descriptor, and the whole descriptor when it does not. A legacy client holds it once an answer to a request that declared `sessionDescriptor` carried it whole, until anything in it changes, so an editor restart sends the new one. A modern request holds the session it named in `_meta.didi.runtime_session_id`. The same rule, and the same record of what was sent, applies to the payload of a live `resources/read` (`godot://editor/state`, `godot://runtime/logs`). |

A value the server does not know is ignored, so a client written against a
later Didi still gets what this one can give, and a declaration of any other
shape declares nothing. `runtime_list_sessions`, `runtime_attach_session`,
`runtime_detach_session` and `runtime_get_session` always answer with whole
descriptors, since the descriptor is their answer; `runtime_get_session` is how
a client that dropped one gets it back. A failed call (`isError: true`) is
never reshaped: its text and its session provenance are what a caller most
needs whole.

A client that declares nothing gets the bytes it got before the extension
existed. The live harness runs the seven-call authoring arc #776 measured both
ways on every engine line, and fails when the declared arc is not under half the
undeclared one's bytes.

A host that cannot declare an extension can still have the descriptor half.
`--session-descriptor once` applies `sessionDescriptor` to every request on the
process, declared or not, with the same holder rules; `every`, the default,
sends the whole descriptor on every live answer. It never leaves out the text
copy, which only a client can decline. The mode is fixed at startup, and
`initialize` and `server/discover` report it as `_meta.didi.sessionDescriptor`.

### Bounded reads and fields

Every read-only tool is either bounded or unbounded, and
`tests/bounded_reads.json` says which, with what bounds it or why nothing
does. A bounded read is one whose answer an argument (`max_results`,
`max_nodes`, `limit`) or an internal limit (an index cap, an output cap, a time
limit) can cut short. Every successful answer from one carries a boolean
`truncated`, whatever path answered it: `true` when a bound was reached, which
can include an answer that filled a bound exactly, and `false` otherwise. A
short answer never looks like a whole one. The narrower flags a tool already
had, such as `log_truncated`, `output_truncated`, `children_truncated` and
`known_groups_truncated`, stay beside it and say which bound it was. A
`runtime_read_logs` or `runtime_read_output` page is `truncated` when it has
more to page, when records before its cursor were evicted, or when a record
was cut when written; such a record carries `message_truncated: true`.

Two large reads are made of sections a caller often wants one of, and take
`fields`, an array of section names published as an enum in their input
schema:

| Tool | Sections |
| :--- | :--- |
| `didi_control_room` | `server`, `project`, `surface`, `facts`, `tools`, `sessions`, `log` |
| `script_reflect_class` | `description`, `methods`, `properties`, `signals`, `enums` |

The answer keeps the named sections and every key that is not a section, and
names the sections it left out in `omitted_fields`, in the order above. A key
the output schema requires is never a section, so `didi_control_room` always
returns `lights`. An unknown name, an empty list or a repeated name is refused
by the schema. A read shaped as one list, such as `project_list_resources` or
`scene_get_hierarchy`, is narrowed with its bounds and filters instead.

### Jobs and the tasks extension

Q8 in the [Build Queue](BUILD_QUEUE.md#q8-long-work-as-jobs). The server answers
one request at a time, and `project_export` and `csharp_check_build` run a
helper process that can take minutes, during which nothing else was answered.
`asset_reimport` waits for the editor to apply a scan, which can take minutes on
a slow editor, past the fifteen seconds the bridge waits for any one command.
Each can now run as a **job**: the same tool call, through the same pipeline,
on a thread of its own, with its answer kept.

**`request_id`.** All three tools take an optional `request_id`, 8 to 64 letters,
digits, `.`, `_`, `:` or `-`, chosen by the caller. A call that carries one runs
as a job and waits up to about ten seconds for it:

- Finished in that time, the answer is exactly what the call answers without a
  `request_id`, with the job named under `_meta.didi.job`
  (`job_id`, `request_id`, `state`, `started_at`).
- Still running, the answer is a success, `{"status": "working", "job": {...},
  "follow_up": [{"work": "poll", ...}]}`, with `elapsed_ms` and
  `poll_interval_ms` in `job`.
- The same call again with the same `request_id` reads the job and never runs
  the work a second time: the working answer while it runs, the stored answer
  once it finishes. The `request_id` alone, with no other arguments, reads it
  too. The same `request_id` with different arguments is
  `409 request_id_conflict`, and runs nothing.
- A job cancelled through `tasks/cancel` reads as `409 job_cancelled`. A job
  whose work threw reads as `500`.

The `request_id` is not an argument of the work. It is removed before anything
else reads the call, so a dry run, its confirmation token and the confirmed call
bind to the same arguments, and a repeat is matched before a single-use token is
spent again. A call with neither a `request_id` nor the extension below behaves
exactly as it did.

**The tasks extension.** A `2026-07-28` request that declares
`io.modelcontextprotocol/tasks` in its own
`_meta["io.modelcontextprotocol/clientCapabilities"].extensions` may be answered
with a task when it calls one of these tools: `resultType: "task"`, with
`taskId`, `status`, `createdAt`, `lastUpdatedAt`, `ttlMs` and `pollIntervalMs`.
The work is given a quarter of a second first, so a call refused on its
arguments is answered at once. `tasks/get` answers the task's state, and once it
has `status: "completed"`, the call's answer under `result`. `tasks/cancel`
acknowledges, stops the helper process and everything under it, and the task
ends `cancelled`. `tasks/update` acknowledges and does nothing, because no task
here asks the client for input. An unknown or expired `taskId` is `-32602`.
`server/discover` declares the extension; `initialize` does not, because the
extension is negotiated per request and a `2024-11-05` request carries no
capabilities to negotiate it with. A request that did not declare it never
receives a task.

**Limits.** At most four jobs run at once; a fifth is `429 rate_limited` with
`retry_after_ms`, and nothing starts. A job is kept an hour from its creation,
and at most 64 are kept, the oldest finished one going first. Jobs are kept in
the server process: they do not survive it, and a server that exits cancels the
jobs still running. Task ids are 128 random bits, since a stdio server has no
authorisation context to bind a task to. No notification is sent about a job; a
client polls. Under [managed recovery](MANAGED_RECOVERY.md) a job's call waits
for its job to finish, because a mutation there passes through recovery steps
written for one call at a time; its `request_id` still reads the kept answer.

**`asset_reimport` as a job.** Its `timeout_ms` goes to 900000 as a job, and
defaults to 300000 there; without a job it stops at 10000, and a larger value is
`400 reimport_needs_job` with `field: "request_id"`. The job sends the bridge
`asset.reimport` with `detach_timeout_ms`, and the bridge answers at once with
`status: "accepted"` and a `reimport_id` instead of waiting. The job then reads
`asset.reimportStatus` with that id every quarter second. The bridge answers
that read on its IPC thread, never the editor's main thread, because the editor
holds every queued command while its progress dialog is open, and the dialog is
open for exactly the work the job waits out. The read answers `state: "working"`
or `state: "finished"` with the reimport's answer, which is the job's answer
with `reimport_id` added; an id the editor does not keep is
`404 reimport_not_found`. A detached reimport nobody has read for 60 seconds
stops being waited for, and its answer is `504 reimport_unread`, so a server
that exited mid-job does not hold the one reimport slot. A bridge older than
detaching ignores `detach_timeout_ms` and answers within the `timeout_ms` of
10000 or less that the job also sends, and that answer is the job's.

**Helper launches run side by side.** A Godot that Didi starts to answer a
question, for a check, a verification or an export, is told not to publish a
runtime session through a variable set in that child's own environment. It used
to be set on the server process under a lock held for the length of the run, so
while an export job ran, every other tool that starts a helper Godot waited for
it. Nothing waits for another launch now.

### Mutation safety extension

Every implemented mutating tool schema includes `dry_run: boolean`. With `dry_run: true`, dispatch stops at the registry boundary and returns `dry_run: true` plus `mutation_preview`; no mutation handler or external process executes. The preview is bound to the tool, sanitized arguments, canonical project, execution mode, optional session ID, and route generation.

The preview reads its target where it can, and runs the argument checks the real call runs before it reads anything, so a dry run against a target that is not there, or with an argument the real call refuses, fails the way the real call would rather than returning a clean preview. `target_read` says which happened, and `preview_kind` is `target_state` when the target was read and `argument_binding` when it was not. A read preview reports `changes[].kind: "planned_mutation"` with `changes[].before` holding what is there now: the file's existence, size and content digest, the property's current value, or the setting's current literal. A probe that only resolved its node, because the tool changes no property of it, reports `changes[].kind: "resolved_target"` with `before.resolved: true` rather than standing `name` in for a before state. `changes[].target` names the subject of the change, not the argument object, which is at `mutation_preview.arguments`. Every preview publishes `max_response_bytes` (8 MiB) and `truncated`; when the cap trips, oversized values are replaced with the byte count that stood there and the elision says so. The token is bound to the real arguments, not to the displayed copy, so an elision cannot fail a confirm. An unread one reports `changes[].kind: "unverified_mutation"` and says so in `before`, because nothing was planned when only the arguments were hashed. A tool whose subject is a plan rather than a file reads it the same way: `project_rename_references` runs its scan and its plan in the preview, so `before` holds the files it will rewrite and the line counts, and the confirmation is bound to that plan. A target is unread when the tool has no probe, or when the engine could not be reached; an unreachable engine is not evidence the mutation would fail, so the preview continues rather than refusing a call that may be fine. Reading a live target sends one read-only `scene.getProperty`, which names the mutation being previewed so the bridge runs that call's own preconditions on the node first, such as whether the scene file can hold the edit or whether a script's base type fits, and refuses with the code the real call gives; nothing a dry run does changes anything. For `scene_set_property` that check is the write's whole check, the type of its value included, and a batch in `writes` is read in one `scene.getProperty` with `reads`, so its preview holds each target's current value under `changes[0].before.writes`.

`runtime_restore_checkpoint`, `editor_reload_project`, `script_patch_method`/`patch_script_symbols`, `signal_emit`, `project_rename_references`, `project_apply_changes`, and `blackboard_clear` always require confirmation. `resource_create`, `script_create`, `viewport_create_test_lab`/`create_visual_test_lab`, `project_export`, and `gridmap_export_mesh_library` require confirmation when `overwrite: true`. The preview's `confirmation_token` is 64 lowercase hexadecimal characters, expires after 120 seconds, is consumed on its first validation attempt, and fails on any argument/context mismatch or replay. Where the preview read a target, the confirm reads it again and refuses with `409` and `data.target_changed` when it no longer matches what the preview saw; `target_checked_on_confirm` in the preview says whether that comparison will be possible. A target that has been removed is not such a change and does not invalidate the token. A request must not combine `dry_run: true` with `confirmation_token`. The startup-only YOLO override is described above.

### Managed recovery extension

Opt-in `--managed-editor` and `--recovery-workspace` startup enables four standalone host tools: `runtime_recovery_status`, `runtime_checkpoint`, `runtime_recover_editor`, and `runtime_restore_checkpoint`. They advertise `local` and return a disabled error in ordinary attachment mode. The host owns only its child editor, uses a new project copy, and refuses manual attach/detach. See [Managed Recovery](MANAGED_RECOVERY.md) for startup validation and saved-file coverage.

Protected mutation results and selected managed-recovery error responses carry compact `recovery` receipts containing `state`, `requires_reconciliation`, `operation`, `checkpoint_id`, `coverage`, and `unprotected_changes`. Successful ordinary reads do not carry this receipt. Query `runtime_recovery_status` for authoritative recovery state, especially after a successful read that may have triggered a restart. Full `runtime_recovery_status` fields are `enabled`, `state`, `editor_running`, `checkpoints`, `pid`, `session_id`, `workspace`, `journal`, `automatic_restart_used`, `requires_reconciliation`, `last_operation`, `last_checkpoint`, `coverage`, `unprotected_changes`, `excludes`, and `next_action`.

Before an ordinary authorized request dispatches, `ensureEditor()` may consume the single automatic restart after an abnormal owned-child exit. This includes ordinary reads and can execute project startup code. If recovery changes the route for a pending mutation, that mutation does not start; inspect the recovered state before submitting a fresh request. `runtime_recovery_status`, dry runs, and confirmation previews do not relaunch. Dry runs execute no recovery mutation, checkpoint, or restore.

An uncertain edit is never replayed. `not_started` means dispatch did not begin; a live/transport `unknown_outcome` means the edit may have applied, summarized as `failed_or_unknown` in the recovery operation. Applied edits whose protection fails report `applied_persistence_failed`, `applied_checkpoint_failed`, or `applied_journal_failed`, and answer with an `error` beside the tool's own answer and the receipt: status 500, `data.outcome` naming which, `retryable: false`, and a `next_call` that reads the recovery status (#1043). Successful protection records `completed_saved_files_checkpointed`. `requires_reconciliation` blocks further mutations until explicit reconciliation: restore a checkpoint, or inspect and accept saved files using `runtime_checkpoint` with `accept_current_files: true` only after the uncertain original editor is proven stopped. A running replacement editor alone does not clear this latch. See the [Tool Reference](TOOL_REFERENCE.md#managed-editor-recovery) for lifecycle states and reconciliation outcomes.

`runtime_restore_checkpoint` requires a listed `checkpoint_id` and the confirmation token from a dry-run preview taken on the same arguments. It verifies/stages the checkpoint, stops only the owned editor, preserves the prior project, installs the copy, and launches an editor. Restore does not reset the automatic restart budget; only a new managed host invocation does, and it must use a fresh workspace.

---

## 3. Internal IPC Protocol (Named Pipes & UNIX Sockets)

- **Session descriptor directory**: Windows `<OS temporary directory>/didi-sessions`; POSIX `$XDG_RUNTIME_DIR/didi-sessions` when `XDG_RUNTIME_DIR` is absolute and set, otherwise `<OS temporary directory>/didi-sessions-<euid>` (controlled override: `DIDI_SESSION_DIR`; override access controls are operator-managed)
- **Pipe Name (Windows)**: `\\.\pipe\godot_didi_<16-hex-project-key>_<pid>_<32-hex-session-id>`
- **Security Descriptor (Windows)**: SDDL `D:(A;;GA;;;BA)(A;;GA;;;OW)` (local administrators and the owning SID; not strictly owner-only)
- **Socket Path (POSIX)**: `<OS temp>/godot_didi_<16-hex-project-key>_<pid>_<12-hex-session-prefix>.sock`, with owner-only permissions
- **Client lock**: `<session-directory>/<32-hex-session-id>.lock`, held with an OS exclusive lock by one MCP client; ownership metadata contains no session token

### Frame Format:
```
Offset 0..3:  uint32_t payload_length (Little-Endian, Max 128 MB)
Offset 4..N:  char payload_bytes[payload_length] (UTF-8 JSON string)
```

Request IDs are correlated exactly. A missing or mismatched response ID closes the client route and reports an unknown-outcome transport failure. Reconnect, write, and read share the request's single deadline on Windows and POSIX. Once a request is parsed, an extension handler exception returns internal error `500` with the original request ID; malformed JSON remains a `400` framing/request error.

### Implemented internal methods

- `session.handshake`

- `editor.getState`, `editor.getRecoveryState`, `editor.getSelection`, `editor.openScenes`
- `scene.getHierarchy`, `scene.instantiateNode`, `scene.removeNode`, `scene.reparentNode`, `scene.setProperty`, `scene.getProperty`, `scene.duplicateNode`, `scene.callMethod`
- `script.attachToNode`, `script.detachFromNode`
- `project.listAutoloads`, `project.setAutoload`, `project.removeAutoload`
- `project.listInputActions`, `project.setInputAction`, `project.removeInputAction`
- `project.getSetting`, `project.setSetting`, `project.resolveUids`
- `engine.classExists`
- `scene.listGroups`, `scene.addToGroup`, `scene.removeFromGroup`, `scene.getGroupMembers`
- `scene.create`, `scene.open`, `scene.close`, `scene.packBranch`
- `editor.undo`, `editor.redo`, `editor.saveScene`, `editor.reloadProject`, `editor.getProtocolServers`
- `asset.reimport`, `asset.readImportedStream`, `resource.refreshCached`, `export.reloadPresets`
- `audio.listBuses`, `audio.configureBus`, `audio.addBus`
- `signal.listConnections`, `signal.connect`, `signal.disconnect`, `signal.emit`
- `vision.captureViewport`, `vision.diffViewport`, `vision.capturePasses`, `vision.setCameraTransform`, `vision.toggleDebugDraw`, `vision.frustumQuery`
- `preview.renderGhost`, `preview.clearGhosts`
- `physics.raycast`, `physics.raycastBatch`, `physics.clearance`, `nav.queryPath`
- `anim.listTracks`, `anim.playTrack`, `anim.addLibrary`
- `tilemap.setCells`, `tilemap.getUsedRect`, `gridmap.setCells`
- `shader.listUniforms`, `shader.setUniform`, `shader.getVisualGraph`
- `ui.hitTest`, `ui.listControls`
- `runtime.getLogs`, `runtime.getOutput`, `runtime.getTree`, `runtime.setPaused`, `runtime.step`, `runtime.stop`
- `runtime.evalGdscript`, `runtime.injectInput`, `runtime.readProfiler`, `runtime.watchInvariants`, `runtime.exploreScene`

Which kind of session may run a method is one table, `livePolicyForMethod` in `include/didi/runtime/session_kind_policy.hpp`, checked as the command is taken off the queue and before anything runs. `runtime.setPaused`, `runtime.step`, `runtime.stop`, `runtime.injectInput`, `runtime.watchInvariants`, `runtime.exploreScene` and `anim.playTrack` are game-only. The reads that make sense in either process are editor-or-game: `runtime.getLogs`, `runtime.getOutput`, `runtime.getTree`, `runtime.evalGdscript`, `runtime.readProfiler`, the spatial queries, `nav.queryPath`, `anim.listTracks`, `ui.listControls`, `ui.hitTest`, the three capture methods, `audio.listBuses`, `audio.configureBus` and `engine.classExists`. Everything else is editor-only. The wrong kind answers `409` with `data.code: "session_kind_rejected"`, `selected_session_kind` and `allowed_session_kinds`.

`asset.readImportedStream` loads an imported audio stream fresh from disk, bypassing the resource cache, so `asset_configure_import` can check the loop properties the engine actually loads. `resource.refreshCached` and `export.reloadPresets` make an open editor re-read a file Didi wrote behind it; without them the editor answers from the copy it already holds, or writes its own list back over the file. `resource.refreshCached` takes one `path`, or up to 256 in `paths` and then answers `results`, one entry each with `cached`, `reloaded` and, when a held copy could not be reloaded, `reload_error`. A held script is reloaded the way the Script editor reloads one that changed on disk, because a `CACHE_MODE_REPLACE` load leaves a script's code as it was; any other resource is reloaded with `CACHE_MODE_REPLACE`, which does not re-read the files it depends on (#1047). A path the editor's file index does not list, in a folder it does, that a loader takes as it stands is put in the index with `EditorFileSystem.update_file`, as the editor's own save does, and its entry gains `indexed: true`, `uid_registered` and, when the file has one, `uid`. Left for the editor's next scan, such a file whose `.uid` sidecar another Godot process had written made 4.5 and 4.6 print `Unrecognized UID` (#1150). A path open in an editor tab also has its tab rebuilt with `EditorInterface.reload_scene_from_path`, and its entry gains `scene_open`, `scene_reloaded` and, when the tab was not rebuilt, `scene_reload_error` or `scene_reload_pending`. Rebuilding drops the tab's unsaved changes, so a tab the engine reports unsaved, or before 4.7 any tab, is rebuilt only when the request carries `discard_unsaved: true`. One tab is rebuilt per request, because on 4.5 and 4.6 a second reload in the same frame does nothing, and the rest are answered pending for the server to ask again (#1068). On 4.5 and 4.6 a rebuilt tab left of a current last tab stays the edited scene, and its entry names the scene that was current in `edited_scene_moved_from`; the server opens that scene again with `scene.open` once the tabs are done, because the editor ignores an open request in the frame it changes scenes (#1072). `editor.openScenes` answers `open_scenes` with the `unsaved_scenes_readable` and `unsaved_scenes` report `editor.getState` carries, so a writer can refuse before it writes.

`editor.getProtocolServers` answers where the editor's GDScript language server and Debug Adapter Protocol server listen: `language_server` and `debug_adapter`, each `{host, port, port_source}` or null when the engine was built without that server. `port_source` is `editor_settings`, or `command_line` when the editor was started with `--lsp-port` or `--dap-port`, which the engine consumes so that only the process's own command line still holds them; the settings port is then `settings_port`. The debug adapter always binds `127.0.0.1`. The server reads it before checking a script with the language server (Q11).

`editor.getRecoveryState` is an internal main-thread readiness observation, not a public MCP tool. Its `filesystem_scanning` field reports ongoing editor filesystem scanning/import activity. Managed startup and reattachment share a 30-second deadline across discovery, exact-child attachment, four quiet readiness polls, and a stable saved-file checkpoint; connection alone is insufficient readiness.

Each `tools/list` definition carries specification `annotations` with `readOnlyHint`, `destructiveHint`, `idempotentHint`, and `openWorldHint`. They are derived and never set by hand, but from four classifications rather than one: whether the tool mutates the project, whether it changes the server's own session state, whether it can only add, and whether it lands in the same state when called twice. Deriving all four from a single read/write bit made `destructiveHint` and `idempotentHint` restatements of `readOnlyHint`, which is no information for a client to act on. Every `inputSchema` carries `additionalProperties: false`, stamped from the same predicate the argument check uses, so a client that validates locally before sending reaches the same verdict the server does. Every required string parameter carries `minLength: 1` unless its schema states another bound, because a required string on this surface names something and the empty string is never the value a caller meant; `script_create`'s `source_text` is the one exception, since an empty file is a file. Successful `tools/call` results whose payload is JSON carry `structuredContent` with that payload, emitted alongside the existing text content item rather than replacing it, so clients that read only `content` are unaffected. A client that reads `structuredContent` can decline the text item; see [Response economy](#response-economy). Where a tool publishes an `outputSchema`, it declares every field the handler can return across both execution paths plus the `execution_mode`, `is_live_engine`, `session` and `session_kind` the server stamps; only the fields present on every answer are `required`, because several are specific to one path.

Every required string parameter carries a declared length: `minLength: 1` unless an empty value is a real answer, and a `maxLength` sized by what the value is. An identifier is 256, a path is 1024, and a body (`source_text`, `new_definition`) is 1,048,576 — the one place a large number is right, and the point is that it is declared. A schema that states its own narrower bound keeps it. Both are stamped where `additionalProperties` is stamped, so an argument the schema cannot describe cannot exist. `maxLength` is a count of characters, which is what the JSON Schema keyword means, so a client that validates against the published schema before sending reaches the same verdict the server does. A handler that has its own reason to bound bytes rather than characters says so in that parameter's description: `eval_gdscript.expression` and the node-path parameters do, and their schema bound stays the character count it publishes.

A refusal names both halves of the mistake at once. A call that omits a required argument and supplies one the tool does not take reports the missing names, the unknown names, and the whole accepted set in one message, because getting a required name wrong is the likelier mistake and reporting only the absence left a caller unable to tell whether the name they did use had been ignored or accepted.

A tool publishes an `outputSchema` when something checks it against a real answer: a contract test calls every publishing tool that is reachable without an engine and fails on any returned key the schema does not declare, and the live Godot harness runs the same comparison against an attached editor for the shapes only it can produce. A tool with no schema is unspecified, and that absence is deliberate rather than an oversight. The rule is enforced in both directions: a publishing tool that the contract test does not call fails the suite, and so does a live-only tool that acquires a schema, because nothing offline could check it. Writing schemas for the live-only tools is not more of the same work; each would be a claim nothing verifies, which is the defect that made `scene_get_hierarchy`'s schema wrong for as long as it was.

These methods execute through the extension's main-thread bridge. Public project search, asset queries, script diagnostics/reflection, visual-test-lab generation, C#/shader checks, export-preset discovery/export, and MeshLibrary generation are standalone filesystem/parser/process handlers and are never routed through extension IPC. If an offline-only helper name is sent to the extension directly, it returns `409`; other reserved internal names return a structured `501` envelope:

```json
{
  "error": {
    "code": 501,
    "message": "Method is registered for compatibility but has no trustworthy live implementation: ..."
  }
}
```

See [Current Capability Matrix](CAPABILITIES.md) for the public tool mapping.

### Session descriptor and authentication envelope

The extension binds its endpoint first, then atomically publishes one schema-`1` JSON descriptor containing `session_id`, private `token`, `pid`, `kind`, canonical `project_path`, `endpoint`, process `started_at_ms`, and protocol version `1.3`. It also carries an optional `build_id`, the build the extension itself came out of, so the server can tell a bridge from its own build from one copied in out of an older build, and an optional `engine_version`, the Godot the extension was loaded into as the engine reports it, so a tool serving a pinned API dump can say whether it matches the editor in front of the caller; a descriptor without either is from an extension older than the field and is accepted, because refusing it would replace a reportable mismatch with a session nothing can reach. Discovery accepts only direct regular-file `*.json` children no larger than 64 KiB, exact endpoint shapes, the required field set plus those two optional fields and nothing else, and a live PID whose process-start identity matches. Public forms omit `token`. Orderly shutdown and proven-stale cleanup retire only an exact identity-matched object to an unpredictable no-replace path and re-verify it. Windows then deletes that exact verified object through its open handle. POSIX intentionally retains the verified `.didi-retired-<session-id>-<32hex>` tombstone because no portable object-bound unlink exists; the active `.json` name is gone and discovery ignores the tombstone. A tombstone left behind when its owner dies between the retirement move and the delete is reaped on a later discovery scan: the entry is removed only when its contents parse as a descriptor, the session id in the filename matches the session id inside it, and the owning process is provably gone. An alive or unverifiable owner, unreadable contents, or a name that disagrees with its contents all retain the tombstone. On POSIX the reaper always retains, for the same reason retirement does. A move collision/race or unavailable atomic operation retains the safer object/path rather than risk another entry.

Before connecting, the standalone client acquires the descriptor's `<session-id>.lock`. The OS lock, not metadata-file presence, enforces one MCP owner. Another client receives `423`; process exit or crash releases the kernel lock. The lock metadata never contains the session authentication token. Releasing a lock does not remove the file it was taken on, so a later discovery scan sweeps one up on both platforms: a `<32hex>.lock` with no `<32hex>.json` beside it is removed only when this process can take the lock itself, which is what proves nobody holds it. A lock another client holds answers `423` to the sweep exactly as it does to an attach, and is left alone.

Every routed live request copies public parameters and adds `_didi_session_token` internally. The extension compares all 64 token bytes in constant work, strips the field, then dispatches the command. `session.handshake` must complete within a finite deadline, 3,000 ms plus the transport's idle-recycle window, and echo matching session/protocol identity before a candidate route replaces the current route. Failed attach is transactional.

```json
{
  "method": "runtime.getLogs",
  "params": {
    "cursor": 43,
    "limit": 100,
    "minimum_level": "warning",
    "_didi_session_token": "<private 64-hex token>"
  }
}
```

The token must never be placed in MCP requests, responses, logs, diagnostics, or copied documentation examples with a real value.

### Failure provenance

An error returned from a live route carries `session` provenance identifying which route failed: `schema_version`, `session_id`, `pid`, `kind`, `project_path`, `started_at_ms` and `protocol_version`. It omits `endpoint`, which successful results and `runtime_list_sessions` do carry; a successful live result carries only `session_id` and `kind` for a client that declared `sessionDescriptor`, or on a server started with `--session-descriptor once`, once it holds the rest (see [Response economy](#response-economy)). The distinction is deliberate rather than incidental: identifying a session and publishing the address to connect to it are different acts, and only the first belongs in a failure. `token` has never appeared in any public form.

It is stated once, at the top level, where a successful result states it too. The extension also names the route on its way out, so a bridged error arrived carrying the same session in `error.data` as well; that inner copy is dropped when the envelope has one to keep. `error.data` retains everything else the engine said, including `outcome`, `route_quarantine`, `transport`, `engine` and `engine_diagnostics`. Over the internal IPC protocol, before the standalone wraps it, the extension's error still carries its own `execution_mode` and `session`: that is the layer at which it is the only attribution there is. Resource errors follow the same rule as tool errors.

### Engine diagnostics

A live answer carries the ERROR and WARNING lines the engine printed to its own console while the call ran, as `engine_diagnostics`: at most eight `{level, message, function, file, line}` records, with `engine_diagnostics_omitted` counting the rest and `engine_diagnostics_note` saying what they are. They sit beside a success's other fields and under `error.data` on a refusal, and a call during which the engine printed nothing carries none. The lines are attributed by the time they were printed, not by cause. `runtime.getOutput`, `runtime.getLogs` and `runtime.watchInvariants` read that output themselves and carry none. Every live tool's `outputSchema` declares the three fields. See [What the engine printed](TOOL_REFERENCE.md#what-the-engine-printed).

### Cursor log response

`runtime.getLogs` returns `records`, `oldest_cursor`, `next_cursor`, and `dropped_before_cursor`. Each record has `sequence`, `timestamp_ms`, `level`, `source`, `message`, and `details` (object or null). Filtering does not freeze the cursor: `next_cursor` advances across all inspected records. The 2,000-record ring caps messages at 16 KiB and details at 64 KiB.

The ring is Didi-owned structured telemetry only. It does not intercept arbitrary Godot/external-process `print()` output; the offline `runtime_launch` tool is the bounded stdout/stderr capture path.

### Runtime tree response bounds

`runtime.getTree` returns at most 10,000 nodes and at most 256 KiB of serialized public tool payload, including token-free session provenance. Node `name`, `type`, and `path` fields are valid UTF-8 capped at 1,024, 256, and 4,096 bytes respectively. Per-field `*_truncated`, per-node `children_truncated`, and top-level `truncated` flags make every clipped boundary explicit; `node_count`, `max_nodes`, and `max_response_bytes` report the observed and configured limits.

### Expression response and timeout semantics

`runtime.evalGdscript` accepts the public `eval_gdscript` fields and returns a token-free result with `context_node`, bounded `value`, `value_type`, `elapsed_ms`, `timeout_ms`, `read_only`, `sandbox_profile`, `execution_mode`, and `session_kind`. It intentionally does not echo expression source. The 1–5,000 ms deadline is checked cooperatively around parse, execution, and conversion; it cannot preempt a native call already in progress. See [Tool Reference](TOOL_REFERENCE.md#eval_gdscript--live) for the exact accepted grammar and receiver allowlist.
