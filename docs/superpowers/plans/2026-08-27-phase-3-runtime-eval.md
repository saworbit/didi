# Phase 3 In-Engine Evaluation and Runtime Sessions Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add secure local editor/game session discovery and attachment, incremental structured runtime logs, live runtime control/tree inspection, and bounded read-only in-engine expression evaluation.

**Architecture:** A `RuntimeSessionClient` in the standalone process validates same-user descriptor files and transactionally routes the existing `IIpcClient` interface to one selected process-unique endpoint. The extension publishes its descriptor at scene initialization, authenticates every request, and continues to execute all Godot access on the main-loop bridge. Runtime observation/control and `Expression` evaluation are small bridge modules with explicit bounds and honest mode metadata.

**Tech Stack:** C++20, Godot 4.5+ GDExtension C ABI, JSON-RPC/MCP 2024-11-05, local named pipes/Unix-domain sockets, CMake/MSVC, PowerShell Godot integration harness.

## Global Constraints

- Phase 3 adds exactly 10 canonical tools: `runtime_list_sessions`, `runtime_attach_session`, `runtime_detach_session`, `runtime_get_session`, `runtime_read_logs`, `runtime_set_paused`, `runtime_step`, `runtime_stop`, `runtime_get_tree`, and `eval_gdscript`.
- The final registry contains exactly 68 canonical tools and 78 registrations including 10 legacy aliases.
- Session descriptors use schema version `1`, protocol version `1.3`, 32 lowercase-hex session IDs, and 64 lowercase-hex tokens.
- Every live request carries and verifies the selected session token; attach is transactional and cannot drop a healthy active session on a failed handshake.
- All Godot object access remains on the registered main-loop callback.
- Runtime logs retain at most 2,000 records; messages are at most 16 KiB, details serialize to at most 64 KiB, and reads return at most 500 records.
- `eval_gdscript` accepts `1..2048` UTF-8 bytes, uses `Expression.execute(..., const_calls_only=true)`, caps `timeout_ms` at `5000`, result depth at `16`, and serialized output at `256 KiB`.
- Evaluation is read-only and expression-only. Arbitrary scripts, assignments, mutation calls, filesystem/process APIs, and dynamic dispatch are rejected.
- `runtime_inject_input`, `runtime_get_call_stack`, and `runtime_read_profiler` remain unimplemented.
- Version becomes `1.3.0`; documentation must distinguish structured session logs from child-process stdout/stderr returned by `runtime_launch`.
- Commits use `Shane Wall <shane.wall@gmail.com>` and contain no Codex attribution.

---

### Task 1: Public protocol and routed session client

**Files:**
- Create: `include/didi/runtime/session_client.hpp`
- Create: `src/runtime/session_client.cpp`
- Modify: `include/didi/common/ipc_channel.hpp`
- Modify: `include/didi/mcp/mcp_protocol.hpp`
- Modify: `include/didi/mcp/tool_registry.hpp`
- Modify: `src/mcp/mcp_server.cpp`
- Modify: `src/mcp/tool_registry.cpp`
- Modify: `src/tools/runtime_tools.cpp`
- Modify: `CMakeLists.txt`
- Test: `tests/test_runtime_sessions.cpp`
- Test: `tests/test_tools.cpp`

**Interfaces:**
- Produces: `runtime::SessionDescriptor` with `toJson()` and strict `fromJson(const json&)` validation.
- Produces: `runtime::IRuntimeSessionClient : ipc::IIpcClient` with `listSessions`, `attachSession`, `detachSession`, and `activeSession`.
- Produces: `runtime::createRuntimeSessionClient(project_root)` and an injectable descriptor directory through `DIDI_SESSION_DIR`.
- Produces: the ten exact tool registrations and thin forwarding handlers.

- [ ] **Step 1: Write failing descriptor, routing, and registry tests**

Create literal descriptors and a fake IPC factory. The tests must catch a validator that accepts the wrong token length, an attach implementation that disconnects the old route before the new handshake succeeds, and a registry with the wrong names/count:

