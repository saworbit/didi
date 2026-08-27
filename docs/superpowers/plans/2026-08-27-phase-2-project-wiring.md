# Phase 2 Project Wiring Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add 18 trustworthy live tools that complete script, project, group, and scene-file wiring against a connected Godot editor.

**Architecture:** Extend the existing MCP registry and main-thread IPC bridge. Keep schemas and forwarding handlers thin, split raw Godot Variant/container helpers from focused project/scene wiring helpers, use UndoRedo for edited-scene state, and use snapshot/save/rollback for `ProjectSettings` state.

**Tech Stack:** C++20, nlohmann JSON, Godot 4.5 GDExtension C ABI, EditorUndoRedoManager, ProjectSettings, PackedScene/ResourceLoader/ResourceSaver, CMake/MSVC, native and real-editor integration tests.

## Global Constraints

- Godot 4.5 remains the minimum supported version.
- Existing 50 registrations remain backward compatible; 18 implemented canonical tools bring `tools/list` to 68.
- Every Phase 2 tool is live-only and uses `ipc::kWaitForDefinitiveResponse`.
- No direct textual editing of `project.godot` or `.tscn` files.
- No Godot object calls from the IPC worker thread.
- No success response before UndoRedo registration, `ProjectSettings.save()`, resource save, or observable editor-state verification completes.
- All `res://` and NodePath inputs reject parent traversal and subtree escape.
- No placeholder or unimplemented Phase 2 registration.

---

### Task 1: Protocol surface and honest live routing

**Files:**
- Modify: `src/mcp/tool_registry.cpp`
- Create: `include/didi/mcp/project_tools.hpp`
- Create: `src/tools/project_tools.cpp`
- Modify: `src/tools/script_tools.cpp`
- Modify: `src/tools/scene_tools.cpp`
- Modify: `src/gdextension/editor_hook.cpp`
- Modify: `CMakeLists.txt`
- Test: `tests/test_tools.cpp`

**Interfaces:**
- Produces: 18 `ToolDefinition` registrations with `ExecutionCapability{{"live"}, true, {}}`.
- Produces: forwarding handlers that map public names to the internal methods listed in the design.

- [ ] **Step 1: Add failing registry and routing tests**

Add literal assertions for all 18 names and the new total:

```cpp
ASSERT_EQ(reg.listTools().size(), 68u);
for (const auto* name : {
    "script_attach_to_node", "script_detach_from_node",
    "project_list_autoloads", "project_set_autoload", "project_remove_autoload",
    "project_list_input_actions", "project_set_input_action", "project_remove_input_action",
    "project_get_setting", "project_set_setting",
    "scene_list_groups", "scene_add_to_group", "scene_remove_from_group", "scene_get_group_members",
    "scene_create", "scene_open", "scene_close", "scene_pack_branch"
}) {
    const auto* tool = reg.getTool(name);
    ASSERT_TRUE(tool != nullptr);
    ASSERT_EQ(tool->capability.modes, std::vector<std::string>({"live"}));
    ASSERT_TRUE(tool->capability.implemented);
}
```

Extend the connected fake IPC test so `script_attach_to_node` forwards `script.attachToNode`, preserves a structured 422 error, and never falls back to a file handler.

- [ ] **Step 2: Run the focused suite and verify RED**

Run:

```powershell
cmake --build build --config Release
.\build\Release\didi_tests.exe
```

Expected: registry assertions fail because the names are absent.

- [ ] **Step 3: Register exact schemas and thin handlers**

Implement handler declarations with these signatures:

```cpp
CallToolResult handleProjectListAutoloads(const json&, std::shared_ptr<ipc::IIpcClient>);
CallToolResult handleProjectSetAutoload(const json&, std::shared_ptr<ipc::IIpcClient>);
CallToolResult handleProjectRemoveAutoload(const json&, std::shared_ptr<ipc::IIpcClient>);
CallToolResult handleProjectListInputActions(const json&, std::shared_ptr<ipc::IIpcClient>);
CallToolResult handleProjectSetInputAction(const json&, std::shared_ptr<ipc::IIpcClient>);
CallToolResult handleProjectRemoveInputAction(const json&, std::shared_ptr<ipc::IIpcClient>);
CallToolResult handleProjectGetSetting(const json&, std::shared_ptr<ipc::IIpcClient>);
CallToolResult handleProjectSetSetting(const json&, std::shared_ptr<ipc::IIpcClient>);
CallToolResult handleScriptAttachToNode(const json&, std::shared_ptr<ipc::IIpcClient>);
CallToolResult handleScriptDetachFromNode(const json&, std::shared_ptr<ipc::IIpcClient>);
CallToolResult handleSceneListGroups(const json&, std::shared_ptr<ipc::IIpcClient>);
CallToolResult handleSceneAddToGroup(const json&, std::shared_ptr<ipc::IIpcClient>);
CallToolResult handleSceneRemoveFromGroup(const json&, std::shared_ptr<ipc::IIpcClient>);
CallToolResult handleSceneGetGroupMembers(const json&, std::shared_ptr<ipc::IIpcClient>);
CallToolResult handleSceneCreate(const json&, std::shared_ptr<ipc::IIpcClient>);
CallToolResult handleSceneOpen(const json&, std::shared_ptr<ipc::IIpcClient>);
CallToolResult handleSceneClose(const json&, std::shared_ptr<ipc::IIpcClient>);
CallToolResult handleScenePackBranch(const json&, std::shared_ptr<ipc::IIpcClient>);
```

