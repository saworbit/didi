# Phase 3 final routing patch H report

## Scope and contract

Patch H closes the remaining public live-routing gaps after Wave G's absolute Windows IPC deadline
fix:

- every lease-bound public live request has a central 17-second maximum, including handlers and
  resources that request an unbounded wait;
- a selected game session makes `godot://editor/state` unavailable and a direct read returns a
  token-free structured live `409` with game provenance and zero IPC dispatch;
- actual runtime session routers must supply an atomic lease with a fully validated descriptor;
- provider-only managed routes receive the same kind/no-lease enforcement as session routers;
- plain fixed `IIpcClient` transports retain the intended legacy compatibility path.

## TDD evidence

The initial H regressions produced a RED native run of 90 passed / 7 failed / 97 total. Four failures
were H's expected routing gaps (`KindAwareAvailability`, `LiveResourceProvenanceAndKind`,
`PublicLiveDeadlineClamp`, and `DescriptorlessSessionFailsClosed`); the other three were Wave G's
concurrent expected Windows IPC deadline failures.

The implementation introduced a central `RuntimeRouteLease::sendRequest` clamp: negative or larger
timeouts become 17,000 ms, while shorter finite deadlines remain unchanged. Regressions record the
effective timeout for a generic editor tool, editor-state, and runtime logs. Editor-state now applies
the selected-kind policy before transport and emits the same token-free structured live envelope as
tool rejection. Session leases are accepted only when their descriptor round-trips through full
validation; invalid or descriptor-less session routers fail closed without dispatch or quarantine.

## Red-team iteration

Two existing route-swap tests initially failed after descriptor validation because their fixtures
changed session IDs without updating platform-specific endpoints. The fixtures now rebuild valid
Windows pipe or POSIX socket endpoints, preserving the intended generation-race coverage.

Independent review then found one Important provider-only bypass: ToolRegistry treated only
`IRuntimeSessionClient` as managed, so another `IRuntimeRouteLeaseProvider` could advertise a game
route yet dispatch an editor tool. A dedicated regression produced RED at 97 passed / 1 failed / 98
total. ToolRegistry now treats every lease provider as managed for kind and no-lease enforcement,
while only plain fixed transports use the permissive compatibility path. The provider-only game
case now returns structured `409` with zero sends, and a missing provider lease returns structured
`503` for live-only tools with zero sends. Independent re-review approved the correction with no
Critical or Important findings.

## Verification

- Full Release build: GREEN (`didi`, `didi_extension`, and `didi_tests`).
- Release native suite: GREEN, 98 passed / 0 failed / 98 total.
- Godot 4.5.1 integration: GREEN, including Phase 1/2 editor workflows and concurrent Phase 3 game
  tree/execution control.
- Godot 4.7.2 integration: GREEN with the same end-to-end coverage.
- Independent re-review: GREEN; no Critical or Important findings.
- `git diff --check`: GREEN.