```cpp
ASSERT_TRUE(SessionDescriptor::fromJson(validDescriptor()).isOk());
auto wrong_token = validDescriptor();
wrong_token["token"] = std::string(63, 'a');
ASSERT_TRUE(SessionDescriptor::fromJson(wrong_token).isErr());

ASSERT_TRUE(client->attachSession("healthy").isOk());
ASSERT_TRUE(client->attachSession("bad-handshake").isErr());
ASSERT_EQ(client->activeSession().value().session_id, "healthy");

ASSERT_EQ(reg.listTools().size(), 78u);
for (const auto* name : {
    "runtime_list_sessions", "runtime_attach_session", "runtime_detach_session",
    "runtime_get_session", "runtime_read_logs", "runtime_set_paused",
    "runtime_step", "runtime_stop", "runtime_get_tree", "eval_gdscript"
}) {
    ASSERT_TRUE(reg.getTool(name) != nullptr);
}
```

- [ ] **Step 2: Run the focused native suite and verify RED**

Run:

```powershell
cmake --build build --config Release
.\build\Release\didi_tests.exe
```

Expected: compilation or registration assertions fail because the session interfaces and tools do not exist.

- [ ] **Step 3: Implement the minimal validated route and schemas**

Use these public shapes:

```cpp
struct SessionDescriptor {
    int schema_version{1};
    std::string session_id;
    std::string token;
    uint64_t pid{0};
    std::string kind;
    std::string project_path;
    std::string endpoint;
    int64_t started_at_ms{0};
    std::string protocol_version;
    json toJson(bool include_token = false) const;
    static Result<SessionDescriptor> fromJson(const json& value);
};

class IRuntimeSessionClient : public ipc::IIpcClient {
public:
    virtual Result<json> listSessions(const std::optional<std::string>& project_path) = 0;
    virtual Result<json> attachSession(const std::string& session_id) = 0;
    virtual Result<json> detachSession() = 0;
    virtual std::optional<SessionDescriptor> activeSession() const = 0;
};
```

The production client scans only direct `*.json` children of the resolved descriptor directory, caps a file at 64 KiB, rejects symlinks/reparse escapes, canonicalizes project paths, and performs liveness checks without deleting malformed entries. `attachSession` creates and handshakes a candidate client before swapping it under a mutex. `sendRequest` copies params, inserts `_didi_session_token`, and delegates to the selected client.

Register session-management tools as implemented `offline_fallback`; register the other six as implemented `live`. Remove none of the existing tools or aliases.

- [ ] **Step 4: Rebuild and verify GREEN**

Run the native suite and confirm the descriptor, transactional route, registration count, capability metadata, and tool-error tests pass.

- [ ] **Step 5: Commit**

```powershell
git add CMakeLists.txt include/didi src/mcp src/runtime src/tools tests/test_runtime_sessions.cpp tests/test_tools.cpp
git -c user.name="Shane Wall" -c user.email="shane.wall@gmail.com" commit -m "feat: add Phase 3 session routing surface"
```

---

### Task 2: Extension session publication and authenticated handshake

**Files:**
- Create: `include/didi/gdextension/session_host.hpp`
- Create: `src/gdextension/session_host.cpp`
- Modify: `include/didi/gdextension/gdextension_ipc.hpp`
- Modify: `src/gdextension/gdextension_ipc.cpp`
- Modify: `src/gdextension/gdextension_entry.cpp`
- Modify: `src/gdextension/editor_hook.cpp`
- Modify: `CMakeLists.txt`
- Test: `tests/test_runtime_sessions.cpp`
- Test: `tests/run_godot_integration.ps1`

**Interfaces:**
- Produces: `SessionHost::start(kind, project_path)`, `descriptor()`, `authorize(request)`, and `stop()`.
- Produces: process-unique endpoint generation on Windows and POSIX.
- Consumes: descriptor/token contracts from Task 1.

- [ ] **Step 1: Add failing host security and real discovery cases**

Native tests must assert that the endpoint contains PID plus session ID, descriptor writes are atomic, permissions are owner-only where POSIX supports them, and `authorize` strips the internal token before forwarding. Add integration requests that list and attach the running editor descriptor.