Each handler requires a connected client and calls its corresponding internal method with `kWaitForDefinitiveResponse`: `script.attachToNode`, `script.detachFromNode`, `project.listAutoloads`, `project.setAutoload`, `project.removeAutoload`, `project.listInputActions`, `project.setInputAction`, `project.removeInputAction`, `project.getSetting`, `project.setSetting`, `scene.listGroups`, `scene.addToGroup`, `scene.removeFromGroup`, `scene.getGroupMembers`, `scene.create`, `scene.open`, `scene.close`, or `scene.packBranch`. Add exactly those 18 names to `EditorHook`’s live bridge allowlist.

- [ ] **Step 4: Rebuild and verify GREEN**

Run the native suite and confirm all registration, capability, and error-propagation tests pass.

- [ ] **Step 5: Commit**

```powershell
git add CMakeLists.txt include/didi/mcp/project_tools.hpp src/mcp/tool_registry.cpp src/tools src/gdextension/editor_hook.cpp tests/test_tools.cpp
git commit -m "feat: register Phase 2 live wiring tools"
```

---

### Task 2: Shared Godot Variant and persistence primitives

**Files:**
- Create: `include/didi/gdextension/godot_bridge_internal.hpp`
- Create: `src/gdextension/godot_variant.cpp`
- Modify: `src/gdextension/godot_bridge.cpp`
- Modify: `CMakeLists.txt`
- Test: `tests/run_godot_integration.ps1`

**Interfaces:**
- Produces: `Result<VariantValue> jsonToGodotVariant(const json&, int depth = 0)` supporting null, bool, integer, finite float, string, Array, and Dictionary.
- Produces: `Result<json> godotVariantToJson(const VariantValue&, int depth = 0)` with a depth limit of 16.
- Produces: `Result<void> saveProjectSetting(GDExtensionObjectPtr settings, const std::string&, const VariantValue&)` with snapshot/save/rollback.
- Produces: `Result<std::string> validateResPath(path, requiredExtension)` and `Result<void> validateIdentifier(name, label)`.

- [ ] **Step 1: Add failing live conversion and rollback cases**

Extend the disposable Godot harness with `project_set_setting` requests for:

```json
{"setting":"didi_phase2/nested","value":{"enabled":true,"values":[1,2,"three"]}}
```

Then read it back and assert the literal nested JSON. Add rejected cases for depth 17, `autoload/Blocked`, `input/blocked`, and `../escape` resource paths.

- [ ] **Step 2: Run the Godot harness and verify RED**

Expected: requests fail at the absent tool/bridge method boundary.

- [ ] **Step 3: Extract bridge internals and implement bounded conversion**

Move only reusable ABI wrappers, `VariantValue`, `callObject`, node/root resolution, UndoRedo helpers, and provenance helpers into the private internal header. Implement recursive Array/Dictionary conversion using real Godot Variants, rejecting unsupported Object values except in typed InputEvent code.

Implement project-setting persistence as:

```cpp
old = has_setting ? get_setting(name) : nil;
set_setting(name, new_value);
error = save();
if (error != OK) {
    set_setting(name, old);
    save();
    return persistence_error;
}
```

- [ ] **Step 4: Rebuild and verify GREEN**

Run native tests plus the focused setting conversion integration cases.

- [ ] **Step 5: Commit**

```powershell
git add CMakeLists.txt include/didi/gdextension/godot_bridge_internal.hpp src/gdextension tests/run_godot_integration.ps1
git commit -m "feat: add bounded Godot Variant persistence"
```

---

### Task 3: Script attachment and persistent groups

**Files:**
- Create: `src/gdextension/scene_wiring_bridge.cpp`
- Modify: `src/gdextension/godot_bridge.cpp`
- Modify: `CMakeLists.txt`
- Test: `tests/run_godot_integration.ps1`
- Fixture: `tests/godot_smoke/subject.gd`

