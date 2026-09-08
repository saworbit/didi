# Control Room Design

**Status:** Implemented. One canonical tool name is added through a [Surface Amendment](SURFACE_AMENDMENTS.md). No new transport, no new mutation, no second MCP client.

**Supersedes nothing.** This is step 3 of the recommended order in [Human Interaction Design](HUMAN_INTERACTION_DESIGN.md#recommended-order-and-where-it-stands), which was written conditionally and has been waiting on its condition.

---

## 1. Why now, and not before

[Human Interaction Design](HUMAN_INTERACTION_DESIGN.md#observability-belongs-in-mcp-apps-not-gdscript) argued that observability belongs in [MCP Apps](https://modelcontextprotocol.io/extensions/apps/overview) rather than in GDScript, and then deliberately refused to build it:

> Still conditional on host support being broad enough to be worth it. Not started, and should not start on a schedule -- start it when a client people actually use can render it.

That condition is now met, and the evidence is external rather than a judgement call. MCP Apps shipped on 26 January 2026 as the first official MCP extension, is stable at revision `2026-01-26`, and is rendered by Claude, Claude Desktop, VS Code GitHub Copilot, Microsoft 365 Copilot, Goose, Postman, MCPJam and Archestra.AI. The extension is negotiated through the standard extensions mechanism and works over stdio, which is Didi's only transport.

So this document exists because the gate opened, not because a dashboard sounded good. If host support had not moved, the correct action would still be to wait.

## 2. What this is

**The Control Room** is a single interactive page that Didi serves to the host over the existing stdio connection. The host renders it in a sandboxed iframe inside the conversation. It answers, at a glance, the question a person actually has when an agent is misbehaving:

*Is the bridge up, what is it attached to, which tools can actually run right now, what is the safety posture, and what has Didi been saying?*

The three states named in [Human Interaction Design](HUMAN_INTERACTION_DESIGN.md#what-the-editor-still-uniquely-justifies) -- extension not loaded, no session attached, engine reachable -- are what its lights distinguish, and each carries the path, pid or session id behind it. That is the same contract the in-editor console already honours, held now on the client side of the boundary where every supported client can see it.

## 3. What this is not

**Not a replacement for the editor console.** The console's whole justification is the case where the MCP connection is broken, and a page served *over* that connection cannot serve that case. The two surfaces answer different questions and neither is redundant. [Human Interaction Design](HUMAN_INTERACTION_DESIGN.md#what-the-editor-still-uniquely-justifies) already said this; it stays true.

**Not a second MCP client.** The Control Room issues `tools/call` to the *host*, which forwards it to this server under the host's own consent policy. It speaks no IPC, holds no session, and reimplements no tool logic. This is the same rule the editor console follows -- the UI renders, the extension decides -- applied to a different renderer.

**Not a new mutation surface.** The one tool this adds is read-only. Every action the page offers is an existing tool name, called through the host, subject to the existing dry-run and confirmation gates. Adding a button does not add an authority.

**Not a configuration surface.** Configuration still has one source: the project and the launch arguments. The page reports what the server was started with. It writes nothing the server reads back.

## 4. Surface

### 4.1 One new canonical tool: `didi_control_room`

Read-only. `readOnlyHint: true`, `destructiveHint: false`, `openWorldHint: false`. No confirmation, no dry-run, because it mutates nothing.

It is named for the product rather than a Godot domain because that is what it reports on: Didi's own state, not the engine's. It is deliberately not `ui_*`, which in this surface means Godot `Control` nodes (`ui_hit_test`, `ui_list_controls`), and deliberately not a new `status_*` family, because one name does not need a family.

The tool returns the dashboard model as `structuredContent`, and carries the UI link in its definition:

```json
"_meta": {
  "ui": { "resourceUri": "ui://didi/control-room", "visibility": ["model", "app"] }
}
```

`visibility` stays `["model", "app"]` rather than `["app"]`: the model benefits from the same summary, and a host with no MCP Apps support still gets a useful text answer instead of a tool it cannot see. This is the fallback path the extension's own design expects.

The tool works with no host UI support at all. That is the point of returning a real payload rather than a render instruction.

### 4.2 One new resource: `ui://didi/control-room`

`mimeType: "text/html;profile=mcp-app"`. A single self-contained HTML document: no external scripts, styles, fonts, or images, so the host's default content security policy (`default-src 'none'; script-src 'self' 'unsafe-inline'`) is sufficient and Didi declares no `csp` domains at all. Declaring none is the strongest available position and it is available only because the page has no dependencies.

The read result carries:

```json
"_meta": { "ui": { "prefersBorder": true } }
```

The page is embedded in the binary at build time from `resources/control_room.html`, generated into C++ by `tools/generate_ui_app.py`. This follows the existing `tools/generate_phase7_schemas.py` pattern rather than inventing a second one. The alternative -- reading the file at runtime like `didi_class_reference.json` -- was rejected because a missing dashboard file should be a build failure, not a runtime surprise, and because the page is small enough that compiling it costs nothing measurable.

### 4.3 Extension negotiation

The server declares support in `server/discover` and `initialize`:

```json
"capabilities": { "extensions": { "io.modelcontextprotocol/ui": { "mimeTypes": ["text/html;profile=mcp-app"] } } }
```

That declaration is static and client-independent, so `server/discover` stays honestly cacheable as `public`.

Extensions are bilateral. Advertising the *UI surface* -- the `ui://` entry in `resources/list` and the `_meta.ui` block on the tool -- is gated on the client having declared the extension too. Modern clients declare capabilities per request in `_meta`, which is where elicitation support is already read from; `2024-11-05` clients declare them once in `initialize` params. Both are honoured.

The gate exists for a concrete reason: an unaware host that lists the resource may read it, and a page of HTML rendered into a model's context is tokens spent on markup nobody will look at.

`--ui-app <auto|always|off>` overrides the gate. `auto` is the default and is the behaviour above. `always` advertises regardless, for a host whose declaration Didi does not recognise. `off` withdraws the resource and the `_meta.ui` link entirely, leaving `didi_control_room` as a plain read-only tool. Launch arguments are already the sanctioned configuration source, so this adds no new one.

## 5. The dashboard model

Assembled from sources that already exist. Nothing here is a new measurement.

| Panel | Source | Light |
| :--- | :--- | :--- |
| **Bridge** | `acquireRuntimeRouteLease`, session descriptors | Green: connected and verified. Amber: descriptors present, none selected, or wrong kind. Red: none. |
| **Project** | Launch arguments, `project.godot` | Green: canonical root resolved and still present. Amber: resolved at startup but no longer readable. |
| **Surface** | `ToolRegistry::buildManifest()` plus per-tool `currentMode` | Per row, not per panel. |
| **Safety** | `MutationSafety`, `m_skipConfirmations`, managed-recovery state | Green: confirmations enforced. Amber: managed mode's automatic restart is armed. Red: confirmations skipped. |
| **Work** | A plain stat for a blackboard file | Informational: present, or not. |
| **Log** | A bounded in-memory ring on `Logger::setSink` | Informational. |

**Amber means unknown, and says why.** A light never goes green on an absence of evidence. Liveness Godot cannot answer for a process it did not start is reported as unverified rather than guessed, exactly as the editor console does.

**The log ring is new and is worth naming.** Didi's own diagnostics go to the process's standard error, which a client launching the server over stdio typically discards. Nothing in the tool surface can read them. A bounded ring behind the existing `Logger::setSink` seam makes them visible for the first time, capped by record count and total bytes, capturing `Info` and above by default. It is not a Godot log: `runtime_read_logs` and `runtime_read_output` remain the engine's, and the page says so rather than blurring them.

**The Work light counts nothing, on purpose.** Every board-reading entry point in `offline::blackboard` sweeps expired state and reclaims lapsed leases before answering, and then saves the board. That is a write. A tool declaring `readOnlyHint: true` cannot perform it, so the light reports only whether a board file exists, from a stat. Counts remain one `blackboard_task_list` call away, under that tool's own classification.

**Actions are existing tools.** Refresh calls `didi_control_room`. The session controls call `runtime_list_sessions`, `runtime_attach_session` and `runtime_detach_session`. Every one goes out through the host and comes back in through the ordinary path, including consent.

## 6. Adversarial requirements

These are stated as requirements rather than as a review afterthought, because each one is a test that must fail without its fix.

**A1 — No secret ever reaches the page.** The model is built from an explicit field allowlist, not by serialising a descriptor. `SessionDescriptor::toJson` already defaults `include_token` to false; the Control Room additionally never names the field. A test fails the build if the token field name appears in the generated payload or anywhere in the page source, mirroring the guard `tests/test_editor_console.py` already holds over the addon.

**A2 — No injected markup executes.** Every dynamic value -- project paths, node names, tool descriptions, log lines -- originates in files a project can contain and must be treated as hostile. The page writes them with `textContent` and DOM construction only. A test rejects `innerHTML`, `outerHTML`, `insertAdjacentHTML`, `document.write`, `eval`, and `new Function` in the page source. Without this, anyone who can write a file in the project can script inside the app's origin.

**A3 — The payload is bounded.** Tool rows, log records, and total serialised bytes are capped through the existing `json_bounds` helpers. A project with fifty thousand files cannot produce a dashboard the host must render.

**A4 — Only the host is listened to.** The page ignores any `message` event whose `source` is not `window.parent`. A sandboxed iframe has an opaque origin, so posting to the parent uses `"*"` as the target -- that is the required pattern, and it is why the inbound check has to carry the weight.

**A5 — No external origin.** The page references no external URL. A test asserts this, which is what keeps the "declare no CSP domains" position true as the page changes.

**A6 — The lights cannot lie.** Every state is derived from a checked fact, and any unchecked state renders amber with its reason. A test drives the model through disconnected, wrong-kind, unverified and skipped-confirmation states and asserts the light and the reason.

**A7 — The ring is safe under concurrency.** The logger is called from several threads. The ring is mutex-guarded, bounded, and never reenters the logger.

## 7. Testing

- **Native** (`tests/test_control_room.cpp`): the model builder against synthetic session, manifest, safety and blackboard states, including every A6 state; the bounds in A3; the allowlist in A1; the ring in A7.
- **Python** (`tests/test_control_room_app.py`): the page's static invariants -- A2, A5, the A1 name guard, presence in the CMake manifest, and that the generator turns the committed page into compilable C++ that round-trips byte for byte.
- **Protocol** (extending the existing server tests): extension declaration in both eras, the bilateral gate, all three `--ui-app` modes, the resource read shape including `_meta.ui`, and the tool's `_meta.ui` link.
- **Documentation**: `tools/validate_documentation.py` derives the surface counts from the built binary, so the count change is verified rather than asserted.

Host rendering itself is verified by hand against a real client, and the result recorded here. No test can assert that a third-party host draws a page correctly.

## 8. Surface impact

| | Before | After |
| :--- | :--- | :--- |
| Canonical tools | 113 | 114 |
| Implemented | 110 | 111 |
| Unimplemented | 3 | 3 |
| Legacy | 10 | 10 |
| `tools/list` entries | 123 | 124 |

The three API-blocked names are untouched. This adds no domain family and no mutation.

## 9. Rejected alternatives

**A Godot editor dock.** Already rejected on the record, for reasons that still hold: it serves only users with the editor open, and every line of GDScript is a line outside the tested boundary. The console that did ship is the narrow exception, justified by the broken-connection case this cannot serve.

**A local web server and a link.** A second transport, which the roadmap forbids outright, plus a port, a lifetime, and an authentication problem that stdio does not have.

**Reusing an existing tool's result instead of adding a name.** The host preloads the page from a *tool's* `_meta.ui.resourceUri`, so an entry point is required. Attaching it to an unrelated tool would make that tool's contract dishonest.

**Rendering the page from a template at runtime.** String interpolation into HTML is exactly the hazard A2 exists to prevent. The page ships static and receives its data as JSON over postMessage.
