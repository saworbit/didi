# Phase 4 Autonomous Verification Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Deliver bounded project text/symbol search, editor-backed asset reimport, reversible named-node viewport isolation, and deterministic live viewport image diffing.

**Architecture:** A new engine-independent `ProjectSearch` module performs request-local bounded scans under a canonical project root. Reimport and visual operations use the existing authenticated runtime lease and Godot main-thread bridge; `EditorHook` owns the multi-frame reimport state, while `ViewportRenderer` owns reversible isolation, a bounded raw capture cache, and image diff orchestration. Engine-independent image arithmetic lives in `didi/common` so edge cases are native-testable.

**Tech Stack:** C++20, Godot 4.5+ GDExtension C ABI, JSON-RPC/MCP 2024-11-05, CMake/MSVC, PowerShell Godot integration harness.

## Global Constraints

- Version is `1.4.0`.
- Phase 4 adds exactly four canonical tools: `project_search_text`, `project_search_symbols`, `asset_reimport`, and `viewport_diff_capture`.
- The final registry contains exactly 72 canonical tools and 82 registrations including 10 legacy aliases.
- Search accepts only `.gd`, `.cs`, `.tscn`, and `.tres`; query length is `1..256` bytes, results `1..500`, files at most 4 MiB, at most 10,000 files and 64 MiB inspected per request.
- Search is literal only and never follows symlinks/reparse points or leaves the canonical project root.
- Reimport accepts `1..256` unique `res://` source assets and `timeout_ms` from `1..10000`; only one request is active per editor session.
- Live capture dimensions are at most `2048x2048`; the cache retains at most 8 captures and 64 MiB of raw RGBA8 pixels.
- Live capture IDs are 32 lowercase hexadecimal characters; offline previews never receive capture IDs.
- Diff threshold is `0..255`, dimensions must match exactly, and all pixel/allocation arithmetic is checked.
- Isolation is editor-only, temporary, restoration-guarded, and never enters UndoRedo or persists scene edits.
- No Critical or Important red-team finding may remain unresolved.

---

## File Structure

- `include/didi/offline/project_search.hpp`: public search options/results and `ProjectSearch` interface.
- `src/offline/project_search.cpp`: canonical traversal, budgets, literal matching, UTF-8 previews, and GDScript/C# declaration extraction.
- `include/didi/mcp/project_tools.hpp`, `src/tools/project_tools.cpp`: offline search handlers.
- `include/didi/common/image_diff.hpp`, `src/common/image_diff.cpp`: checked RGBA diff calculation and diff-pixel generation.
- `include/didi/gdextension/viewport_renderer.hpp`, `src/gdextension/viewport_renderer.cpp`: capture IDs/cache, isolation transaction, current-frame capture, and diff response.
- `include/didi/gdextension/godot_bridge.hpp`, `src/gdextension/godot_bridge.cpp`: authoritative reimport/path operations and reversible visual-state primitives.
- `include/didi/gdextension/editor_hook.hpp`, `src/gdextension/editor_hook.cpp`: reimport scheduling, two-frame idle confirmation, timeout, concurrency gate, and shutdown cancellation.
- `src/tools/asset_tools.cpp`, `src/tools/visual_tools.cpp`: live public handlers and MCP image shaping.
- `src/mcp/tool_registry.cpp`: four Phase 4 schemas and honest capabilities.
- `tests/test_project_search.cpp`: functional and adversarial offline search tests.
- `tests/test_image_diff.cpp`: diff arithmetic, threshold, overflow, cache-budget helper, and restoration-guard tests.
- `tests/test_tools.cpp`: registration counts, schemas, validation, routing, and response content tests.
- `tests/run_godot_integration.ps1`, `tests/godot_smoke/*`: editor-backed reimport/isolation/diff lifecycle.
- `CMakeLists.txt`: production/test source registration and version.
- Release and user documentation named in the governing design.

---

### Task 1: Bounded Project Search Core

