# Phase 4 Autonomous Verification Design

## Objective

Close the roadmap's Phase 4 milestone with four trustworthy verification capabilities: bounded project text and symbol search, editor-backed asset reimport that waits for the editor to become idle, deterministic viewport image diffing, and temporary named-node isolation during viewport capture. The public surface must preserve Didi's provenance contract: local project inspection is labeled `offline_fallback`, engine work is labeled `live`, and no unavailable engine result is synthesized as success.

The user requested autonomous end-to-end Phase 4 delivery with sensible red-team iteration. That instruction authorizes the repository roadmap's exact Phase 4 row as the scope and authorizes design, implementation, review, and documentation decisions that remain inside that boundary.

## Repository Contract

Phase 4 is the exact roadmap row **Symbol Search, Asset Reimport, Viewport Diffing & Isolation**. It adds four canonical tools, taking the public registry from 68 to 72 canonical tools and from 78 to 82 registrations including the existing 10 legacy aliases:

- `project_search_text`
- `project_search_symbols`
- `asset_reimport`
- `viewport_diff_capture`

The existing `viewport_capture_frame` tool gains working `node_isolation_path` semantics instead of adding a fifth tool. Its `capture_viewport` alias receives the same behavior.

Phase 4 does not add UID-cache synchronization, import preset editing, export presets, headless project export, MeshLibrary generation, C# compilation diagnostics, shader diagnostics, animation keyframing, UI hit testing, persistent visual baselines, image resampling, arbitrary filesystem search, or a second indexing service. Those remain later roadmap work.

Version becomes `1.4.0`.

## Options Considered

### 1. Implement every adjacent roadmap subsection

This would combine UID synchronization, import-status inspection, import presets, export, multi-target viewport expansion, and debug modifiers with the four Phase 4 headline capabilities. It offers breadth but crosses milestone boundaries, increases live-engine risk, and makes verification failures harder to localize.

### 2. Add one generic verification workflow tool

A single command could search, mutate, capture, and compare internally. This is a smaller registry change, but it hides reusable primitives, couples unrelated failure modes, and prevents agents from composing verification around their own mutation steps.

### 3. Add four focused capabilities and complete existing isolation behavior (chosen)

Offline search remains a bounded standalone concern. Reimport and viewport capture remain authenticated editor-session operations on Godot's main thread. Image comparison uses live capture IDs retained inside the selected extension process, so it compares the exact raw pixels that produced the earlier capture without accepting unbounded image payloads or introducing a PNG decoder dependency. This is the smallest surface that closes the roadmap loop honestly.

## Project Text Search

`project_search_text` scans project-owned text resources without requiring a live Godot session.

Input:

```json
{
  "query": "PlayerController",
  "search_path": "res://",
  "extensions": [".gd", ".cs", ".tscn", ".tres"],
  "case_sensitive": true,
  "whole_word": false,
  "max_results": 100
}
```

- `query` is required UTF-8 text from 1 to 256 bytes and cannot contain NUL.
- `search_path` defaults to `res://` and must resolve to a directory inside the configured project root.
- `extensions` defaults to `.gd`, `.cs`, `.tscn`, and `.tres`; callers may select only these four extensions.
- `max_results` defaults to `100` and is restricted to `1..500`.
- Matching is literal, with optional ASCII case folding and whole-word boundaries. Phase 4 deliberately excludes regular expressions to avoid catastrophic-backtracking and portability risks.
- Results contain project-relative `res://` path, one-based line and column, and a UTF-8-safe line preview capped at 1,024 bytes. Ordering is stable by normalized path, line, then column.
- The scanner skips symlinks/reparse points, `.git`, `.godot`, `.worktrees`, build outputs, files over 4 MiB, and files outside the allowlisted extensions. It inspects at most 10,000 files and 64 MiB per request.
- The response includes scanned/skipped counts, `truncated`, `execution_mode: "offline_fallback"`, and the canonical project root. Unreadable or invalidly encoded files are reported in bounded diagnostics instead of aborting unrelated results.

All path containment checks use canonical handles/paths where the platform permits them. Lexical `..`, absolute paths, drive/UNC changes, symlink escapes, and `res://` aliases that resolve outside the project are rejected.

## Project Symbol Search

`project_search_symbols` builds a request-local index over the same bounded project files and returns declarations rather than text occurrences.

Input:

```json
{
  "query": "Player",
  "search_path": "res://",
  "match": "prefix",
  "kinds": ["class", "function", "signal", "variable", "constant", "enum"],
  "max_results": 100
}
```

