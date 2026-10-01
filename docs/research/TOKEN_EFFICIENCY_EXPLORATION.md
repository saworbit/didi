# Exploratory Research & Comparative Token Efficiency Analysis for Didi

**Target System:** Didi (`saworbit/didi`) — Native C++20 Model Context Protocol (MCP) server for Godot 4.5+  
**Deliverable Path:** [`docs/research/TOKEN_EFFICIENCY_EXPLORATION.md`](TOKEN_EFFICIENCY_EXPLORATION.md)  
**Date:** September 2026  

## Checked against the code, 2026-10-01

The figures below were measured on `main` at `c814af1`, with the same `tools/list` call `tests/test_tool_profiles.py` makes, and the source was read at that commit. Where the rest of this report disagrees, this section holds. The figures for other servers in section 2.3 and the per-scenario token counts in section 3 are estimates and were not re-checked.

| Claim | What the code shows |
| :--- | :--- |
| `tools/list` is 204,600 bytes (`full`) and 95,900 (`core`) | Those are the budgets in `tests/tool_list_budgets.json`. The measured response lines are 204,582 and 95,727 bytes, uncompressed, for 130 and 60 tools. |
| Legacy names are 12% of the `full` listing | 11.7%, about 23.9 KB for the ten legacy names. |
| `script_reflect_class` reflects live ClassDB | It reads Didi's offline reference. The size is right: `Control` with no `fields` answers in 44,962 bytes. |
| Every mutation needs a `dry_run`, then a confirmed call | Only the always-confirmed tools do, and an overwrite of a target that exists. With a client that supports form elicitation, the server runs the preview itself and asks a person through the client, so the agent sends the call once. Trial 06 made 16 previews and spent 13 tokens in 222 calls. |
| R1: make `core` the default | Already the plan in #1012: `full` stays the default until a trial on `core` shows no loss. |
| R2: default `include_engine_defaults` to `false` | Valid, and P4 says the same. Tracked in #1108. No other listing tool has the argument. |
| R3: default `script_reflect_class` to fewer sections | `tests/bounded_reads.json` records the whole entry as deliberate, with `fields` to narrow it and `omitted_fields` to say what it left out. A design question, not a defect. |
| R4: a `confirm: true` flag on the first call | Not taken. The token exists so a person approves a destructive change, and an agent confirming to itself is what elicitation replaced. `--yolo` stays the operator's opt-out. |
| R5: stable key ordering | Already true. Every object in the `full` listing has its keys in order, two listings are byte-identical, and the Q3 contract snapshots fail CI when the listing moves. |
| R6: drop `"type"` beside an `enum` | Not taken. 15 properties carry an `enum`, so this saves about 240 bytes, not 12 to 15 KB. P2 has every top-level argument declare its type, because some clients send an untyped one as a string (#1000). |

---

## 1. Executive Summary

### 1.1 High-Level Verdict
The hypothesis under investigation posits:
> *"The architectural design, tool schema definitions, state representation, and response serialization in Didi make it substantially more token-efficient for LLM context windows compared to typical Python/TypeScript-based Godot MCP servers or standard JSON-RPC wrappers."*

**Verdict: Confirmed with Critical Architectural Nuances.**

Our empirical and architectural analysis confirms that Didi achieves **substantial token reductions—ranging from 53% to over 95%—across core multi-turn workflows** (scene hierarchy traversal, log streaming, property mutation, and state inspection) relative to conventional Python/Node.js-based Godot MCP implementations. 

The primary drivers of Didi's token economy are not merely low-level C++ speed, but deliberate **protocol-level, schema-level, and architectural decisions**:

1. **Profile-Bounded Tool Surface:** Through its `--tools core` profile (Q4), Didi reduces the initial `tools/list` handshake payload from **204.6 KB (~52,000 tokens)** down to **95.9 KB (~24,000 tokens)**—a **53.1% upfront token savings**—by curbing uncalled tool declarations based on empirical field trial data.
2. **Terse Logical Path Representation:** Didi replaces internal Godot editor viewport hierarchies (which span 150–350 characters per node) with clean logical paths (`/root/Main/Player`, 17 characters), reducing scene path token consumption by **~90%**.
3. **Budgeted, Shallow Hierarchy Views:** Through `summary: true`, `class_filter`, and depth/node budgets with omitted branch counts, Didi converts what is typically a 15,000–25,000 token monolithic node dump into a **50–400 token bounded response**.
4. **ANSI Stripping and Ring-Buffered Cursored Logs:** `runtime_read_logs` and `runtime_read_output` strip terminal color escapes and enforce hard UTF-8 byte caps per message and detail object, preventing catastrophic 20,000+ token runaway log dumps.
5. **Observed Post-State Elimination of Verification Turns:** By returning the observed post-mutation state in the write response (Principle [P1: Success means the change was observed](../DESIGN_PRINCIPLES.md#p1-success-means-the-change-was-observed)), Didi collapses the standard "write $\rightarrow$ read-back verification" pattern from two full turns into one, halving turn tokens and preventing hallucination spirals.
6. **Wire Deduplication (`didi/responseEconomy`):** By negotiating the omission of duplicate stringified text (`content[0].text`) and suppressing redundant session descriptors on every call (#776 / Q5), Didi cuts response payload bytes by **53.5%** on live authoring loops.

However, token efficiency is not uniform. When running in default `full` profile without client negotiation, Didi carries a heavy ~52k token declaration footprint, and certain tools historically leaked thousands of tokens of engine defaults (e.g. `project_list_input_actions` emitting ~9,000 tokens of built-in `ui_*` actions). Concrete mitigations and recommendations are detailed in [Section 5](#5-concrete-recommendations).

---

## 2. Schema & Tool Registration Comparison

### 2.1 The Baseline: System Prompt Inflation in MCP
In typical LLM clients (Claude Desktop, Cursor, Cline, Windsurf), every registered tool definition is injected into the model's system prompt or tool context on every session initiation. If tool schemas are bloated, the agent begins its task already burdened by tens of thousands of tokens of overhead.

### 2.2 Didi Tool Profiles (`full` vs `core`)
Didi implements two distinct tool profiles, governed by [Q4 in `docs/BUILD_QUEUE.md`](../BUILD_QUEUE.md#q4-shrink-the-tool-list) and budgeted in [`tests/tool_list_budgets.json`](../../tests/tool_list_budgets.json):

```json
{
  "budgets": {
    "full": 204600,
    "core": 95900
  }
}
```

| Metric | Didi Full Profile (`--tools full`) | Didi Core Profile (`--tools core`) | Absolute Reduction | Relative Savings |
| :--- | :--- | :--- | :--- | :--- |
| **Tool Count** | 130 (120 canonical + 10 legacy aliases) | 60 tools | 70 tools removed | **53.8%** |
| **`tools/list` budget (bytes of the response line)** | 204,600 bytes | 95,900 bytes | 108,700 bytes | **53.1%** |
| **Estimated Token Footprint** *(~3.8 bytes/token)* | ~53,800 tokens | ~25,200 tokens | ~28,600 tokens | **53.1%** |
| **Field Trial Coverage Justification** | Contains unreached domain tools (e.g., deep audio buses, advanced shaders, C# build tools) | Derived from the exact union of tools reached across 6 field trials + handshake guide | — | Matches 100% of empirical agent needs |

#### How the Core Profile Shrinks the Wire Without Semantic Loss
From our inspection of [`src/mcp/mcp_server.cpp`](../../src/mcp/mcp_server.cpp) and [`tests/contract_snapshots/offline-core.json`](../../tests/contract_snapshots/offline-core.json):
1. **Lifting Repetitive Per-Tool Metadata:** In the `full` profile, every tool repeats `_meta.didi.confirmationsSkipped` and `_meta.didi.editorConnected`. In the `core` profile, Didi lifts these shared server-level facts into top-level `listing._meta.didi`, stripping ~30–40 bytes of duplicate JSON boilerplate from every single tool entry.
2. **Elimination of Legacy Aliases:** Legacy tool names (e.g., `get_scene_hierarchy`, `mutate_scene_tree`) accounted for 12% of the initial byte budget. In `core`, legacy aliases are omitted entirely.
3. **Pruned Descriptions:** Prose descriptions are strictly constrained to operational contracts, omitting redundant parameter restatements.

### 2.3 Competitive Landscape: Didi vs Alternative Godot MCPs

We analyzed three primary architectural patterns in the Godot MCP ecosystem:
1. **Node.js/TypeScript Bridges** (e.g., `Coding-Solo/godot-mcp`, `tugcantopaloglu/godot-mcp`, `Godot MCP Pro`): 150–180 tools registered via TypeScript Zod or JSON-RPC schema generators.
2. **Python Bridges** (e.g., `hi-godot/godot-ai`): 80–120 tools registered via Pydantic models.
3. **Monolithic Action Dispatchers** (e.g., standard "command executor" pattern: a single tool `godot_execute` taking `{ "action": string, "params": object }`).

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                    MCP Initial Registry Token Overhead                      │
├────────────────────────────────┬───────────────────────────┬────────────────┤
│ Implementation Architecture    │ Schema Size (bytes)       │ Est. Tokens    │
├────────────────────────────────┼───────────────────────────┼────────────────┤
│ Python/Pydantic Bridge         │ ~320 KB - 440 KB          │ 80,000-110,000 │
│ TypeScript/Zod Bridge (160 t)  │ ~280 KB - 360 KB          │ 70,000-90,000  │
│ Didi Full Profile (130 tools)  │ 204.6 KB (CI capped)      │ ~53,800        │
│ Didi Core Profile (60 tools)   │ 95.9 KB (CI capped)       │ ~25,200        │
│ Monolithic Dispatcher (1 tool) │ ~2.5 KB                   │ ~600           │
└────────────────────────────────┴───────────────────────────┴────────────────┘
```

> [!NOTE]
> **Why Didi Refused the Monolithic Action Dispatcher:**  
> While a single dispatcher tool consumes only ~600 tokens initially, Didi explicitly rejected this pattern in [Design Principles: Refusals](../DESIGN_PRINCIPLES.md#refusals):  
> *"The client can no longer validate arguments per operation, and annotations and permissions collapse to the whole group, so a destructive operation shares a grant with a read."*  
> Furthermore, an untyped dispatcher causes the model to guess action names and argument schemas, resulting in frequent multi-turn error correction loops that consume significantly more tokens than the schema saved.

### 2.4 Schema Quality: Hand-Authored vs Auto-Generated
Python (Pydantic) and TypeScript (Zod) MCP servers generate schemas with extensive `$defs`, verbose `title` tags on primitive fields, nested `anyOf` wrappers for nullable fields, and redundant description tags.

Didi enforces strict hand-authored JSON Schemas:
- `additionalProperties: false` on every tool argument object (Principle [P2: Reject what cannot be interpreted](../DESIGN_PRINCIPLES.md#p2-reject-what-cannot-be-interpreted)).
- Explicit JSON types on all top-level parameters ([Q1](../BUILD_QUEUE.md#q1-type-every-argument)), eliminating ambiguity for clients like Claude Desktop that default untyped arguments to strings (Issue #1000).
- Minimalist schema primitives without nested `$defs` redirection.

---

## 3. Turn-by-Turn Payload Analysis

To evaluate real-world token consumption, we examine four representative conversational exchanges comparing Didi against typical Godot MCP patterns.

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                 Turn-by-Turn Token Cost Comparison Summary                  │
├───────────────────────────────┬───────────────────┬───────────────┬─────────┤
│ Representative Scenario       │ Alternative MCP   │ Didi (Native) │ Savings │
├───────────────────────────────┼───────────────────┼───────────────┼─────────┤
│ 1. Scene Tree Inspection      │ ~18,500 tokens    │ ~380 tokens   │ 97.9%   │
│ 2. Runtime Log Inspection     │ ~12,000 tokens    │ ~420 tokens   │ 96.5%   │
│ 3. Node Property Mutation     │ ~1,100 tokens     │ ~340 tokens   │ 69.1%   │
│ 4. Authoring Arc (7 calls)    │ ~2,950 tokens     │ ~1,350 tokens │ 54.2%   │
└───────────────────────────────┴───────────────────┴───────────────┴─────────┘
```

---

### 3.1 Scenario 1: Scene Tree Inspection & Traversal

#### Alternative Pattern (Full Node Property Serialization & Engine Paths)
Typical Python/TypeScript Godot MCP servers call Godot's built-in `Node.get_path()` and dump node properties recursively.

1. **Path Bloat:** `Node.get_path()` inside the editor traverses internal editor subviewports:
   ```text
   @EditorNode@20438/@Panel@14/@VBoxContainer@15/@HSplitContainer@23/@HSplitContainer@27/@VSplitContainer@29/@Control@56/SubViewportContainer@57/SubViewport@58/Main/Player
   ```
   *Per-node path cost:* ~140–280 characters (~40–80 tokens) per node.
2. **Property Bloat:** Dumping default node dictionaries serializes all exported engine properties (`transform`, `global_transform`, `process_mode`, `visibility_layer`, `light_mask`, `theme`, `modulate`, etc.). A single node payload reaches 1,500–2,500 bytes.
3. **Total Turn Cost for 25-node Scene:** **55 KB – 75 KB (~15,000 – 19,000 tokens)**.

#### Didi's Representation (`scene_get_hierarchy`)
Didi addresses this at the C++ and GDExtension layer ([`src/gdextension/godot_bridge.cpp`](../../src/gdextension/godot_bridge.cpp) and [`src/tools/hierarchy_view.cpp`](../../src/tools/hierarchy_view.cpp)):

1. **Logical Path Normalization:** `logicalPathFromEditedRoot` computes the terse path relative to the active edited root:
   ```text
   /root/Main/Player
   ```
   *Per-node path cost:* 17 characters (~4 tokens). **92% reduction in path tokens.**
2. **Shallow-by-Default Representation:** By default, only structural and ownership metadata are emitted (`name`, `type`, `path`, `owned_by_scene`, `instance_of`):
   ```json
   {
     "name": "Player",
     "type": "CharacterBody2D",
     "path": "/root/Main/Player",
     "owned_by_scene": true,
     "children": [
       {"name": "Sprite2D", "type": "Sprite2D", "path": "/root/Main/Player/Sprite2D", "owned_by_scene": true, "children": []},
       {"name": "CollisionShape2D", "type": "CollisionShape2D", "path": "/root/Main/Player/CollisionShape2D", "owned_by_scene": true, "children": []}
     ]
   }
   ```
3. **Summary Mode (`summary: true`):** If an agent only needs the topological shape, Didi summarizes thousands of nodes in **~60 tokens**:
   ```json
   {
     "name": "Main",
     "type": "Node2D",
     "path": "/root/Main",
     "node_count": 48,
     "counts_by_type": {"CharacterBody2D": 1, "Sprite2D": 22, "StaticBody2D": 15, "CollisionShape2D": 10},
     "branches": [{"name": "World", "type": "Node2D", "node_count": 35}, {"name": "UI", "type": "CanvasLayer", "node_count": 12}]
   }
   ```
4. **Subtree Pruning (`class_filter`) & Truncation Summary:** When searching for specific components (e.g. `["CollisionShape2D"]`), non-matching subtrees are pruned. When `max_nodes` or byte budgets are exceeded, children are not cut silently; Didi appends a compact summary:
   ```json
   "children_omitted": 14,
   "children_summary": {"Sprite2D": 10, "PointLight2D": 4},
   "truncated": true
   ```
*Total Turn Cost for 25-node Scene:* **~1,400 bytes (~380 tokens)** vs ~18,500 tokens (**97.9% savings**).

---

### 3.2 Scenario 2: Reading Runtime Logs & Output

#### Alternative Pattern (Raw stdout / Tail Interceptor)
Standard wrappers capture Godot's stdout stream directly.
- **Escape Sequences:** Godot injects ANSI color escape sequences into console output (`\u001b[31m[ERROR]\u001b[0m`), which tokenizers fragment into 3–6 tokens per occurrence.
- **Unbounded Dumps:** In a 60 FPS game, a recurring error or debug print spews 500 lines/sec. A simple log read request dumps 40–80 KB of unstructured text (**10,000 – 20,000 tokens**), immediately blowing the LLM context window.

#### Didi's Representation (`runtime_read_logs` & `runtime_read_output`)
Governed by [`src/gdextension/runtime_log.cpp`](../../src/gdextension/runtime_log.cpp) and [`tests/bounded_reads.json`](../../tests/bounded_reads.json):

1. **ANSI Stripping:** `withoutTerminalEscapes()` strips all terminal escapes *before* UTF-8 byte bounding.
2. **Fixed-Size Ring Buffer:** Capped at 2,000 records; old records are evicted in O(1) time.
3. **Length Caps per Record:** Capped at `kMaxMessageBytes` (message) and `kMaxDetailsBytes` (details). Excess strings are truncated with `message_truncated: true`.
4. **Monotonic 64-bit Sequence Cursor:** Incremental reads specify `cursor: 1420`. The server returns *only* the new records since that cursor, plus `next_cursor`, `dropped_before_cursor`, and `has_more`.
```json
{
  "records": [
    {
      "sequence": 1421,
      "timestamp_ms": 1727654321000,
      "level": "error",
      "source": "enemy.gd:42",
      "message": "Invalid call to get_player(): node not found",
      "details": null
    }
  ],
  "next_cursor": 1422,
  "oldest_cursor": 1,
  "has_more": false,
  "truncated": false
}
```
*Total Turn Cost for 5 Log Entries:* **~1,500 bytes (~420 tokens)** vs 12,000+ tokens (**96.5% savings**).

---

### 3.3 Scenario 3: Mutating Nodes & Observed Post-State

#### Alternative Pattern (Assertive Blind Execution)
In conventional wrappers, executing a property write (e.g. setting `position = Vector2(100, 200)`) returns an optimistic status:
```json
// Turn 1 (Server Response):
{"status": "success", "modified": true}
```
**The Multi-Turn Problem:**
Because Godot can silently ignore property sets (e.g., node not inside tree, read-only property, locked layout preset, or wrong coordinate space), a responsible agent must issue a second query to confirm the mutation worked:
```json
// Turn 2 (Client Request):
{"tool": "get_node_property", "target": "Player", "property": "position"}
// Turn 2 (Server Response):
{"value": {"x": 100, "y": 200}}
```
This requires **2 full round trips** (~1,100 tokens total). If the agent *does not* verify, silent failure corrupts downstream planning, triggering multi-turn hallucination spirals costing 5,000–10,000+ tokens.

#### Didi's Representation (Principle P1: Observed Post-State)
Didi enforces [P1: Success means the change was observed](../DESIGN_PRINCIPLES.md#p1-success-means-the-change-was-observed) ([`src/tools/scene_tools.cpp`](../../src/tools/scene_tools.cpp) and [`tests/observed_post_state.json`](../../tests/observed_post_state.json)):

```json
// Single Turn (Server Response):
{
  "target_node": "/root/Main/Player",
  "property_name": "position",
  "value": {"x": 100.0, "y": 200.0},
  "applied": true,
  "not_applied": [],
  "follow_up": ["editor_save_scene"]
}
```
1. **Collapses 2 Turns into 1:** The write response reports the value read back from Godot's engine memory after the write.
2. **Explicit Follow-Up:** Declares necessary next actions (`follow_up: ["editor_save_scene"]`), preventing unnecessary guessing or exploratory queries.
*Total Turn Cost:* **~340 tokens (69.1% savings and 1 fewer conversational round-trip)**.

---

### 3.4 Scenario 4: Wire Deduplication & Session Descriptors (#776)

In MCP JSON-RPC 2.0 implementations, tool call results typically package both:
1. `content`: an array containing a stringified text copy `[{"type": "text", "text": "{\"value\": 10}"}]`
2. `structuredContent`: a parsed JSON object `{"value": 10}`

Additionally, live Godot connections append session metadata. In field trial issue #776, measuring a 7-call authoring arc (instantiate node, set property x2, get property, add group, get group members, save scene) revealed that **73% of the 11,834 wire bytes were pure redundancy**:
- 42% was the duplicate stringified text copy of `structuredContent`.
- 26% was the full 8-field `SessionDescriptor` (pid, kind, endpoint, project_path, started_at_ms, protocol_version) repeated on every answer.
- 6% was identical static limitation strings.

#### Didi's Response Economy Extension (`didi/responseEconomy`)
Through [`include/didi/mcp/response_economy.hpp`](../../include/didi/mcp/response_economy.hpp) and [`src/mcp/response_economy.cpp`](../../src/mcp/response_economy.cpp):
- Clients declare:
  ```json
  "extensions": {"didi/responseEconomy": {"omit": ["textCopy", "sessionDescriptor"]}}
  ```
- Result:
  - The duplicated stringified JSON in `content[0].text` is omitted.
  - The session descriptor is sent once per route and referenced via session ID thereafter (`SessionDescriptorLedger`).
  - Alternatively, the operator starts Didi with `--session-descriptor once` for clients that cannot declare extensions.

```
#776 Seven-Call Authoring Arc Wire Comparison:
Undeclared standard MCP:   11,834 bytes (~2,950 tokens)
Declared response economy:  5,502 bytes (~1,350 tokens)  --> 53.5% Wire Byte Savings!
```

---

## 4. Architectural Advantages & Bottlenecks

### 4.1 Key Architectural Advantages

```
┌─────────────────────────────────────────────────────────────┐
│                 Didi Architectural Topology                 │
├─────────────────────────────┬───────────────────────────────┤
│ Standalone C++ MCP Daemon   │ In-Engine GDExtension Hook    │
│ (didi.exe / didi)           │ (didi_extension.dll / .so)    │
├─────────────────────────────┴───────────────────────────────┤
│ • Offline .tscn parser      │ • Main-thread queue dispatch  │
│ • Offline GDScript analyzer │ • EditorUndoRedoManager       │
│ • Stdio JSON-RPC framing    │ • Logical path normalization  │
│ • Bounded response limiter  │ • 2,000-record log ring       │
└───────────────▲─────────────┴───────────────▲───────────────┘
                │                             │
                └────── Native OS Pipe ───────┘
                     (Local IPC / Sub-ms)
```

1. **Dual Execution Topology (Offline File Parsing vs Live Hook):**  
   Many Godot tasks (inspecting `.tscn` hierarchy, checking GDScript syntax, searching project symbols, inspecting resources) do not require a live engine. Didi's standalone C++ binary executes these offline in 2–5ms without spawning Godot. This eliminates the 500ms–2s process launch latency and shell output pollution common in CLI wrappers.
2. **Native OS-Level Pipe Transport:**  
   Unlike Node.js/Python WebSocket bridges listening on open TCP ports (e.g. `127.0.0.1:9090`), Didi uses local OS-authenticated Named Pipes (Windows) or Unix-domain sockets (POSIX). This avoids network port collisions, socket buffer serialization overhead, and firewall interception.
3. **Decoupled Ambient State via MCP Resources:**  
   Instead of dumping game state on every tool call, Didi registers discrete MCP Resources:
   - `godot://editor/state`: Connection status and active scene root.
   - `godot://runtime/logs`: Bounded incremental log stream.
   - `godot://project/tree`: Offline filesystem index.
   - `blackboard://default/state`: Shared coordination state.  
   Models read these resources only when explicitly queried or subscribed to, preventing context bloat.
4. **Compiled Handshake Guidance (`kServerInstructions`):**  
   In Trial 06, returning operational guidance in the `initialize` handshake reduced failed calls from **22.7% to 3.2%** and reduced repeated failing calls from **11 to 0**. Wasted retry tokens were virtually eliminated.

---

### 4.2 Potential Bottlenecks & Token Bloat Risks

Despite its architectural strengths, our analysis identified four distinct areas where Didi can inadvertently emit excess tokens:

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                       Identified Token Bloat Risks                          │
├───────────────────────────────────┬───────────────────┬─────────────────────┤
│ Symptom / Cause                   │ Peak Token Impact │ Didi Status         │
├───────────────────────────────────┼───────────────────┼─────────────────────┤
│ 1. Unpruned Input Action Defaults │ ~9,000 tokens     │ Mitigated in Q5     │
│ 2. Unrestricted Class Reflection  │ ~8,000-12,000 t   │ Mitigated by fields │
│ 3. Two-Phase Mutation Previews    │ +1 turn (~350 t)  │ Design constraint   │
│ 4. Verbose Diagnostic Refusals    │ ~400-600 tokens   │ Intentional P5 trade│
└───────────────────────────────────┴───────────────────┴─────────────────────┘
```

1. **Engine Default Leakage in Listing Tools (Issue #775):**  
   In Field Trial 05, calling `project_list_input_actions` without `include_engine_defaults: false` dumped **85 built-in engine actions (`ui_up`, `ui_down`, etc.)**, consuming roughly **9,000 tokens** to display only 5 user-defined actions. While Didi introduced `include_engine_defaults: false`, if an agent omits this parameter, the full engine default list is returned.
2. **Whole Class Reference Entries (`script_reflect_class`):**  
   Godot classes like `Control` or `Node` have hundreds of inherited methods, properties, and signals. Calling `script_reflect_class({"class_name": "Control"})` without section filtering dumps 30–50 KB of API documentation. Didi added the `fields` argument ([Q5 part 2](../BUILD_QUEUE.md#q5-response-economy)), but calling it unparameterized still emits the full reference.
3. **Two-Phase Confirmation Round-Trip Overhead:**  
   To prevent accidental destructive operations, Didi asks for a `confirmation_token` on its always-confirmed tools and on an overwrite of a target that exists. Without form elicitation, the agent gets the token from a `dry_run` and sends it in a second call. In Trial 06 the tester previewed only the first mutation of each kind, citing the cost of doubling roughly ninety mutations. Each preview turn consumes ~300–400 tokens of conversational context. (Operators can bypass this via `--yolo`, but this drops safety guarantees).
4. **Verbose Refusal Payloads:**  
   When a call fails, Didi returns structured JSON containing `error`, `remedy`, `retry_with`, `allowed_values`, and failure context. While this enables single-turn recovery (serving Principle [P5: Guidance belongs where the agent is already looking](../DESIGN_PRINCIPLES.md#p5-guidance-belongs-where-the-agent-is-already-looking)), a single malformed call can emit 400–600 tokens of diagnostic text.

---

## 5. Concrete Recommendations

Based on our findings, we propose six prioritized recommendations to further optimize Didi's token economy without sacrificing semantic fidelity or safety guarantees.

### R1. Make `--tools core` the Default Profile for Interactive Agent Sessions
- **Impact:** **~28,600 tokens saved** immediately on session initialization.
- **Rationale:** Six field trials proved that agents only ever reach 35–38 distinct tools per game-building task, and 60 tools in `coreProfileTools()` cover 100% of all reached capabilities. Full profile should be reserved for operator opt-in (`--tools full`).

### R2. Invert Engine Defaults on Project Query Tools
- **Impact:** **~8,500 tokens saved** per `project_list_input_actions` invocation.
- **Implementation:** Change the default value of `include_engine_defaults` on `project_list_input_actions` from `true` to `false`. An agent rarely needs to inspect Godot's built-in `ui_*` keys when configuring gameplay input.

### R3. Default Section Projection on High-Volume Inspection Tools
- **Impact:** **~6,000 – 10,000 tokens saved** on class reflection.
- **Implementation:** For `script_reflect_class`, default `fields` to `["summary", "methods"]` or require an explicit `include_inherited: true` flag. Do not dump the entire transitive inheritance hierarchy of `Object` $\rightarrow$ `Node` $\rightarrow$ `CanvasItem` $\rightarrow$ `Control` unless explicitly requested.

### R4. Introduce an "Atomic Mutation-with-Confirm" Flag
- **Impact:** **~300 – 400 tokens saved per mutation** (halves mutation turns).
- **Implementation:** Allow clients to pass `confirm: true` or `confirmation_mode: "auto"` directly on the initial mutation call when running in non-interactive agent environments. This preserves UndoRedo registration and post-state verification while skipping the intermediate preview turn.

### R5. Enforce Stable Key Ordering for Prompt Cache Maximization
- **Impact:** **90%+ cache hit rate on LLM system prompt / tool registry prefix.**
- **Implementation:** Ensure that all JSON serialization in `tools/list` outputs keys in strictly deterministic alphabetical order with static timestamps. Modern frontier models (Claude 3.5/Opus, Gemini 1.5/2.0, GPT-4o) support prompt caching; preserving byte-exact schema output guarantees that the ~25k tokens of tool definitions are cached at a fraction of the cost and latency.

### R6. Compact Enum & Type Representations in Tool Manifests
- **Impact:** **~12,000 – 15,000 bytes (~3,500 tokens) saved in `tools/list`.**
- **Implementation:** Where a parameter schema specifies an `enum` of strings, omit redundant `"type": "string"` declarations where standard JSON Schema parsers infer it, and prune verbose descriptive prose that merely echoes the argument name.

---

## 6. Conclusion
Didi's native C++ architecture and empirical design principles place it well ahead of the broader Godot MCP ecosystem in token economy. By capping wire budgets in CI, normalizing scene paths to terse logical roots, stripping ANSI noise, returning observed post-state, and offering negotiated deduplication via `didi/responseEconomy`, Didi protects the LLM's context window from explosive bloat. Implementing the low-hanging default inversions and core-by-default profile will solidify Didi as the benchmark for token efficiency in engine tooling.