**Files:**
- Create: `include/didi/offline/project_search.hpp`
- Create: `src/offline/project_search.cpp`
- Create: `tests/test_project_search.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Produces: `Result<SearchResponse> ProjectSearch::searchText(const SearchOptions&) const`
- Produces: `Result<SearchResponse> ProjectSearch::searchSymbols(const SymbolSearchOptions&) const`
- Produces JSON-ready `SearchMatch`, `SearchDiagnostic`, and `SearchResponse::toJson()`.

- [ ] **Step 1: Write failing functional tests**

Create fixtures under a per-test temporary directory and assert exact stable results:

```cpp
TEST(ProjectSearch, TextAndGdscriptSymbols) {
    SearchFixture f;
    f.write("scripts/player.gd",
            "class_name PlayerController\n# func Fake()\nfunc jump():\n\tpass\n");
    ProjectSearch search(f.root());
    auto text = search.searchText(SearchOptions{.query = "PlayerController"});
    ASSERT_TRUE(text.isOk());
    ASSERT_EQ(text.value().matches.at(0).path, "res://scripts/player.gd");
    auto symbols = search.searchSymbols(SymbolSearchOptions{.query = "Player", .match = SymbolMatch::Prefix});
    ASSERT_TRUE(symbols.isOk());
    ASSERT_EQ(symbols.value().matches.at(0).name, "PlayerController");
    ASSERT_EQ(symbols.value().matches.at(0).kind, "class");
}
```

- [ ] **Step 2: Run the suite and verify RED**

Run: `cmake --build build --config Release && .\build\Release\didi_tests.exe`

Expected: compile failure because `didi/offline/project_search.hpp` does not exist.

- [ ] **Step 3: Add the bounded public API**

Define the exact defaults and types:

```cpp
struct SearchOptions {
    std::string query;
    std::string search_path{"res://"};
    std::vector<std::string> extensions{".gd", ".cs", ".tscn", ".tres"};
    bool case_sensitive{true};
    bool whole_word{false};
    size_t max_results{100};
};

enum class SymbolMatch { Exact, Prefix, Contains };
struct SymbolSearchOptions : SearchOptions {
    SymbolMatch match{SymbolMatch::Prefix};
    std::vector<std::string> kinds{"class", "function", "signal", "variable", "constant", "enum"};
};

class ProjectSearch {
public:
    explicit ProjectSearch(std::filesystem::path project_root);
    Result<SearchResponse> searchText(const SearchOptions& options) const;
    Result<SearchResponse> searchSymbols(const SymbolSearchOptions& options) const;
};
```

- [ ] **Step 4: Implement minimal traversal and extraction**

Canonicalize the configured root once, reject non-`res://` and lexical traversal, use `symlink_status` before recursion, allow only the four extensions, enforce all budgets before reading, split lines without copying unbounded previews, and sort matches by normalized path/line/column. Mask comments and quoted strings before declaration extraction; recognize only the declaration forms listed in the design.

- [ ] **Step 5: Run the suite and verify GREEN**

Run: `cmake --build build --config Release && .\build\Release\didi_tests.exe`

Expected: all baseline tests plus `ProjectSearch.TextAndGdscriptSymbols` pass.

- [ ] **Step 6: Add Gate 1 adversarial tests before hardening**

Add individual tests for `../`, absolute/UNC paths, a directory symlink escape, `.godot`/build exclusion, NUL/binary and malformed UTF-8 files, a 1,025-byte preview, declarations inside comments/strings, the 500-result edge, deterministic ordering, and byte/file-budget truncation. Name the production defect each test catches in a `// Break caught:` comment.

- [ ] **Step 7: Run adversarial tests and verify RED**

Expected: at least the symlink, malformed input, or declaration-masking case fails for the intended reason.

- [ ] **Step 8: Harden until Gate 1 is GREEN**

Implement handle/path containment checks, UTF-8-safe truncation, bounded diagnostics, declaration masking, and budget metadata. Run the full suite after each focused fix; stop after one audit/fix/recheck loop unless a Critical condition remains.

- [ ] **Step 9: Commit**

```powershell
git add CMakeLists.txt include/didi/offline/project_search.hpp src/offline/project_search.cpp tests/test_project_search.cpp
git commit -m "feat: add bounded project text and symbol search"
```

---

### Task 2: Search Tools and Public Contract

**Files:**
- Modify: `include/didi/mcp/project_tools.hpp`
- Modify: `src/tools/project_tools.cpp`
- Modify: `src/mcp/tool_registry.cpp`
- Modify: `tests/test_tools.cpp`

