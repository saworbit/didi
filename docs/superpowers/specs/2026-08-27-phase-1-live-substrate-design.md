# Phase 1 Live Substrate Design

## Objective

Close the Phase 1 trust gap: an attached GDExtension must execute supported work on Godot's main thread against real editor objects, and every unsupported or offline path must identify itself honestly.

## Constraints

- Keep the existing stdio MCP server and local named-pipe/Unix-socket topology.
- Never call Godot object APIs from the IPC worker thread.
- Never report a live scene mutation, UndoRedo registration, editor operation, or viewport frame unless the corresponding Godot call completed.
- Preserve useful offline parsers, diagnostics, indexing, and file-based workflows with explicit offline provenance.
- Avoid new tool names and Phase 2 functionality.

## Chosen Architecture

The GDExtension registers Godot's native main-loop frame callback and drains `EditorHook` there. This replaces the non-functional `DidiHook` singleton lookup without adding another class binding or transport. On Godot versions that do not expose the callback, the IPC endpoint does not start, so the standalone server remains offline instead of timing out or executing engine work on a worker thread.

A focused `GodotBridge` wraps the vendored GDExtension C ABI. It owns Variant/StringName/NodePath conversion and exposes bounded operations for:

- live edited-scene hierarchy traversal;
- node instantiate, remove, reparent, property read/write, and duplicate;
- EditorUndoRedoManager transactions;
- editor undo, redo, save, and filesystem rescan;
- real editor viewport texture-to-image byte capture.

`EditorHook` remains the command router. It calls `GodotBridge` only after the queue reaches the main-loop callback. Existing offline handlers stay outside the bridge.

## Capability and Error Contract

Tool and resource definitions carry a Didi execution descriptor with supported modes (`live`, `offline_fallback`) and an `implemented` flag. `tools/list` and `resources/list` expose it. Tools that are registered for compatibility but still have no real implementation are marked `unimplemented` and their extension handlers return a structured 501 error.

Live results include explicit provenance such as `execution_mode: "live"`, `is_live_engine: true`, and, for viewport captures, `is_live_frame: true`. Offline results use `execution_mode: "offline_fallback"` and never claim a live frame or UndoRedo transaction.

IPC errors preserve numeric codes and messages. A missing edited scene, node, method binding, editor singleton, or unsupported JSON-to-Variant conversion is an error, not a default object or success response.

## Scene Mutation Semantics

Mutations use `EditorUndoRedoManager` actions. Instantiation and duplication add/remove the child through do/undo methods and keep the node alive for redo. Removal detaches and reattaches the same node. Reparenting performs the inverse parent operations. Property changes use do/undo property entries with the pre-change Variant. The bridge commits only after all operations have been registered.

Phase 1 supports JSON null, boolean, integer, real, and string property values. Composite Godot values remain explicitly unsupported until a typed conversion contract exists.

## Viewport Semantics

For live capture, the bridge resolves the requested editor SubViewport, reads its ViewportTexture image, obtains width, height, and RGBA8 bytes, and encodes those bytes to PNG. If any stage is unavailable, capture fails. The existing synthesized grid image remains available only as an attributed offline preview helper and can never set `is_live_frame: true`.

## Verification

- Unit tests prove discovery metadata distinguishes live/fallback/unimplemented tools and resources.
- Unit tests prove unattached extension handlers reject live-only calls and never emit fake success.
- Existing IPC tests prove structured errors propagate across the transport.
- A hidden-window Godot editor smoke fixture loads the extension and exercises the main-loop pump plus live hierarchy/property/UndoRedo/editor calls through the real pipe.
- The complete CMake build and native test suite must remain green.

## Scope Check

This design closes only the four Phase 1 substrate requirements. It does not add script attachment, project settings, runtime attach, eval, search, visual diffing, import/export, or enterprise isolation.