```cpp
auto denied = host.authorize({{"method", "session.handshake"}, {"params", {{"_didi_session_token", "wrong"}}}});
ASSERT_TRUE(denied.isErr());
ASSERT_EQ(denied.error().code, 401);
auto allowed = host.authorize(authenticatedRequest(host.descriptor().token));
ASSERT_TRUE(allowed.isOk());
ASSERT_TRUE(!allowed.value()["params"].contains("_didi_session_token"));
```

- [ ] **Step 2: Run focused tests and verify RED**

Expected: the editor still binds the fixed default pipe and publishes no descriptor.

- [ ] **Step 3: Implement scene-level lifecycle and per-request authentication**

Start the host exactly once at `GDEXTENSION_INITIALIZATION_SCENE`, classify the process with `Engine.is_editor_hint()`, canonicalize the current project path, and start IPC at the descriptor endpoint. Handle `session.handshake` directly with authoritative metadata. All other methods require the internal token before `EditorHook::postCommand`.

Use cryptographically strong OS randomness (`BCryptGenRandom` on Windows and `getrandom`/`/dev/urandom` on POSIX); do not use `std::rand`, timestamps, or PID as token material. Atomically publish after the server starts. If publication fails, stop IPC and expose no live service. On shutdown, cancel pending commands, stop IPC, and remove only the exact descriptor owned by the host.

- [ ] **Step 4: Rebuild and verify GREEN**

Run native tests and the editor-discovery slice of the Godot harness. Confirm the existing Phase 1/2 requests still auto-attach to the single matching editor.

- [ ] **Step 5: Commit**

```powershell
git add CMakeLists.txt include/didi/gdextension src/gdextension tests
git -c user.name="Shane Wall" -c user.email="shane.wall@gmail.com" commit -m "feat: publish authenticated Godot sessions"
```

---

### Task 3: Incremental structured runtime logs

**Files:**
- Create: `include/didi/gdextension/runtime_log.hpp`
- Create: `src/gdextension/runtime_log.cpp`
- Modify: `include/didi/common/logger.hpp`
- Modify: `src/common/logger.cpp`
- Modify: `src/gdextension/editor_hook.cpp`
- Modify: `src/gdextension/gdextension_ipc.cpp`
- Modify: `src/mcp/resource_registry.cpp`
- Modify: `src/tools/runtime_tools.cpp`
- Modify: `CMakeLists.txt`
- Test: `tests/test_runtime_logs.cpp`
- Test: `tests/test_tools.cpp`

**Interfaces:**
- Produces: `RuntimeLogRing::append(level, source, message, details)` and `read(cursor, limit, minimum_level)`.
- Produces: `Logger::setSink(LogSink)` so the extension can mirror Didi logs without recursive logging.
- Produces: `runtime.getLogs` with cursor semantics; `godot://runtime/logs` reads the same live contract.

- [ ] **Step 1: Write failing ring and resource tests**

Cover rollover, gap reporting, filtering with cursor advancement, message/detail truncation, and strict input validation:

```cpp
RuntimeLogRing ring(3);
ring.append("debug", "test", "one");
ring.append("info", "test", "two");
ring.append("warning", "test", "three");
ring.append("error", "test", "four");
auto page = ring.read(0, 2, "warning");
ASSERT_TRUE(page["dropped_before_cursor"]);
ASSERT_EQ(page["records"].size(), 2u);
ASSERT_EQ(page["next_cursor"], 5u);
```

- [ ] **Step 2: Run focused tests and verify RED**

Expected: cursor/log ring symbols are absent and the resource still returns the old unsequenced extension buffer.

- [ ] **Step 3: Implement the bounded sequence ring and live forwarding**

Assign sequence numbers starting at `1`. Interpret cursor as “next sequence to inspect”; cursor `0` begins at the oldest retained record. Always advance `next_cursor` past inspected records, including filtered ones. Validate `limit` in `1..500` and levels against the closed enum. Log command start, completion, structured error, handshake, attach-visible lifecycle, and runtime transitions. Never include session tokens or full expression text in logs.

Update the runtime resource description and return a structured live error instead of silently falling back if an attached session’s log request fails. Offline fallback remains a real standalone-status record.

- [ ] **Step 4: Rebuild and verify GREEN**

Run all native tests. Exercise two sequential log reads against the editor and prove there are no duplicate sequences.

- [ ] **Step 5: Commit**