**Interfaces:**
- Consumes: `ProjectSearch::searchText`, `ProjectSearch::searchSymbols`.
- Produces: `handleProjectSearchText(args, ipc)` and `handleProjectSearchSymbols(args, ipc)`.

- [ ] **Step 1: Write failing registry and validation tests**

```cpp
ASSERT_EQ(reg.listTools().size(), 80u);
ASSERT_TRUE(reg.getTool("project_search_text") != nullptr);
ASSERT_TRUE(reg.getTool("project_search_symbols") != nullptr);
auto bad = reg.callTool("project_search_text", {{"query", ""}});
ASSERT_TRUE(bad.isError);
```

Also assert schemas expose exact length/enumeration/minimum/maximum constraints and capability metadata reports `offline_fallback` only.

- [ ] **Step 2: Run and verify RED**

Expected: registry count remains 78 and tools are missing.

- [ ] **Step 3: Implement strict JSON parsing and handlers**

Parse every field by type without `json::value()` coercion, instantiate `ProjectSearch(std::filesystem::current_path())`, return `SearchResponse::toJson()` with `execution_mode: "offline_fallback"`, and log counts only.

- [ ] **Step 4: Register the exact schemas**

Register both canonical tools with no aliases and descriptions that say literal/lexical and bounded. Keep all pre-existing tool order and aliases stable.

- [ ] **Step 5: Run and verify GREEN**

Expected: 70 canonical and 80 total registrations at this checkpoint; all tests pass.

- [ ] **Step 6: Self-review Gate 1 public boundary**

Inspect the diff for accidental IPC use, absolute-path disclosure, schema/handler disagreement, query logging, and unbounded JSON diagnostics. Add a failing regression test before each fix.

- [ ] **Step 7: Commit**

```powershell
git add include/didi/mcp/project_tools.hpp src/tools/project_tools.cpp src/mcp/tool_registry.cpp tests/test_tools.cpp
git commit -m "feat: expose project search tools"
```

---

### Task 3: Editor-Backed Reimport Lifecycle

**Files:**
- Modify: `include/didi/gdextension/godot_bridge.hpp`
- Modify: `src/gdextension/godot_bridge.cpp`
- Modify: `include/didi/gdextension/editor_hook.hpp`
- Modify: `src/gdextension/editor_hook.cpp`
- Modify: `src/gdextension/gdextension_ipc.cpp`
- Modify: `src/tools/asset_tools.cpp`
- Modify: `src/mcp/tool_registry.cpp`
- Modify: `tests/test_tools.cpp`
- Modify: `tests/test_runtime_routing.cpp`

**Interfaces:**
- Produces: public `handleAssetReimport(args, ipc)` forwarding `asset.reimport` with a definitive response deadline.
- Produces: `GodotBridge::beginAssetReimport(paths)` and `GodotBridge::isEditorFilesystemScanning()`.
- Produces: `EditorHook::scheduleAssetReimport(...)` with one `PendingAssetReimport`.

- [ ] **Step 1: Write failing handler/routing tests**

Use a fake live client to assert the exact method and arguments, reject empty/non-array/duplicate/non-`res://` inputs locally, reject timeout `0` and `10001`, and assert the new tool is live/editor-only.

- [ ] **Step 2: Run and verify RED**

Expected: `asset_reimport` is missing and no request is forwarded.

- [ ] **Step 3: Register and forward the public tool**

Add this schema shape:

```cpp
{"paths", {{"type", "array"}, {"minItems", 1}, {"maxItems", 256},
           {"uniqueItems", true}, {"items", {{"type", "string"}, {"minLength", 7}, {"maxLength", 1024}}}}},
{"timeout_ms", {{"type", "integer"}, {"default", 10000}, {"minimum", 1}, {"maximum", 10000}}}
```

Forward using the lease-bound client and existing finite definitive-response policy.

- [ ] **Step 4: Add a failing pure state-machine test**

Extract a small `ReimportProgress` value that requires two consecutive `observe(scanning=false)` calls, resets on `true`, and reports timeout from a monotonic start/deadline. Verify a one-idle-frame implementation fails.

- [ ] **Step 5: Implement authoritative begin/poll operations**

On the main thread, resolve the authoritative project root, validate the complete batch before mutation, obtain `EditorFileSystem`, invoke `reimport_files`, and let `EditorHook` poll `is_scanning` once per callback. Return canonical `res://` paths and live/editor provenance only after two idle observations.

