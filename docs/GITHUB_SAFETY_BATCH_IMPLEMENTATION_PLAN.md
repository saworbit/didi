# GitHub Safety Batch Implementation Plan

> **Status:** Completed historical implementation plan. Branch, issue, command, and test-count details below record delivery at that time and are not current operating instructions.

> **For Codex:** Execute this plan inline with test-driven development and review checkpoints. Preserve one independently reviewable commit per issue.

**Goal:** Resolve the first prioritized GitHub safety batch by closing false-positive issue #10 with evidence and fixing reproducible issues #12 and #14 without broadening behavior.

**Architecture:** Keep the production changes at the two faulty serialization/boundary decisions. `GDScriptDiagnostics::patchSymbol` will distinguish replaceable metadata (`@` annotations and `##` documentation) from ordinary `#` comments, while `JsonRpcResponse::toJson` will preserve the JSON value supplied by the success factory. Each contract gets a native regression that fails on the unmodified implementation.

**Tech Stack:** C++17, nlohmann/json, the repository-native `didi_tests` harness, CMake/Ninja with MSVC, Python documentation validators, GitHub CLI.

---

## Task 1: Record the issue #10 invariant resolution

**Files:**
- Modify: `docs/GITHUB_SAFETY_BATCH_DESIGN.md`
- Inspect: `src/mcp/tool_registry.cpp:146-245`
- Inspect: `src/mcp/tool_registry.cpp:382-390`
- Inspect: `tests/test_runtime_routing.cpp:1068-1110`

1. Confirm each `LeaseDispatchClient::Binding` initializes `last_error` to `std::nullopt`.
2. Confirm the only assignment to `last_error` is guarded by `state->lease.has_value()`.
3. Confirm `callTool` reads the error from the same bound state before dereferencing the corresponding lease.
4. Run the existing non-atomic and descriptorless routing tests as part of the full native suite.
5. Close GitHub issue #10 as not reproducible, citing the implementation invariant and fail-closed tests. Do not add a speculative behavior-neutral code change.
6. Commit the corrected design separately as `docs: correct issue 10 audit`.

## Task 2: Preserve ordinary comments when patching symbols (#12)

**Files:**
- Modify: `tests/test_script_patch.cpp`
- Modify: `src/offline/gdscript_diagnostics.cpp:428-437`

1. Add `test_gdscript_symbol_patch_preserves_ordinary_comments` and register it as `GDScript.PatchPreservesOrdinaryComments`.
2. Construct a source containing an ordinary license/header comment, a `##` symbol documentation line, an `@warning_ignore` annotation, and the target function.
3. Patch the function and assert that the ordinary comment remains byte-for-byte, the old `##` documentation and annotation are replaced with the symbol definition, and the new function body is present.
4. Build and run `build\didi_tests.exe`; confirm the new test fails because the current backward scan absorbs the ordinary `#` comment.
5. In `GDScriptDiagnostics::patchSymbol`, remove the redundant ordinary-comment condition from the metadata scan, retaining only `@` and `##` prefixes.
6. Rebuild and run the native suite; confirm the regression and all existing tests pass.
7. Run `git diff --check` and commit as `fix(offline): preserve comments when patching symbols`.

## Task 3: Preserve explicit JSON-RPC null results (#14)

**Files:**
- Modify: `tests/test_jsonrpc.cpp`
- Modify: `src/mcp/jsonrpc.cpp:45-59`

1. Add `test_jsonrpc_null_result_serialization` and register it as `JsonRpc.NullResultSerialization`.
2. Create a successful response with a numeric ID and `nullptr`; assert both `toJson()` and parsed `serialize()` output contain a `result` member whose value is JSON null.
3. Build and run `build\didi_tests.exe`; confirm the new test fails because the current serializer converts null to `{}`.
4. Change the success branch in `JsonRpcResponse::toJson` to assign `result` directly.
5. Rebuild and run the native suite; confirm the null regression, existing object-response coverage, and all other tests pass.
6. Run `git diff --check` and commit as `fix(mcp): preserve null JSON-RPC results`.

## Task 4: Update release-facing documentation

**Files:**
- Modify: `CHANGELOG.md`

1. Replace `No unreleased changes.` with a `### Fixed` section.
2. Add concise bullets for preserving ordinary comments during GDScript symbol replacement and preserving explicit null JSON-RPC success results.
3. Do not claim #10 as a code fix or change version/tool-count/capability facts.
4. Run `python tools\validate_documentation.py` and `python -m unittest tests.test_documentation_validator -v`.
5. Run `git diff --check` and commit as `docs: record GitHub safety fixes`.

## Task 5: Red-team and verify the branch

**Files:**
- Review: all changes since `origin/main`

1. Reconfigure only if needed with `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl` from a Visual Studio developer environment.
2. Build serially with `cmake --build build -- -j1` to avoid shared MSVC PDB contention.
3. Run `build\didi_tests.exe` and require every native test to pass.
4. Run `python -m unittest tests.test_documentation_validator -v`.
5. Run `python tools\validate_documentation.py`.
6. Run `git diff --check` and confirm `git status --short` contains no uncommitted changes.
7. Request an independent code review against `origin/main`. Resolve every Critical or Important finding with another focused red-green loop, then repeat review and verification until clean.

## Task 6: Publish, merge, and verify closure

1. Push `codex/github-safety-batch` to `saworbit/didi`.
2. Open one pull request against `main` with `Fixes #12` and `Fixes #14`; mention that #10 was separately closed after invariant audit.
3. Wait for all Windows, Linux, and macOS checks. Inspect failures and annotations rather than relying only on aggregate status.
4. If CI finds a real defect, reproduce it locally where practical, add or strengthen the regression, make the smallest fix, rerun full verification, and push the new commit.
5. Merge only when every required check passes and there are zero unresolved review findings or annotations.
6. Confirm issues #12 and #14 are closed, `origin/main` contains the merge, and no branch/worktree cleanup would discard unmerged work.
7. Re-audit the remaining open backlog and proceed in the approved order: #11, then #15/#13, then #16, then #17.
