# Gogo Design

**Codename:** Gogo. The parallel Godot bench farm behind Didi.

**Status:** design only. No protocol, tool-count, or capability change until a later implementation phase lands with tests.

**Depends on:** Phase 3 authenticated sessions, Phase 4 visual verification, `runtime_launch`, schema-1 session descriptors.

**Does not depend on:** Agent-to-Agent (A2A), a second transport, or turning a Godot process into an autonomous planner.

**Naming:** Didi remains the product, binary (`didi` / `didi.exe`), GDExtension, and MCP server. Gogo is the subsystem. Code lives under `didi::gogo`. Do not rename the repository or the existing 78-tool surface.

In *Waiting for Godot*, Didi waits. Gogo is the one who actually shows up. That is the split: Didi is the bridge; Gogo is the farm that does the parallel work.

---

## 1. Why this exists

Didi today is a single MCP server talking to at most one attached Godot process. That is the right default for an interactive editor session.

AI agents do not work at human speed. A useful next shape is **Gogo**: the standalone `didi` process stays the MCP tool host, and behind it Gogo can own several short-lived Godot processes so an agent can say "give me twenty benches and try this in parallel."

Gogo is an orchestrator, not an agent. Planning, retry policy, and "is this level good" stay in the LLM client. Didi plus Gogo supply isolated Godot compute, honest routing, and bounded results.

This is complementary to MCP, not a replacement. MCP remains the agent-to-tool surface. A2A, if added later, would only advertise Gogo as a peer. It would not replace stdio MCP or the local IPC session protocol.

---

## 2. Vocabulary

| Term | Meaning |
| :--- | :--- |
| **Didi / MCP host** | The standalone `didi` / `didi.exe` process. One per IDE/agent connection. |
| **Gogo** | The owned-process controller inside that host: pool, leases, reap, fan-out. |
| **Session** | An existing Phase 3 descriptor: 32-hex id, PID + start time, `editor` or `game`, project path, local endpoint, private token. |
| **Active route** | The single attached session the current MCP tools already use. Unchanged. |
| **Bench** | A Godot process **owned by Gogo**: spawned, authenticated, leased, reaped. Usually `kind=game`, headless. |
| **Experiment** | One bounded job: a tool invocation or a small ordered batch, routed to one or more benches, with a deadline and an explicit outcome. |
| **Workspace** | Optional isolated project copy used by a bench so parallel mutation does not share `.godot/` or unsaved editor state. |

A user-opened editor remains a **session**. It is attachable. It is not a Gogo bench unless this host spawned it. Mixing those two ownership models is how you corrupt a human's unsaved scene.

---

## 3. What we will not build

These are hard non-goals for the first Gogo milestone. Several already appear in `docs/ROADMAP.md`.

- Do not implement A2A, Agent Cards, gRPC, or HTTP in this milestone.
- Do not add a second plugin architecture or a network transport. Benches still publish process-unique local pipes/sockets and schema-1 descriptors.
- Do not make the GDExtension plan, negotiate, or "be an agent." The extension stays a command executor on Godot's main thread.
- Do not attach twenty agents to one live editor. Godot scene, editor, and rendering APIs are not safe for concurrent mutation. Parallelism is **process** parallelism.
- Do not treat `runtime_launch` as Gogo. That tool waits for the child to **exit** (1-120 s) and returns stdout/stderr. A bench must stay alive and publish a descriptor.
- Do not silently write into `demo/` or Didi's own repository CWD. Gogo workspaces are explicit directories.
- Do not raise the 78-tool count by stuffing stubs. New names land only when they execute.
- Do not advertise "20 instances" as a compatibility guarantee. Twenty is an operator aspiration. The contract is a small default cap and a documented hard cap.
- Do not rename `didi` to `gogo`. The binary and product stay Didi.

---

## 4. How this sits on what already works

```
LLM client
   |
   |  MCP stdio / JSON-RPC 2.0
   v
didi (MCP host)
   |  existing tool registry + offline engine
   |
   +-- active route -- authenticated local IPC -- user editor or user game
   |
   +-- Gogo controller
         +-- spawn / reap Godot children
         +-- wait for schema-1 descriptors
         +-- handshake with the existing 3 s protocol-1.3 attach
         +-- route experiments to owned benches
                |
                v
         Godot child N + Didi GDExtension
         (usually --headless --path <workspace>)
```

Reuse, do not fork:

- `RuntimeSessionClient` discovery, handshake, generation, quarantine.
- Session-kind policy (`editor_only` / `game_only` / `editor_or_game`).
- 15 s extension deadline, 17 s public live deadline, `not_started` vs `unknown_outcome`.
- Capture IDs, isolation restore, exact RGBA diffs.
- Restricted local DACLs / owner-only POSIX sockets.
- PID + start-time identity so recycled PIDs are not treated as live.

Change the ownership model, not the wire protocol.

---

## 5. The gap in the current process tools

| Capability | Today | Gogo needs |
| :--- | :--- | :--- |
| Start Godot | `runtime_launch` runs a child and **blocks until exit** | Start and **keep** the child |
| Discover | `runtime_list_sessions` sees any same-user descriptor | Filter to **owned** benches vs foreign sessions |
| Attach | One active route; same-kind ambiguity stays detached | Address benches by id without stealing the user's editor route |
| Stop | `runtime_stop` on an attached **game** | Stop/reap a bench Gogo spawned, including a wedged process |
| Isolate project | All sessions share `--project` | Optional per-bench workspace copy |
| Fan-out | Client must attach, call, detach | One experiment across N benches |

The first implementation slice is therefore: **owned persistent launch + attach-without-displacing-the-active-route + explicit reap.** Pooling and fan-out come after that works twice in a row.

---

## 6. Ownership and isolation rules

1. **Foreign sessions are read-only from Gogo's point of view.** A human editor the user already opened can be listed and attached through the existing tools. Gogo must not reap it, overwrite its project, or use it as a parallel mutation worker.
2. **One editor bench per project per host, default zero.** Editors are for the human. Parallel Gogo farms use headless games.
3. **One mutating owner per workspace.** Two benches never share a writable `.godot/` or the same unsaved scene. Either they are read-only against the canonical project, or each gets a workspace copy.
4. **Workspaces live outside the source tree.** `<project>/.didi-lab/<bench-id>/` is **wrong** if that dirties version control. Prefer `<OS temp>/didi-gogo/<host-id>/<bench-id>/` or an explicit `--gogo-root`. The host records the mapping. A project-local `.didi-gogo/` directory is allowed only when gitignored and documented.
5. **Copy is bounded.** First milestone copies `project.godot`, the requested scene, and declared extra paths. It does not recursively clone `addons/` binaries or `.godot/` import caches unless the operator opts in. Broken imports fail the bench start; they do not hang the MCP host.
6. **Reap is Gogo's job.** On MCP shutdown, bench TTL expiry, explicit release, or authenticated identity failure, Gogo sends `runtime.stop` when possible, then terminates the child process group, then retires only an exact identity-matched descriptor using the existing proof-safe path.

---

## 7. Capacity contract

These numbers are the design default. They are not marketing.

| Limit | Default | Hard cap | Why |
| :--- | ---: | ---: | :--- |
| Benches per MCP host | 4 | 16 | Godot + GPU + import RAM dominate; 20 is opt-in later |
| Editor-kind benches | 0 | 1 | One human editor is already expensive |
| Pending experiments | 8 | 32 | Avoid unbounded MCP-side queues |
| Bench start wait | 20 s | 60 s | Descriptor publish + handshake must beat "Godot is importing" |
| Bench idle TTL | 120 s | 600 s | Reap abandoned farms |
| Experiment deadline | 17 s live / tool-specific | no infinite wait | Same unknown-outcome rules as Phase 3 |
| Workspace bytes | 256 MiB | 2 GiB | Fail closed rather than fill the disk |
| Concurrent live captures across benches | 4 | 8 | PNG + RGBA cache is per process, but the MCP host still serializes stdio |

An acquire request for more benches than free capacity returns a structured error with `acquired`, `requested`, and `capacity`. It must not start a partial farm and then report success.

---

## 8. Proposed MCP surface

Do not register these until the implementation phase. Keep the 78-tool count stable until then.

Suggested canonical names, Domain 13, prefix `gogo_`:

### 8.1 `gogo_status`

Local session-management style. No Godot call required.

Returns host id, capacity, each bench (`bench_id`, `session_id`, `kind`, `pid`, `state`, `workspace`, `lease_expires_at`, `last_error`), and in-flight experiment counts.

`state` is one of: `starting`, `ready`, `busy`, `quarantined`, `stopping`, `dead`.

### 8.2 `gogo_acquire`

Parameters:

- `count` (1..hard cap)
- `kind`: `game` (default) or `editor`
- `scene_path` optional `res://` path
- `headless` default true for `game`
- `isolate_workspace` default true
- `ttl_seconds` default 120
- `extra_paths` optional allowlisted `res://` copies
- `godot_extra_args` optional, no shell, same injection rules as `runtime_launch`