**Interfaces:**
- Produces: `executeSceneWiring(method, params, editor)` for `script.*` and `scene.group*` methods.
- Consumes: edited-root confinement, Variant conversion, method-bind preflight, and UndoRedo helpers.

- [ ] **Step 1: Add failing script and group scenarios**

In a disposable fixture copy, attach `res://subject.gd` to `Subject`, verify `get_script().resource_path`, undo, redo, detach, and undo. Add `Subject` to `phase2_actor`, query canonical members, undo, remove, undo, and list groups.

Add rejected cases for missing scripts, non-Script resources, duplicate membership, missing membership, empty group names, edited-root escape, and unsafe NodePaths.

- [ ] **Step 2: Run the live harness and verify RED**

Expected: the first attach request fails because the bridge method is absent.

- [ ] **Step 3: Implement minimal live wiring**

Load scripts through `ResourceLoader.load(path, "Script")`, validate `Object.is_class("Script")`, and add do/undo `script` properties. Use `Node.add_to_group(group, persistent)` and `remove_from_group(group)` through UndoRedo after `is_in_group` prechecks. Traverse only the edited subtree for member queries and return canonical paths.

- [ ] **Step 4: Rebuild and verify GREEN**

Run the full live integration harness and native suite.

- [ ] **Step 5: Commit**

```powershell
git add CMakeLists.txt src/gdextension/scene_wiring_bridge.cpp src/gdextension/godot_bridge.cpp tests/godot_smoke tests/run_godot_integration.ps1
git commit -m "feat: wire scripts and persistent scene groups"
```

---

### Task 4: Project settings and autoloads

**Files:**
- Create: `src/gdextension/project_wiring_bridge.cpp`
- Modify: `src/gdextension/godot_bridge.cpp`
- Modify: `CMakeLists.txt`
- Test: `tests/run_godot_integration.ps1`

**Interfaces:**
- Produces: `executeProjectWiring(method, params, editor)` for project settings and autoload methods.
- Consumes: `ProjectSettings` singleton, Variant conversion, identifier/path validation, and snapshot/save/rollback.

- [ ] **Step 1: Add failing persistence cases**

Set/get/remove `didi_phase2/test_value`. Set `PhaseTwoAuto` to `res://subject.gd`, list it, reject duplicate without `replace`, replace singleton state, remove it, and prove both keys are absent after removal by reading through Godot and inspecting the disposable `project.godot` after editor shutdown.

Reject invalid identifiers, missing paths, absolute paths, parent traversal, wrong extensions, missing settings, and typed namespace access through the generic setting tool.

- [ ] **Step 2: Run the live harness and verify RED**

Expected: the first project-setting request fails because the bridge method is absent.

- [ ] **Step 3: Implement ProjectSettings operations**

Use `autoload/<name>` values of `*res://path` for singleton entries and `res://path` otherwise. Enumerate `get_property_list()` and filter prefixes rather than parsing files. Every mutation uses snapshot/save/rollback and returns the persisted value.

- [ ] **Step 4: Rebuild and verify GREEN**

Run the live harness and confirm the disposable project file contains the intended values only while the source fixture remains byte-identical.

- [ ] **Step 5: Commit**

```powershell
git add CMakeLists.txt src/gdextension/project_wiring_bridge.cpp src/gdextension/godot_bridge.cpp tests/run_godot_integration.ps1
git commit -m "feat: persist project settings and autoloads"
```

---

### Task 5: Typed InputMap persistence

**Files:**
- Modify: `src/gdextension/project_wiring_bridge.cpp`
- Modify: `src/gdextension/godot_variant.cpp`
- Test: `tests/run_godot_integration.ps1`

**Interfaces:**
- Produces: `Result<VariantValue> inputEventFromJson(const json&)`.
- Produces: normalized JSON for supported `InputEventKey`, `InputEventMouseButton`, `InputEventJoypadButton`, and `InputEventJoypadMotion` objects.

- [ ] **Step 1: Add failing event matrix**

Persist `phase2_jump` with deadzone `0.25` and four literal events, list it, replace it, remove it, and inspect the disposable `project.godot`. Add one rejected request for each unknown event type, missing required index, negative index, axis value outside `-1..1`, empty key event, unknown property, deadzone outside `0..1`, and duplicate without `replace`.

- [ ] **Step 2: Run the live harness and verify RED**

Expected: the action request fails because typed InputEvent construction is absent.

- [ ] **Step 3: Implement closed event conversion**

Construct the exact Godot event class through ClassDB, set only documented properties, place object Variants into a real Array, and persist:

```json
{"deadzone":0.25,"events":["InputEvent objects"]}
```

Normalize list output to the design’s JSON schema. Destroy or release every event object on validation failure before persistence.

- [ ] **Step 4: Rebuild and verify GREEN**

Run live and native suites; verify no Godot errors or leaked invalid actions.

- [ ] **Step 5: Commit**

```powershell
git add src/gdextension/project_wiring_bridge.cpp src/gdextension/godot_variant.cpp tests/run_godot_integration.ps1
git commit -m "feat: persist typed InputMap actions"
```

---

### Task 6: Scene create, open, safe close, and branch packing

**Files:**
- Modify: `src/gdextension/scene_wiring_bridge.cpp`
- Modify: `src/gdextension/godot_bridge.cpp`
- Test: `tests/run_godot_integration.ps1`

**Interfaces:**
- Produces: scene-file methods backed by `PackedScene`, `ResourceLoader`, `ResourceSaver`, and `EditorInterface`.

- [ ] **Step 1: Add failing scene lifecycle scenarios**

Pack `Container` to `res://generated/container.tscn`; load its hierarchy offline and assert owned children. Create `res://generated/phase2_scene.tscn` with a `Node2D` root, verify it becomes active, reopen the original scene, refuse overwrite without the flag, refuse unsafe extensions/paths, protect unsaved close, explicitly close, and reopen.

- [ ] **Step 2: Run the live harness and verify RED**

Expected: the first pack request fails because the bridge method is absent.

- [ ] **Step 3: Implement resource lifecycle with cleanup**

Validate paths before object construction. For branch packing, duplicate the branch, recursively set duplicate descendants’ owner to the duplicate root, pack, save, and destroy the duplicate on every path. For create, restrict roots to `Node2D`, `Node3D`, or `Control`; pack/save before opening. Verify the active root’s `scene_file_path` after open/create. Check `get_unsaved_scenes()` before close unless `discard_unsaved` is true.

- [ ] **Step 4: Rebuild and verify GREEN**

Run native tests and the complete disposable live harness; assert generated files exist only under the disposable project.

- [ ] **Step 5: Commit**

```powershell
git add src/gdextension/scene_wiring_bridge.cpp src/gdextension/godot_bridge.cpp tests/run_godot_integration.ps1
git commit -m "feat: add safe scene file lifecycle"
```

---

### Task 7: Documentation, CI contract, red team, and release gate

**Files:**
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
- Modify: `.github/workflows/ci.yml`
- Modify: `.github/pull_request_template.md`
- Test: `tests/test_tools.cpp`
- Test: `tests/run_godot_integration.ps1`

**Interfaces:**
- Consumes: all Phase 2 contracts.
- Produces: a documented 58-canonical/68-total tool surface and verified merge-ready branch.

- [ ] **Step 1: Update the exact CI smoke contract**

Change the end-to-end assertion to `len(tools) == 68`; assert `script_attach_to_node` is implemented/live-only while `signal_connect` remains unimplemented. Keep offline resource and hierarchy checks.

- [ ] **Step 2: Reconcile all user and operator documentation**

Document schemas, examples, execution modes, persistence/UndoRedo behavior, safe-close/overwrite rules, supported InputEvents, new test counts, and Phase 2 completion. Remove stale “40 canonical” and Phase 2 NEXT claims.

- [ ] **Step 3: Run adversarial review**

Review and test malformed JSON, deep containers, invalid Unicode identifiers, path normalization, symlink escape, duplicate names, failed save rollback, editor shutdown during persistence, void-method false success, temporary object lifetime, unsaved close, partial scene writes, owner normalization, method-bind hash failure, timeout-after-dequeue, repeated undo/redo, and source-fixture cleanliness. Add a failing regression before every confirmed fix.

- [ ] **Step 4: Run the complete verification matrix**

```powershell
cmake --build build --config Release
.\build\Release\didi_tests.exe
.\tests\run_godot_integration.ps1 -GodotExecutable C:\Godot\Godot_v4.5.1-stable_win64_console.exe
git diff --check
git status --short
```

Run the exact CI MCP Python contract locally and verify all required documentation files. Request an independent review of the full base-to-head diff and iterate until no Critical or Important findings remain.

- [ ] **Step 5: Commit**

```powershell
git add .github README.md CHANGELOG.md docs tests
git commit -m "docs: close out Phase 2 project wiring"
```

- [ ] **Step 6: Integrate end-to-end**

Push `codex/phase2-project-wiring`, create a PR against `main`, monitor Windows/Linux/macOS/docs checks, fix any failure with red-green coverage, merge only when clean, and monitor both post-merge `main` workflows to success.
