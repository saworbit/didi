# Phase 3 final CI regression J report

## Scope

Regression J restores the documented offline resource contract for a legitimate runtime session
manager when no session is selected, without reopening unauthenticated provider routing.

The routing state distinction is now explicit:

- a real `IRuntimeSessionClient` with `activeSession() == null` is idle and may execute declared
  offline resource fallbacks;
- a session client with a selected route but no authenticated lease is unavailable/fail-closed;
- a non-session `IRuntimeRouteLeaseProvider` with no authenticated lease remains malformed and
  fail-closed;
- a provider-only descriptor-less lease remains rejected at acquisition.

## TDD evidence

The observed CI behavior was reproduced through the public MCP server boundary. The new regression
asserts `resources/list` metadata and `resources/read` payloads for both `godot://editor/state` and
`godot://runtime/logs` with an idle session manager. RED was 99 passed / 1 failed / 100 total because
both resources were advertised unavailable and reads returned structured `503` errors.

The corrected classification restores `currentMode: offline_fallback`, keeps live/editor flags
false, emits no session identity, and returns the documented offline editor status plus the
cursor-shaped runtime-log record (`records`, `next_cursor`, `oldest_cursor`, and
`dropped_before_cursor`). The existing descriptor-less provider regression remains GREEN and proves
structured `503`, unavailable metadata, and zero dispatch.

## Verification

- Exact CI end-to-end MCP smoke from `.github/workflows/ci.yml`: GREEN (`Fast CI MCP verification
  complete`), including 68 canonical/78 total registrations and cursor-shaped offline logs.
- Full Release build: GREEN (`didi`, `didi_extension`, and `didi_tests`).
- Release native suite: GREEN, 100 passed / 0 failed / 100 total.
- Godot 4.5.1 integration: GREEN, including Phase 1/2 editor workflows and concurrent Phase 3 game
  tree/execution control.
- Godot 4.7.2 integration: GREEN with the same end-to-end coverage.
- `git diff --check`: GREEN.
