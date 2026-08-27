# Whole-branch fix wave C report

## Scope

- Whole-branch finding 8: bound every runtime-tree string field and the aggregate serialized payload, with explicit truncation.
- Reconcile the governing expression design/plan and Phase 3 public documentation.
- Add missing public JSON Schema constraints and harden the documentation/CI checks.

## TDD record

### Runtime tree and schemas

- RED native: `Tools.HonestCapabilities` failed because `runtime_read_logs.cursor` had no non-negative schema minimum. The same test now covers the 32-hex session ID, 1,024-byte runtime/context paths, and 2,048-byte expression contracts.
- RED Godot 4.7.2: the existing 10,001-child fixture reached the 10,000-node cap without any serialized-response limit (`Runtime tree did not stop at the serialized response budget before the node cap.`).
- Added a live multibyte oversized-name/path probe and wide-tree assertions before implementation.
- RED combined-route rerun: compact engine accounting still produced a 271,471-byte public payload because `CallToolResult` pretty-prints JSON after adding session provenance.
- GREEN implementation: names/types/paths are UTF-8-normalized and capped at 1,024/256/4,096 bytes; truncation is field-explicit; traversal conservatively charges pretty-serialized bytes at absolute nesting depth, reserves for the validated session-provenance envelope, and stops before the 256 KiB public tool-payload budget, reporting `max_response_bytes`, `node_count`, and truncation flags.

### CI live cursor contract

- RED native: changed the connected fake-runtime assertions to require a sequenced live page and duplicate-free second cursor read; the prior error-only fake failed at `!live_logs.isError`.
- GREEN fixture: the fake IPC endpoint now implements `runtime.getLogs` cursor `42 -> 43`, returns no duplicate at cursor `43`, supplies live resource provenance, and recognizes the exact internal `runtime.evalGdscript` route.

## Documentation and CI reconciliation

- Amended the governing design and plan to record deterministic unambiguous auto-attach, fresh `runtime_get_session` identity handshakes, the deliberately stricter receiver-aware expression sandbox, omitted expression-source provenance, exact `runtime.evalGdscript` method, and runtime-tree byte limits.
- Documented default POSIX owner-only controls, the Windows owner-plus-local-administrators ACL, and operator responsibility for `DIDI_SESSION_DIR` overrides.
- Documented exact-owned shutdown/proven-stale retirement and normal deletion, with retained files only for proof-safety failure cases.
- Replaced the stale fixed Phase 1/2 harness/native-count wording with the 76-test Phase 3 release matrix and authoritative runner count.
- CI selects one Python interpreter, rejects repository-escaping Markdown targets, validates Markdown anchors, asserts the public Phase 3 schema limits, and relies on the native connected fake for the live cursor contract rather than presenting the standalone offline resource as live evidence.

## Verification

- Release build: passed (MSVC Release; server, extension, and tests linked).
- Native suite: passed, 76/76.
- Godot 4.5.1: passed the complete concurrent editor/game integration harness against the final Release build.
- Godot 4.7.2: passed the complete integration harness, including the new oversized-name and wide-tree assertions.
- Exact MCP smoke: passed locally.
- Markdown link/anchor validator: passed locally across 19 Markdown files.
- CI YAML parse: passed locally.

## Pushback / residual limitations

- The 256 KiB runtime-tree limit applies to the public tool payload, including token-free session provenance, but not the outer MCP/JSON-RPC string framing. The engine serializer conservatively reserves 72 KiB for the router-added provenance and fixed metadata because descriptor input is capped at 64 KiB.
- Evaluation deadlines remain cooperative, not preemptive. The stricter grammar/receiver surface is the security control that makes that claim honest.