```powershell
git add CMakeLists.txt include/didi src/common src/gdextension src/mcp src/tools tests
git -c user.name="Shane Wall" -c user.email="shane.wall@gmail.com" commit -m "feat: stream structured runtime logs"
```

---

### Task 4: Live game tree and execution control

**Files:**
- Create: `src/gdextension/runtime_bridge.cpp`
- Create: `include/didi/gdextension/runtime_bridge.hpp`
- Modify: `include/didi/gdextension/editor_hook.hpp`
- Modify: `src/gdextension/editor_hook.cpp`
- Modify: `src/gdextension/godot_bridge.cpp`
- Modify: `src/tools/runtime_tools.cpp`
- Modify: `CMakeLists.txt`
- Fixture: `tests/godot_smoke/runtime_main.tscn`
- Fixture: `tests/godot_smoke/runtime_probe.gd`
- Test: `tests/run_godot_integration.ps1`

**Interfaces:**
- Produces: `executeRuntimeBridge(method, params, session_kind)` for session/tree/pause/stop calls.
- Produces: `EditorHook::scheduleRuntimeStep(frames, promise, control)` and frame-callback completion.
- Consumes: existing bounded node traversal and Variant conversion patterns without requiring `EditorInterface` in game mode.

- [ ] **Step 1: Add failing editor-plus-game scenarios**

Start the game fixture concurrently with the editor. Assert two discoverable sessions with distinct IDs/endpoints. Attach to the game, inspect `/root/RuntimeRoot`, pause and verify, step one frame and observe `frame_counter + 1` while paused again, resume, then reattach to the editor and prove the game remains alive.

Add rejected cases for depth `17`, traversal path `..`, step while unpaused, step on editor, frames `0`/`61`, stop code `256`, and editor-only scene mutation while attached to game.

- [ ] **Step 2: Run the Godot slice and verify RED**

Expected: game sessions are absent because the extension still lacks the runtime bridge and game control methods.

- [ ] **Step 3: Implement runtime root resolution and verified control**

Resolve the active `SceneTree` from `Engine.get_main_loop()` and require `is_class("SceneTree")`. For editor sessions, `runtime_get_tree` may inspect the SceneTree but existing scene/editor mutation methods continue to use `EditorInterface`. Reuse canonical absolute NodePaths and cap traversal at 10,000 nodes.

For pause/resume, call `SceneTree.set_pause`, then `is_paused` and compare the requested state. For stepping, accept only a paused game, reject concurrent steps, set pause false, decrement on subsequent main-loop callbacks, set pause true on the final callback, verify it, and only then fulfill the held response. Shutdown cancels an outstanding step. For stop, call `quit(exit_code)` and return `shutdown_requested: true`.

- [ ] **Step 4: Rebuild and verify GREEN**

Run native tests and the concurrent Godot slice. Confirm exact frame delta, final paused state, tree bounds, editor/game isolation, and descriptor cleanup after game stop.

- [ ] **Step 5: Commit**

```powershell
git add CMakeLists.txt include/didi/gdextension src/gdextension src/tools tests
git -c user.name="Shane Wall" -c user.email="shane.wall@gmail.com" commit -m "feat: control and inspect live Godot runtimes"
```

---

### Task 5: Read-only bounded `eval_gdscript`

**Files:**
- Create: `include/didi/gdextension/expression_sandbox.hpp`
- Create: `src/gdextension/expression_sandbox.cpp`
- Modify: `src/gdextension/editor_hook.cpp`
- Modify: `src/gdextension/godot_bridge.cpp`
- Modify: `src/tools/runtime_tools.cpp`
- Modify: `CMakeLists.txt`
- Test: `tests/test_expression_sandbox.cpp`
- Test: `tests/run_godot_integration.ps1`

**Interfaces:**
- Produces: `ExpressionPolicy::validate(source)` with a token scanner that ignores quoted-string contents and escapes.
- Produces: `executeExpression(params, session_kind)` using Godot `Expression.parse` and `Expression.execute`.
- Produces: bounded JSON conversion for Vector2, Vector3, Color, and Object/Node summaries.

- [ ] **Step 1: Write failing policy and live evaluation tests**

The native table must include allowed and rejected literals independently of the production allowlist:

```cpp
for (const auto& source : {
    "node.get('process_priority')", "node.get_child_count()", "[1, 2, 3].size()",
    "'OS.execute is text'", "clamp(7, 0, 5)"
}) ASSERT_TRUE(ExpressionPolicy::validate(source).isOk());

for (const auto& source : {
    "OS.execute('cmd', [])", "FileAccess.open('res://x', 1)", "node.set('x', 1)",
    "node.call('queue_free')", "node.queue_free()", "load('res://x.gd')", "x = 1",
    "while true: pass", "node.get_script().get_source_code()"
}) ASSERT_TRUE(ExpressionPolicy::validate(source).isErr());
```

Live cases evaluate a scalar property, child count, array, dictionary, Vector2, context default, and game frame counter. Reject invalid UTF-8/NUL, 2049-byte source, timeout `0`/`5001`, unsafe identifier obfuscation attempts, missing/escaped context nodes, deep and oversized results, parse errors, and unsupported Object results.

- [ ] **Step 2: Run focused tests and verify RED**

Expected: policy and tool are absent.

- [ ] **Step 3: Implement token policy and Godot Expression execution**

Scan UTF-8 bytes into identifiers, strings, punctuation, and operators; reject unterminated strings and escape errors. Reject all statements/assignments and any identifier in the design’s high-risk set. When an identifier is followed by `(` after whitespace, require it in the exact callable allowlist. Do not regex raw source because quoted dangerous words must remain harmless and escapes must not bypass scanning.

Construct `Expression`, call `parse(expression, PackedStringArray{"node", "tree"})`, then `execute(Array{context_node, scene_tree}, nullptr, false, true)`. Check `has_execute_failed` and `get_error_text`. Measure monotonic elapsed time at validation, parse, and execution boundaries. Destroy the Expression on every exit path. Convert only the specified bounded types and include sandbox provenance fields.

- [ ] **Step 4: Rebuild and verify GREEN**

Run native and Godot tests. Perform mutation checks by temporarily permitting `set` and disabling `const_calls_only`; prove at least one rejection test fails in each case, then restore the implementation and rerun green.

- [ ] **Step 5: Commit**

```powershell
git add CMakeLists.txt include/didi/gdextension src/gdextension src/tools tests
git -c user.name="Shane Wall" -c user.email="shane.wall@gmail.com" commit -m "feat: add bounded in-engine expression evaluation"
```

---

### Task 6: End-to-end runtime integration and adversarial hardening

**Files:**
- Modify: `tests/run_godot_integration.ps1`
- Modify: `tests/godot_smoke/project.godot`
- Modify: `tests/godot_smoke/runtime_main.tscn`
- Modify: `tests/godot_smoke/runtime_probe.gd`
- Modify: implementation files only when a failing regression proves a defect

**Interfaces:**
- Consumes: all Phase 3 contracts.
- Produces: one disposable, concurrent editor/game acceptance harness with deterministic cleanup.

- [ ] **Step 1: Expand the harness into the complete Phase 3 acceptance sequence**

Use a per-run `DIDI_SESSION_DIR` under the disposable build fixture. Start the editor and game hidden, wait for two valid descriptors, then exercise list/attach/get/detach, incremental logs, both trees, pause/resume/step, safe eval and every rejection class. Preserve all 119 Phase 1/2 requests.

- [ ] **Step 2: Run the full harness and capture the first RED**

Run:

```powershell
.\tests\run_godot_integration.ps1 -GodotExecutable C:\Godot\Godot_v4.5.1-stable_win64_console.exe
```

Expected on the first expansion: at least one adversarial assertion exposes an incomplete boundary.

- [ ] **Step 3: Red-green every confirmed defect**

Pressure-test malformed descriptors, symlink/reparse escapes, PID reuse metadata, endpoint prefix tricks, token leakage, handshake timeout, failed attach rollback, simultaneous descriptor creation, descriptor deletion during attach, session death during a call, cursor overflow/gaps/filter starvation, 16 KiB messages, 64 KiB details, step shutdown, concurrent step, pause verification failure, 10,000-node truncation, escaped quotes, Unicode identifiers, comment/semicolon/newline injection, callable whitespace, dynamic dispatch, const-call bypass, deep/large values, non-finite numbers, and source-fixture cleanliness. Add one failing regression before each implementation fix.