- `match` is `exact`, `prefix`, or `contains` and defaults to `prefix`.
- `kinds` may select from `class`, `function`, `signal`, `variable`, `constant`, and `enum`.
- GDScript declarations include `class_name`, `func`, `signal`, `var`, `const`, and `enum`. C# declarations include namespace-level or type-member classes, structs, interfaces, enums, methods, fields, properties, and events mapped to the public kinds.
- Every result includes name, kind, language, `res://` path, one-based line/column, and a bounded declaration preview. Where it can be determined without semantic execution, it also includes the containing type.
- Comments and quoted strings are excluded before declaration recognition. The scanner is deliberately lexical, not a replacement language server, and says so in response metadata.
- Search bounds, path rules, stable ordering, diagnostics, and provenance are identical to text search.

The implementation shares one `ProjectSearch` component for traversal, validation, bounds, UTF-8 handling, and result ordering. Language-specific declaration extractors are small, testable functions with no engine dependency.

## Asset Reimport

`asset_reimport` is live-only and editor-only. It accepts `paths` containing 1 to 256 unique `res://` source assets and optional `timeout_ms` from 1 to 10,000, defaulting to 10,000.

The standalone handler validates JSON shape and obtains an editor-kind runtime lease. The extension repeats all security validation against its authoritative canonical project root, rejects paths under `.godot`, rejects `.import` sidecars and directories, and requires every source to exist as a regular project-owned file. It then invokes `EditorFileSystem.reimport_files` on Godot's main thread.

Reimport completion is a main-loop state machine, not a blocking sleep. After triggering reimport it polls `EditorFileSystem.is_scanning()` once per main-loop callback and completes only after two consecutive idle callbacks. The response contains each normalized path, accepted count, elapsed time, `idle: true`, `execution_mode: "live"`, `is_live_engine: true`, and `session_kind: "editor"`.

Only one reimport request may be active per editor session. A concurrent request returns `409`. Validation failure occurs before any mutation. A bounded internal timeout returns `504` with `outcome: "unknown_outcome"` because Godot may still finish work after the caller stops waiting; it does not claim rollback. Session shutdown cancels the pending response. Game sessions and detached callers return honest availability errors.

## Live Capture IDs

Every successful live `viewport_capture_frame` response gains a `capture_id` identifying the exact raw RGBA8 pixels used to encode the returned PNG. Capture IDs are 32 lowercase hexadecimal characters and are scoped to the extension process. Offline fallback captures do not receive an ID and cannot be used as live diff baselines.

`ViewportRenderer` retains a process-local least-recently-used cache with these hard limits:

- at most 8 captures;
- at most 64 MiB of raw pixels across all captures;
- at most 2,048 by 2,048 pixels per capture;
- checked multiplication before every allocation;
- eviction before insertion when either limit would be exceeded.

Cache misses return `404`. A process restart naturally invalidates every ID. No pixels or identifiers are written to disk.

## Named-Node Isolation Capture

When `viewport_capture_frame.node_isolation_path` is non-empty, capture is live-only and editor-only. The path must resolve inside the active edited-scene subtree. The target, its descendants, and its ancestor chain remain visible; unrelated `CanvasItem` and `Node3D` branches are temporarily hidden. The renderer records every changed object's identity and original visibility before applying the first change.

Isolation supports `isolation_background` values `original` and `transparent`, defaulting to `original`. Transparent capture is accepted only when the target viewport exposes a reversible transparent-background setting; unsupported targets return `409` rather than silently retaining the original background.

The renderer forces one draw after isolation, reads the viewport image, restores all changed visibility/background values in reverse order through an unconditional scope guard, and forces another draw after restoration. Restoration runs on success, capture failure, encoding failure, and C++ exception. If an object disappears during capture, the renderer skips the invalid identity, reports a restoration diagnostic, and returns an error rather than claiming a clean restoration. Isolation never registers UndoRedo and never persists scene edits.

The response adds `isolated: true`, canonical `node_isolation_path`, `isolation_background`, `temporarily_hidden_count`, `state_restored: true`, and the live capture ID. Non-isolated capture retains its current behavior.

## Viewport Diff Capture

`viewport_diff_capture` is live-only and consumes a prior live capture ID plus the same capture selectors supported by `viewport_capture_frame`:

```json
{
  "baseline_capture_id": "0123456789abcdef0123456789abcdef",
  "camera_identifier": "active_editor_view",
  "resolution": {"width": 1024, "height": 768},
  "node_isolation_path": "/root/Main/Player",
  "isolation_background": "original",
  "threshold": 0
}
```

- `threshold` is an integer `0..255`, default `0`. A pixel is changed when any RGBA channel's absolute delta is greater than the threshold.
- The tool captures a fresh comparison frame in the selected live editor session and stores it as a normal live capture.
- Baseline and comparison dimensions must match exactly. Phase 4 performs no implicit resizing or color-space conversion.
- Checked 64-bit arithmetic protects pixel counts, channel sums, and serialized sizes.
- The result contains baseline and comparison capture IDs, dimensions, threshold, `changed_pixels`, `total_pixels`, `changed_ratio`, per-channel mean absolute error, `max_channel_delta`, an optional inclusive changed bounding box, `identical`, isolation metadata, and live provenance.
- The second MCP content item is a PNG diff image. Unchanged pixels are transparent. Changed pixels encode absolute RGB deltas with opaque alpha so small changes remain locatable. The JSON metadata does not duplicate image Base64.