- [ ] **Step 6: Run and verify functional GREEN**

Expected: handler, routing, state-machine, and baseline tests pass; checkpoint counts are 71 canonical/81 total.

- [ ] **Step 7: Gate 2 red-team tests**

Add tests for mixed valid/invalid atomicity, `.import` and `.godot`, duplicate normalized paths, game sessions, concurrent pending requests, shutdown cancellation, scan true/false/true races, exact timeout, and `unknown_outcome` provenance. Verify each new regression test fails before the corresponding fix.

- [ ] **Step 8: Fix and recheck Gate 2 once**

Keep the request gate held until response fulfillment/cancellation, restore it on every exit path, and ensure route quarantine follows existing Phase 3 exact-generation semantics only for unresolved transport outcomes.

- [ ] **Step 9: Commit**

```powershell
git add include/didi/gdextension/godot_bridge.hpp src/gdextension/godot_bridge.cpp include/didi/gdextension/editor_hook.hpp src/gdextension/editor_hook.cpp src/gdextension/gdextension_ipc.cpp src/tools/asset_tools.cpp src/mcp/tool_registry.cpp tests/test_tools.cpp tests/test_runtime_routing.cpp
git commit -m "feat: reimport assets through the live editor"
```

---

### Task 4: Checked Image Diff and Capture Cache