Behaviour:

1. Reject if `count` exceeds remaining capacity.
2. Create N workspace directories when isolation is on.
3. Spawn N Godot children with the Didi extension loaded.
4. Wait until each child publishes a schema-1 descriptor whose project path matches the workspace, kind matches, and PID+start identity is live.
5. Perform the existing 3-second authenticated handshake **without** replacing the user's active route.
6. Return bench ids plus public session metadata. Never return tokens.

Any child that misses the start deadline is killed and the whole acquire fails. No "3 of 8 are fine" success.

### 8.3 `gogo_release`

Stops named benches or `all_owned: true`. Foreign sessions are rejected. Missing ids are errors, not silent success.

### 8.4 `gogo_run`

Fan-out wrapper, still MCP tools underneath.

Parameters:

- `tool` — an already-implemented canonical name
- `arguments` — that tool's existing schema
- `bench_ids` — one or more owned ready/busy-eligible benches
- `mode`: `all` (default) or `any`

Rules:

- The named tool must allow the bench kind. `runtime_step` cannot target an editor bench.
- Editor-only scene mutations cannot target a game bench.
- `all` waits for every bench or the experiment deadline. Each bench gets its own outcome object.
- `any` returns on first success and does not cancel siblings in v1 (cancellation is easy to get wrong with `unknown_outcome`). Document that leftover work may still finish inside Godot.
- Quarantine stays per bench / per route generation. One wedged bench does not kill the farm.

### 8.5 Session selector on existing live tools (later)

An optional `session_id` / `bench_id` argument on live tools is attractive and dangerous. It changes every live handler. Defer it until `gogo_run` proves routing. Until then, the user's active route stays the only implicit target for `scene_*`, `viewport_*`, and `eval_gdscript`.

---

## 9. Bench lifecycle

```
acquire
  -> mkdir workspace (optional)
  -> spawn Godot
  -> wait descriptor (schema 1, matching project/kind/pid/start)
  -> handshake (protocol 1.3, 3 s)
  -> ready
       -> gogo_run / explicit attach
         -> busy
         -> ready | quarantined
  -> idle TTL or release or host shutdown
  -> stopping (runtime.stop if game + alive)
  -> kill process group if still alive
  -> proof-safe descriptor retirement
  -> delete workspace unless retain_workspace=true
```

`starting` is not routable. `quarantined` is not routable until the operator releases it. A quarantined bench may still complete an in-flight Godot command; callers must not retry mutations with `outcome: "unknown_outcome"`.

Gogo stores the child process handle (Windows) or process group (POSIX) so reap does not depend on the descriptor still being present.

---

## 10. Experiment outcomes

Every `gogo_run` item returns the same honesty flags the rest of Didi already uses:

- `execution_mode`
- `session` public descriptor (no token)
- `outcome`: `ok`, `not_started`, `unknown_outcome`, `rejected`, `bench_dead`
- `route_quarantine` when the live path requires it
- tool payload or structured error

```json
{
  "execution_mode": "local_gogo_management",
  "requested": 4,
  "completed_ok": 3,
  "results": [ { "bench_id": "...", "outcome": "ok", "payload": {} } ]
}
```

`gogo_run` itself is success at the MCP layer when the request was valid and every bench was addressed. Partial Godot failure lives in `results[]`. That matches Phase 3: transport and tool errors are explicit, not hidden behind a fake top-level success with empty data.

If the tool name is unimplemented or kind-incompatible, fail before spawn-side work. Do not start Godot to discover a policy you already have in `session_kind_policy.hpp`.

---

## 11. Recommended first product loop

1. Agent calls `gogo_acquire` with `count: 4`, `kind: game`, a scene, `isolate_workspace: true`.
2. Agent calls `gogo_run` with `viewport_capture_frame` or a later input/physics tool once those exist.
3. Agent compares captures with `viewport_diff_capture` **inside each bench** (capture IDs are process-local; they must not be sent to a sibling bench).
4. Agent keeps winners, releases losers, or acquires a fresh set.

Until `runtime_inject_input` and physics tools are real, the honest Gogo loops are:

- parallel headless playthroughs on long-lived benches that expose `runtime_get_tree`, `eval_gdscript`, pause/step/stop, and live capture
- parallel **offline** search / syntax / hierarchy against isolated workspaces (no Godot required)

Do not market "AI playtesting farm" while input injection is still unimplemented.

---

## 12. A2A, deferred

A2A is the wrong first protocol for this repo.