A missing/evicted ID returns `404`; malformed IDs return `400`; dimension mismatch returns `409`; and capture or restoration errors propagate without a partial success result.

## Threading, Routing, and Error Handling

- Project search executes only in the standalone process and never opens a runtime route.
- Reimport, isolation, capture-cache access, and diff generation execute only on the selected editor extension's registered main-loop callback.
- The standalone tool registry acquires a runtime route lease so a concurrent attach cannot redirect an in-flight live request.
- All public inputs are validated both in the standalone handler and again at the authoritative engine boundary where they can affect Godot or project files.
- Live errors preserve structured codes and provenance. A transport timeout after work starts quarantines only the exact route generation under the existing Phase 3 policy.
- Search returns bounded per-file diagnostics; live tools fail atomically at the response boundary and never return success metadata with missing images or incomplete restoration.
- Logs include search summaries without query or source contents, reimport lifecycle events without filesystem-absolute paths, isolation restoration failures, cache eviction counts, and diff summaries.

## Staged Red-Team and Iteration Plan

### Gate 1: Search boundary

After text and symbol search pass their functional tests, adversarial tests target lexical `..`, absolute/UNC paths, symlink/reparse escapes, build and `.godot` recursion, binary/NUL content, malformed UTF-8, long lines, comments/strings that mimic declarations, worst-case result volume, deterministic ordering, and integer/file-budget boundaries. Critical or Important findings are fixed before live engine work begins.

### Gate 2: Reimport lifecycle

After editor-backed reimport passes its happy-path integration test, adversarial tests target duplicate and mixed-validity batches, `.import` and `.godot` paths, game sessions, concurrent requests, shutdown while pending, idle-state races, timeout provenance, and path replacement between standalone and engine validation. The implementation proceeds only when invalid batches are mutation-free and every started request has an honest outcome.

### Gate 3: Visual state and arithmetic

After isolation and diff pass functional tests, adversarial tests target missing/off-tree/freed nodes, exceptions at every capture step, original invisible states, transparent-background rejection, state restoration ordering, capture-ID guessing and eviction, dimension mismatch, maximum images, alpha-only changes, threshold edges, sum overflow, and repeated capture/diff loops. The loop is bounded to one review, one fix pass, and one focused recheck unless a Critical issue remains.

### Gate 4: Integrated contract

The final review checks registry counts, execution-mode metadata, editor/game routing, MCP content shape, version and documentation consistency, smoke-fixture cleanliness, cache lifetime claims, and the exact roadmap boundary. All Critical and Important findings must be resolved before final verification.

## Verification

- Native tests cover search validation/traversal/extraction/bounds, public schemas and counts, live lease dispatch, capture-cache behavior, diff arithmetic/encoding, and state-guard restoration using small engine-independent helpers.
- TDD mutation checks prove tests fail for path containment removal, comment/string symbol leakage, missing two-frame idle confirmation, state restoration omission, dimension-check removal, threshold off-by-one, capture-cache overgrowth, and dishonest offline capture IDs.
- The Godot 4.5.1 integration harness searches project fixtures, reimports a copied SVG fixture, observes the editor return to idle, captures the unisolated scene, captures an isolated subject, verifies scene visibility afterward, mutates a visual property, obtains a diff with a non-empty bounding box, restores the property, and obtains an identical capture at the selected threshold.
- The fixture remains byte-identical except ignored Godot/build artifacts. Session descriptors and sockets are cleaned up.
- Release build, all native tests, complete Godot integration, MCP stdio smoke, documentation validator/link checks, and `git diff --check` pass from a clean tree.
- An independent adversarial code review finds no unresolved Critical or Important issue.

## Documentation and Release

README, changelog, roadmap, capabilities, tool reference, API specification, architecture, quickstart, LLM instructions, admin guide, developer guide, integration guide, resources/prompts, CI smoke, PR template, and the documentation validator must agree on version `1.4.0`, the 72-canonical/82-total registry, search bounds, editor-only reimport/isolation/diff behavior, live capture-ID lifetime, diff semantics, and honest provenance.

## Scope Check

The four capabilities share one verification loop: locate project code/assets, make or observe a change, ensure Godot imports it, capture the relevant node without persistent scene mutation, and compare raw live frames. They can be verified in one editor-backed harness while keeping offline search independently testable. The excluded import-pipeline, export, deep-language, UI, and enterprise controls are independent later milestones and are not required for Phase 4 completion.