**Files:**
- Create: `include/didi/common/image_diff.hpp`
- Create: `src/common/image_diff.cpp`
- Create: `tests/test_image_diff.cpp`
- Modify: `include/didi/gdextension/viewport_renderer.hpp`
- Modify: `src/gdextension/viewport_renderer.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Produces: `Result<ImageDiffResult> diffRgba(const RgbaImage&, const RgbaImage&, uint8_t threshold)`.
- Produces: `ViewportRenderer::storeCapture(ViewportPixels)` and `ViewportRenderer::findCapture(id)` with 8-entry/64-MiB LRU bounds.

- [ ] **Step 1: Write failing arithmetic tests**

```cpp
TEST(ImageDiff, ThresholdAndBoundingBox) {
    RgbaImage before{2, 1, {0,0,0,255, 10,10,10,255}};
    RgbaImage after {2, 1, {0,0,0,255, 12,9,10,255}};
    auto diff = diffRgba(before, after, 1);
    ASSERT_TRUE(diff.isOk());
    ASSERT_EQ(diff.value().changed_pixels, 1u);
    ASSERT_EQ(diff.value().bounds->x, 1);
    ASSERT_EQ(diff.value().max_channel_delta, 2);
}
```

Add dimension mismatch, alpha-only change, threshold equality, empty/overflow dimensions, exact mean-channel sums, and transparent unchanged pixel tests.

- [ ] **Step 2: Run and verify RED**

Expected: image diff header is missing.

- [ ] **Step 3: Implement checked diff arithmetic**

Validate `width * height * 4` before indexing, use `uint64_t` sums, classify a pixel only when a delta is strictly greater than threshold, emit transparent unchanged pixels and opaque absolute-RGB changed pixels, and return no bounding box for identical images.

- [ ] **Step 4: Run and verify GREEN**

- [ ] **Step 5: Write failing cache-bound tests**

Inject deterministic IDs into a testable `CaptureCache`; assert LRU refresh, 9th-entry eviction, byte-budget eviction, oversize rejection, malformed ID rejection, and no ID for an offline handler result.

- [ ] **Step 6: Implement minimal cache and live capture IDs**

Generate 16 random bytes using the same platform entropy contract as session IDs, lowercase-hex encode them, insert only after PNG encoding succeeds, and include the ID in live metadata. Do not cache offline previews.

- [ ] **Step 7: Run all tests and commit**

```powershell
git add CMakeLists.txt include/didi/common/image_diff.hpp src/common/image_diff.cpp tests/test_image_diff.cpp include/didi/gdextension/viewport_renderer.hpp src/gdextension/viewport_renderer.cpp
git commit -m "feat: add bounded live capture cache and image diff"
```

---

### Task 5: Reversible Isolation and Viewport Diff Tool

**Files:**
- Modify: `include/didi/gdextension/godot_bridge.hpp`
- Modify: `src/gdextension/godot_bridge.cpp`
- Modify: `include/didi/gdextension/viewport_renderer.hpp`
- Modify: `src/gdextension/viewport_renderer.cpp`
- Modify: `src/gdextension/editor_hook.cpp`
- Modify: `src/tools/visual_tools.cpp`
- Modify: `src/mcp/tool_registry.cpp`
- Modify: `tests/test_image_diff.cpp`
- Modify: `tests/test_tools.cpp`

**Interfaces:**
- Produces: `ViewportRenderer::diffViewport(params)` for `vision.diffViewport`.
- Extends: `ViewportRenderer::captureViewport(params)` with isolation metadata and capture ID.
- Produces: public `handleViewportDiffCapture(args, ipc)` returning JSON plus PNG image content.

- [ ] **Step 1: Write failing public contract tests**

Assert `viewport_diff_capture` schema, 32-hex ID validation, threshold bounds, live-only behavior, two-content response, metadata image-body removal, and final counts of 72 canonical/82 total.

- [ ] **Step 2: Run and verify RED**

- [ ] **Step 3: Add a failing restoration-guard test**

Use injected visibility callbacks over fake object IDs; throw during capture and assert all changed states restore in reverse order and the result cannot claim `state_restored: true` if one identity vanished.

- [ ] **Step 4: Implement isolation transaction**

Resolve `node_isolation_path` under the active edited-scene root, enumerate visual branches, record identity/class/original visibility before changes, hide unrelated branches, optionally set reversible transparent background, force draw, capture, restore through the unconditional guard, force draw again, and attach the exact metadata from the design.

- [ ] **Step 5: Implement live diff dispatch**

Add `vision.diffViewport` to editor-only main-thread routing. Look up the baseline, capture/store a fresh comparison with the requested isolation, reject dimension mismatch, call `diffRgba`, PNG-encode the diff pixels, remove `image_base64` in the public handler, and return it as the second MCP content item.

- [ ] **Step 6: Run and verify functional GREEN**

- [ ] **Step 7: Gate 3 adversarial tests**

Cover missing/off-tree/freed nodes, already-invisible branches, capture and encoder failures, unsupported transparent backgrounds, restoration order, cache miss/eviction, dimension mismatch, 2048-square maximum, alpha-only changes, thresholds `0` and `255`, checked overflow, and 100 repeated capture/diff iterations. Watch each regression test fail before fixing it.

- [ ] **Step 8: Fix and perform one focused Gate 3 recheck**

No Critical or Important finding may remain. Minor findings may be recorded only if they do not contradict the design or public contract.

- [ ] **Step 9: Commit**

```powershell
git add include/didi/gdextension/godot_bridge.hpp src/gdextension/godot_bridge.cpp include/didi/gdextension/viewport_renderer.hpp src/gdextension/viewport_renderer.cpp src/gdextension/editor_hook.cpp src/tools/visual_tools.cpp src/mcp/tool_registry.cpp tests/test_image_diff.cpp tests/test_tools.cpp
git commit -m "feat: isolate and diff live viewport captures"
```

---

### Task 6: Godot Integration Verification

**Files:**
- Modify: `tests/godot_smoke/main.tscn`
- Create: `tests/godot_smoke/reimport_probe.svg`
- Modify: `tests/run_godot_integration.ps1`

**Interfaces:**
- Verifies the four public Phase 4 tools through the real MCP/runtime session boundary.

- [ ] **Step 1: Add failing Phase 4 integration assertions**

Add harness steps that search fixtures, copy/touch the SVG probe, call `asset_reimport`, capture baseline and isolated subject, verify visibility state after isolation, mutate an existing visible scalar property through `scene_set_property`, diff against baseline, restore the property, recapture, and assert identical pixels at threshold zero.

- [ ] **Step 2: Run and verify RED where integration wiring is incomplete**

Run: `.\tests\run_godot_integration.ps1 -GodotExecutable C:\Godot\Godot_v4.5.1-stable_win64_console.exe -BuildDirectory build`

- [ ] **Step 3: Fix only real integration gaps**

For every failure, add or tighten a native regression test first when the behavior is engine-independent. Preserve fixture bytes and clean session descriptors/sockets on failure.

- [ ] **Step 4: Run integration twice**

Expected: both consecutive runs pass, proving cache/session cleanup and non-persistent isolation.

- [ ] **Step 5: Commit**

```powershell
git add tests/godot_smoke tests/run_godot_integration.ps1
git commit -m "test: verify Phase 4 in a live Godot editor"
```

---

### Task 7: Release Documentation and Contract Validation

**Files:**
- Modify: `CMakeLists.txt`
- Modify: `README.md`
- Modify: `CHANGELOG.md`
- Modify: `docs/ROADMAP.md`
- Modify: `docs/CAPABILITIES.md`
- Modify: `docs/TOOL_REFERENCE.md`
- Modify: `docs/API_SPECIFICATION.md`
- Modify: `docs/ARCHITECTURE.md`
- Modify: `docs/QUICKSTART.md`
- Modify: `docs/LLM_INSTRUCTIONS.md`
- Modify: `docs/ADMIN_GUIDE.md`
- Modify: `docs/DEVELOPER_GUIDE.md`
- Modify: `docs/INTEGRATION_GUIDE.md`
- Modify: `docs/RESOURCES_AND_PROMPTS.md`
- Modify: `.github/workflows/ci.yml`
- Modify: `.github/pull_request_template.md`
- Modify: `tools/validate_documentation.py`
- Modify: `tests/test_documentation_validator.py`

**Interfaces:**
- Produces a consistent `1.4.0`, 72-canonical/82-total release contract.

- [ ] **Step 1: Write failing documentation-validator tests**

Assert version, counts, all four tool names, editor-only wording for reimport/isolation/diff, offline-only wording for search, 8-entry/64-MiB capture lifetime, threshold semantics, and Phase 4 roadmap completion.

- [ ] **Step 2: Run and verify RED**

Run: `python -m unittest tests.test_documentation_validator -v`

- [ ] **Step 3: Update release documentation**

Use one canonical wording for provenance and limits; include copy-paste JSON examples for each tool; state that live capture IDs die with the extension process and that search is lexical/literal.

- [ ] **Step 4: Run validator and link checks**

Run: `python tools/validate_documentation.py`

Expected: zero drift and zero broken internal links.

- [ ] **Step 5: Commit**

```powershell
git add CMakeLists.txt README.md CHANGELOG.md docs .github tools/validate_documentation.py tests/test_documentation_validator.py
git commit -m "docs: close out Phase 4 verification"
```

---

### Task 8: Final Red-Team, Verification, and Branch Handoff

**Files:**
- Modify only files implicated by reproduced Critical/Important findings.
- Create: `.superpowers/sdd/2026-08-28-phase-4-verification/final-review.md`

**Interfaces:**
- Produces final evidence that the design, implementation, tests, and docs agree.

- [ ] **Step 1: Gate 4 adversarial review**

Review `git diff origin/main...HEAD` against every design section. Check path and session boundaries, input double-validation, main-thread-only Godot access, cancellation/outcome truth, restoration under every exit, cache arithmetic/lifetime, MCP image shaping, counts, and docs. Record severity, file/line, exploit/failure path, and required fix.

- [ ] **Step 2: Reproduce every Critical/Important finding with a failing test**

Do not change production code before the regression test fails for the expected reason.

- [ ] **Step 3: Fix and run focused tests**

Limit the review loop to one fix pass plus one focused recheck. Continue only for remaining Critical findings.

- [ ] **Step 4: Run fresh full verification**

```powershell
cmake --build build --config Release --parallel
.\build\Release\didi_tests.exe
.\tests\run_godot_integration.ps1 -GodotExecutable C:\Godot\Godot_v4.5.1-stable_win64_console.exe -BuildDirectory build
python -m unittest tests.test_documentation_validator -v
python tools/validate_documentation.py
git diff --check origin/main...HEAD
git status --short
```

Expected: build exit 0; every native and integration test passes; documentation checks pass; no whitespace errors; only the ignored build outputs and the committed Phase 4 changes exist.

- [ ] **Step 5: Commit review-driven fixes and report**

```powershell
git add -u
git add .superpowers/sdd/2026-08-28-phase-4-verification/final-review.md
git commit -m "fix: resolve Phase 4 red-team findings"
```

- [ ] **Step 6: Prepare handoff**

Report branch `codex/phase4-verification`, commit list, exact verification counts, any explicitly accepted Minor finding, and integration options without merging or pushing unless separately requested.
