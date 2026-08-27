# Phase 3 whole-branch fix wave E report

## Scope

Wave E addresses whole-branch red-team findings 2 and 4:

- exact-route leases binding a client, token-free descriptor, and route generation to one live call;
- generation-conditional quarantine so an old failed call cannot disconnect a newly attached route;
- selected-session-kind enforcement before MCP tool dispatch and again inside the extension;
- structured wrong-kind `409` responses with zero IPC/main-thread dispatch.

Wave D's prerequisite structured transport API landed first. Wave F retained documentation
ownership; Wave E changed only routing, registry, extension-policy, and focused test files.

## TDD evidence

### RED: authoritative kind enforcement

The first focused build failed to link because the extension-side rejection seam did not exist.
Before implementation, direct `tools/call` on a connected game still dispatched viewport and editor
tools despite unavailable metadata. The regressions cover viewport capture, scene inspection and
mutation, editor save, editor-to-game runtime control, and direct authenticated extension methods.

### GREEN: authoritative kind enforcement

ToolRegistry now rejects live calls outside the selected descriptor kind before invoking a handler.
The extension independently applies the same method policy before posting to the Godot main thread.
Both boundaries return token-free structured live `409` envelopes and the fake transport records
zero dispatches.

## Route lease and final verification

### RED: mutable-router quarantine race

A blocked `runtime.step` was started on game route A, route B was explicitly attached, and the old
request then returned a structured started/unknown transport deadline. The regression failed because
the old call invoked `disconnect()` on the mutable session router, disconnecting B. This reproduced
the exact misattribution/quarantine race without timing sleeps.

### GREEN: exact route lease

`RuntimeRouteLease` snapshots the underlying IPC client, authoritative descriptor, and route
generation under one session-client lock. It injects the token only into that exact client's request.
Live tool and resource execution and provenance now use that same lease. Quarantine compares the
generation, client identity, and session identity before clearing the selected route; an old lease
cannot affect a newer attachment. Structured Wave D transport state determines truthful
`not_started` versus `unknown_outcome` attribution.

The blocked A/new B regression now returns A's token-free provenance while B remains selected and
connected with zero disconnects.

## Red-team iteration

The first independent review found four material gaps and the wave was not committed:

- ToolRegistry checked a lease but generic handlers still dereferenced the mutable router.
- generic editor handlers flattened authoritative transport errors before exact-route quarantine;
- alternate session routers could receive a fabricated generation-zero mutable lease;
- `godot://editor/state` retained a selected descriptor after structured transport failure.

Focused regressions produced a 86 passed / 3 failed / 89 total RED. The final implementation binds
every ToolRegistry invocation to an exact per-thread lease frame, preserves structured live errors
centrally, conditionally quarantines only that lease, fails closed for non-atomic session routers,
and applies the same transport-state handling to editor state.

Re-review then exposed two subtler call-boundary cases: attach B may disconnect leased A before a
generic handler's connectivity check, and a nested no-lease call could inherit an outer TLS frame.
The proxy now drives bound calls through exact `sendRequest` so disconnected A yields a structured
503 with A provenance and zero B dispatch. Every nested call pushes a shadow frame, and offline
results never inherit live provenance. Deterministic regressions cover both cases. The second
independent re-review approved the result with no remaining actionable findings.

## Verification

- Full Release build: GREEN (`didi`, `didi_extension`, and `didi_tests`).
- Release native suite: GREEN, 92 passed / 0 failed / 92 total.
- Godot 4.7.2 integration: GREEN, including Phase 1/2 editor workflows and concurrent Phase 3 game
  tree/execution control.
- Godot 4.5.1 integration: GREEN with the same end-to-end coverage.
- Internal re-review: GREEN; no remaining actionable findings.
- `git diff --check`: GREEN.
