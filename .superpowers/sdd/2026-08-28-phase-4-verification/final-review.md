# Phase 4 Final Red-Team Review

Date: 2026-08-28

Branch: `codex/phase4-verification`

Base: `origin/main` at `c0df5a8`

## Scope reviewed

The review compared `origin/main...HEAD` and the uncommitted review fixes against every section of the Phase 4 design. It covered canonical path containment, bounded parsing, session-kind policy, main-thread engine access, reimport completion truth, reversible viewport isolation, capture-cache arithmetic and lifetime, MCP image shaping, registry/version counts, documentation, and the real Godot integration workflow.

## Findings and resolutions

| Severity | Location | Reproduction / failure path | Resolution |
|---|---|---|---|
| Critical | `include/didi/gdextension/editor_hook.hpp:155`, `src/gdextension/editor_hook.cpp:191` | `EditorFileSystem.reimport_files` can synchronously re-enter the main-loop callback. A non-recursive request mutex deadlocked the live editor integration. | Use a narrowly scoped recursive mutex for this documented Godot re-entry path. The live SVG reimport workflow now completes and requires two consecutive idle frames. |
| Important | `src/gdextension/editor_hook.cpp:215` | Polling `is_scanning` while treating the pending request as stable allowed a nested callback to complete or replace it before the outer frame resumed. | Snapshot the command identity, perform the engine query outside the local critical section, then reacquire and verify identity before mutation. |
| Important | `src/offline/project_search.cpp:228`, `src/offline/project_search.cpp:290` | Multiline GDScript triple-quoted strings and C# verbatim/raw strings containing fake declarations were reported as symbols. Both new tests failed before the fix. | Carry per-file lexical quote state across lines for both languages; regression tests assert fake declarations are excluded and later real declarations remain visible. |
| Important | `src/offline/project_search.cpp:156` | NUL-bearing or malformed UTF-8 source could be treated as searchable text and leak arbitrary bytes into previews. | Reject it as a bounded `binary_or_invalid_utf8` diagnostic without aborting unrelated files. |
| Important | `src/tools/project_tools.cpp:53` | `max_results = UINT64_MAX` could enter a signed extraction path and throw instead of returning a structured validation error. | Validate signed and unsigned JSON integer representations separately before conversion; regression coverage uses `UINT64_MAX`. |
| Important | `src/tools/visual_tools.cpp:39`, `src/tools/visual_tools.cpp:134` | A live peer could return a missing or non-string `image_base64` and the public adapter could throw or incorrectly report success. | Require an object, non-empty string image body, and valid capture ID before emitting MCP image content. The malformed-response test failed before the fix. |
| Important | `src/gdextension/viewport_renderer.cpp:29` | A random capture-ID collision could replace a live cached baseline, violating comparison identity. | Check the bounded cache and retry at most 32 times, then fail closed. |
| Important | `tests/test_runtime_routing.cpp:1306` | The direct extension kind-policy regression covered capture but not the new diff method. | Assert `vision.diffViewport` is rejected for game sessions and admitted for editor sessions before main-thread dispatch. |

Earlier gate findings also fixed during implementation were C# declarations being omitted, normalized dot-path reimport validation, symlink/generated-directory traversal, capture restoration on failure, dimension mismatch and threshold edge behavior, and release count/version drift.

## Gate outcomes

- Gate 1 — search boundary: passed. Literal/lexical scanning is bounded, stable, canonical-root confined, symlink avoiding, encoding checked, and multiline-string aware.
- Gate 2 — reimport lifecycle: passed. Requests are editor-only, atomically validated, single-flight, re-entry safe, timeout bounded, and complete only after two idle frames.
- Gate 3 — viewport isolation/diff: passed. Isolation restores in reverse order on all exits, capture storage is bounded, IDs are collision checked, dimensions and arithmetic are checked, and public responses separate metadata from PNG content.
- Gate 4 — contract and release: passed. Exactly 72 canonical tools / 82 registrations, version 1.4.0, editor/game routing policy, CI assertions, and documentation agree.

## Verification evidence

- Fresh Visual Studio 2022 Release configure/build from an empty `build/`: passed.
- Native suite on the clean artifact: 118 passed, 0 failed.
- Godot 4.5.1 live integration on the clean artifact: passed search, SVG reimport, reversible named-node isolation, changed viewport diff, restored identical diff, and descriptor cleanup.
- Documentation validator unit suite: 8 passed, 0 failed.
- Repository documentation contract: 26 Markdown files checked; version sources and internal links aligned.
- Binary version: `didi (godot-mcp-native) v1.4.0`.
- `git diff --check origin/main...HEAD` and the uncommitted review diff: passed.
- Accepted Critical or Important findings: none.
- Accepted Minor findings: none.
