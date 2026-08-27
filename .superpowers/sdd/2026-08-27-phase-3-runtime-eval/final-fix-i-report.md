# Phase 3 final routing correction I report

## Scope

Correction I closes the last unauthenticated managed-route path: every
`IRuntimeRouteLeaseProvider`, including every `IRuntimeSessionClient`, must return a lease with a
fully validated authoritative session descriptor. Descriptor-less compatibility remains available
only to plain, non-provider fixed `IIpcClient` transports.

ToolRegistry now selects a managed lease-aware dispatch adapter only for managed sources and a
plain exact-binding adapter for legacy fixed transports. This keeps handler execution bound to the
route selected at the registry boundary without turning a plain transport into a descriptor-less
managed provider. Managed sources without an authenticated lease fail live-only tools and live
resources with structured `503` responses and zero IPC. Their tools/resources metadata advertises
live APIs as unavailable and omits unverified session identity.

## TDD and red-team iteration

The new provider-only descriptor-less regression first produced RED at 98 passed / 1 failed / 99
total because `acquireRuntimeRouteLease` accepted `{client, descriptor: null, generation}`.

After enforcing the descriptor invariant, the suite deliberately exposed two compatibility details:

- a disconnected authenticated session must retain the existing offline-fallback advertisement;
- wrapping a plain fixed transport in a provider would incorrectly remove its legacy live path.

Availability now distinguishes an unauthenticated managed source from a disconnected session whose
selected identity still validates. The ToolRegistry adapter split preserves plain fixed transport
compatibility while keeping managed calls lease-bound and authenticated. Focused tests cover direct
lease rejection, structured tool/resource `503` responses, tools/resources metadata, zero dispatch,
and the existing fixed-transport live suite.

## Verification

- Full Release build: GREEN (`didi`, `didi_extension`, and `didi_tests`).
- Release native suite: GREEN, 99 passed / 0 failed / 99 total.
- Godot 4.5.1 integration: GREEN, including Phase 1/2 editor workflows and concurrent Phase 3 game
  tree/execution control.
- Godot 4.7.2 integration: GREEN with the same end-to-end coverage.
- `git diff --check`: GREEN.
