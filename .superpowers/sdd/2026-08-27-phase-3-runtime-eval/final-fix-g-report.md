# Phase 3 final fix wave G: absolute Windows IPC deadlines

## Scope

Wave G closes round-3 Important finding 1 only: a Windows IPC request now uses one absolute monotonic deadline across connection acquisition, every connection retry, request write, response header, and response payload. Runtime/session routing, tool/resource policy, documentation, and CI remain owned by the other final-fix streams.

## Finding verification

The finding was confirmed against `a6abb2b`:

- `sendRequest` used a fixed 500 ms connection budget before creating the caller's I/O deadline, allowing reconnect plus I/O to exceed the public request timeout.
- Every `ERROR_PIPE_BUSY` retry passed the original timeout to `WaitNamedPipeA`, so availability races could restart the wait budget.
- Non-busy connection retries always slept 50 ms, even when less time remained.
- `exactOverlappedIo` checked the deadline only after an operation became pending. A chain of immediately available synchronous completions could continue without observing an already-expired deadline.

## RED evidence

Three raw named-pipe regressions were added before the production edit. The baseline run was **90 passed, 7 failed, 97 total**. The three Wave G failures were:

- `IPC.Win32ConnectRetryDeadline`: a 10 ms missing-pipe budget overshot because the retry path slept for a fixed 50 ms.
- `IPC.Win32ReconnectSharesDeadline`: reconnect and response I/O did not share the caller's 100 ms budget, and the failure did not report the expected timeout state.
- `IPC.Win32QueuedResponseDeadline`: a zero-duration request accepted an already-queued synchronous response instead of rejecting the expired operation before I/O.

The other four failures were concurrent Wave H routing REDs and were outside this wave.

## Implementation

- `sendRequest` creates its `Win32Deadline` before any implicit connection attempt and passes that same object through connection, write, header, and payload work.
- Public `connect` also creates one absolute deadline for its complete retry loop.
- Every connection iteration checks expiry before `CreateFileA`.
- `WaitNamedPipeA` receives only the current rounded-up remaining duration; non-busy retry sleeps are capped to the remaining duration. Neither path reuses the original timeout.
- Every exact overlapped read/write iteration checks the same monotonic deadline before issuing another synchronous or asynchronous operation.
- Negative timeouts remain unbounded and retain the definitive-response behavior.

## GREEN evidence

- Focused behavior in the complete native binary after the production edit: all three Wave G regressions passed.
- Intermediate complete native run: **95 passed, 2 failed, 97 total**; the only failures were Wave H's in-progress `RuntimeRouting.ToolDispatchRouteBinding` and `RuntimeRouting.DisconnectedLeaseProvenance` REDs.
- Complete Release build: `didi_core`, `didi`, `didi_extension`, and `didi_tests` built successfully.
- Final complete native suite: **97 passed, 0 failed, 97 total**.
- Godot 4.5.1 full integration: passed (`Phase 1/2 editor workflows plus concurrent Phase 3 game tree and execution control`).
- Godot 4.7.2 full integration: passed (same complete workflow matrix).
- `git diff --check`: clean.

## Files changed

- `src/common/ipc_channel_win32.cpp`
- `tests/test_ipc.cpp`
- `.superpowers/sdd/2026-08-27-phase-3-runtime-eval/final-fix-g-report.md`

## Scoped self-review

- No request phase constructs or refreshes a deadline after `sendRequest` begins.
- No finite connection wait or retry sleep can deliberately exceed the remaining caller budget.
- Deadline checks cover both immediately completed and pending overlapped-I/O loop iterations.
- A pre-write expiry reports `request_started=false`, `outcome_unknown=false`, and `timed_out=true`; response-phase expiry preserves the established unknown-outcome/quarantine contract.
- Existing negative-timeout, stalled-write, fragmented-header, slow-trickle, handshake-cap, post-accept, malformed-response, and server-frame deadline coverage remains green.
- No runtime/session routing, resources, MCP metadata, documentation, or CI file is included in Wave G.