- [ ] **Step 4: Run the complete verification slice**

Run Release build, all native tests, full Godot integration, and `git diff --check`. Confirm the descriptor directory is empty after cleanup and no Godot bridge errors appear in stdout/stderr.

- [ ] **Step 5: Commit**

```powershell
git add tests include src CMakeLists.txt
git -c user.name="Shane Wall" -c user.email="shane.wall@gmail.com" commit -m "test: harden Phase 3 runtime boundaries"
```

---

### Task 7: Documentation, release contract, independent red team, and integration gate

**Files:**
- Modify: `CMakeLists.txt`
- Modify: `src/standalone/main.cpp`
- Modify: `README.md`
- Modify: `CHANGELOG.md`
- Modify: `docs/ROADMAP.md`
- Modify: `docs/CAPABILITIES.md`
- Modify: `docs/TOOL_REFERENCE.md`
- Modify: `docs/API_SPECIFICATION.md`
- Modify: `docs/ARCHITECTURE.md`
- Modify: `docs/QUICKSTART.md`
- Modify: `docs/LLM_INSTRUCTIONS.md`
- Modify: `docs/DEVELOPER_GUIDE.md`
- Modify: `docs/ADMIN_GUIDE.md`
- Modify: `docs/INTEGRATION_GUIDE.md`
- Modify: `docs/RESOURCES_AND_PROMPTS.md`
- Modify: `.github/workflows/ci.yml`
- Modify: `.github/pull_request_template.md`
- Test: `tests/test_tools.cpp`

**Interfaces:**
- Consumes: the complete Phase 3 behavior.
- Produces: version `1.3.0`, 68-canonical/78-total discovery contract, current guides, and a reviewed merge candidate.

- [ ] **Step 1: Update the exact release and CI smoke contract**

Set CMake, executable banner, and package references to `1.3.0`. Assert exactly 78 tool registrations, live `eval_gdscript`, local session management metadata, retained unimplemented runtime debugger tools, and cursor-shaped live logs in the CI smoke.

- [ ] **Step 2: Reconcile every user/operator/developer guide**

Document exact schemas and examples, descriptor location and cleanup, auto-attach rules, editor/game mode differences, token secrecy, cursor polling, pause/step/stop semantics, expression grammar/allowlist/limits, error handling, current test counts, and Phase 3 completion. State prominently that the structured session ring does not intercept arbitrary external-process `print()` output and that `runtime_launch` remains the captured stdout/stderr path.

- [ ] **Step 3: Request independent whole-branch red-team review**

Review the full `origin/main..HEAD` diff against the design and plan. Require explicit attention to authentication, stale/PID reuse behavior, descriptor path validation, token leakage, concurrency, pending step promises, timeout truthfulness, expression scanner bypasses, result bounds, game/editor mode confusion, cleanup, cross-platform code, documentation honesty, and regression coverage. Fix every Critical and Important finding with a failing test first, then request a scoped re-review.

- [ ] **Step 4: Run the final verification matrix from a clean tree**

```powershell
cmake --build build --config Release
.\build\Release\didi_tests.exe
.\tests\run_godot_integration.ps1 -GodotExecutable C:\Godot\Godot_v4.5.1-stable_win64_console.exe
git diff --check origin/main...HEAD
git status --short
```

Run the exact MCP stdio CI smoke locally, validate all Markdown links, re-read the design requirement-by-requirement, and confirm all commits are authored by Shane Wall with no Codex attribution.

- [ ] **Step 5: Commit documentation and review fixes**

```powershell
git add .github CMakeLists.txt src/standalone README.md CHANGELOG.md docs tests
git -c user.name="Shane Wall" -c user.email="shane.wall@gmail.com" commit -m "docs: close out Phase 3 runtime sessions"
```

- [ ] **Step 6: Prepare authorized integration**

Keep the branch and worktree intact. Once the user explicitly authorizes pushing the full `codex/phase3-runtime-eval` branch, push it, open a PR against `main`, monitor every Windows/Linux/macOS/docs check, red-green any failure, merge only when all required checks pass, then monitor both post-merge `main` workflows to success.