- Didi's clients are IDE agents that already speak MCP.
- A2A is agent-to-agent: discovery, stateful tasks, vendor-neutral delegation.
- Building a competent Godot-side planner inside the extension violates the "honest executor" rule that made Phases 1 through 4 trustworthy.

If A2A is added later:

- The MCP host, not the GDExtension, presents the Agent Card. The card name can be Gogo; the implementation is still Didi.
- Skills map onto `gogo_acquire` / `gogo_run` / `gogo_release`.
- Transport stays local or explicit operator HTTP. No silent port bind.
- Tokens and descriptors never appear on the A2A surface.
- MCP remains the native interface; A2A is a facade.

Until a second process that is not an MCP client needs to delegate work to Didi, skip it.

---

## 13. Security

Gogo inherits the local attachment boundary and adds process-control risk.

- Same-user only. No remote listen.
- Public JSON never includes the 64-hex token.
- `godot_extra_args` is an allowlisted argv array, never a shell string.
- Workspace paths are canonicalized and must stay under `--gogo-root` or the platform temp Gogo directory.
- Reap uses the owned process handle, then identity-matched descriptor retirement. Never delete a descriptor that does not match PID + start time.
- Do not run Gogo benches as Administrator / root because an editor happened to. Prefer the MCP host's own uid.
- Capture IDs and RGBA caches stay inside the child. The host must not merge caches across benches.
- `eval_gdscript` stays the read-only expression subset. Gogo does not relax the sandbox because the process is "just a worker."

---

## 14. Implementation sequence

### Slice A — Owned persistent launch (code)

New internal types in `didi::gogo` (`OwnedBench`, spawner). Extend `TestRunner` or add `GogoSpawner` so a child can be started **without** waiting for exit. Wait for descriptor + handshake. Add `gogo_status` against owned state only. Native tests with a fake descriptor publisher; no Godot required yet.

### Slice B — Reap and TTL

Process-group kill, proof-safe retirement, idle timer, MCP-host shutdown hook. Tests for: child dies before descriptor; descriptor appears for the wrong project; handshake fails; TTL reap; shutdown reap.

### Slice C — Isolation

Workspace copy + `--path`. Fail if copy exceeds byte cap. Integration: two benches, two workspaces, `runtime_get_tree` sees distinct roots.

### Slice D — Public Gogo tools

`gogo_status` / `gogo_acquire` / `gogo_release` / `gogo_run`. Documentation, capability metadata, changelog, tool count bump in the same change. Godot integration on 4.7.2 with at least two headless benches.

### Slice E — Optional A2A facade

Only after Slice D is boring.

Do not start Slice D while Phase 3 quarantine, reentrancy (issue 34), or `runtime_launch` identity bugs are still on fire. A farm multiplies those defects.

---

## 15. Tests that would make this real

Native, no Godot:

- Acquire rejects over-capacity and partial farms.
- Foreign session ids are refused by release and `gogo_run`.
- Handshake failure leaves zero ready benches and no orphan processes in the fake supervisor.
- Capture IDs from bench A are rejected by bench B.
- Kind policy rejects `runtime_step` on an editor bench before IPC.

Godot integration:

- Two headless benches for the smoke project publish distinct session ids.
- `gogo_run` `runtime_get_tree` returns both trees.
- Release leaves discovery empty for those ids.
- Host process exit reaps children (the easy way to leak 16 Godots).
- Isolated workspaces do not write into the canonical project.

---

## 16. Documentation impact when code lands

When Slice D ships, update in the same change: `README.md` topology, `docs/ARCHITECTURE.md`, `docs/CAPABILITIES.md`, `docs/TOOL_REFERENCE.md`, `docs/LLM_INSTRUCTIONS.md`, `docs/ROADMAP.md`, `CHANGELOG.md`, and the tool-count facts the documentation validator enforces.

Until then this file is the contract. It does not change version `1.4.0` or the 72 / 10 / 82 / 54 / 18 release facts.

---

## 17. Decision record

| Question | Decision |
| :--- | :--- |
| Product / binary name? | Didi. |
| Farm / orchestrator codename? | Gogo. |
| MCP or A2A first? | MCP. A2A later as a facade. |
| Is Godot the agent? | No. Godot is a bench. |
| Parallelism unit? | Process, not threads in one editor. |
| Default worker kind? | Headless game. |
| Share the user's editor? | List/attach only. Never Gogo-reap. |
| Partial acquire success? | No. |
| Cross-bench capture IDs? | Invalid. |
| Twenty instances? | Aspiration. Default 4, hard cap 16. |
| New tools before they run? | No. |
