# Phase 3 final fix wave D: exact transport deadlines

## Scope

Wave D closes red-team findings 1, 3, and 6 plus minor 2: exact Windows IPC deadlines, structured transport outcome state, bounded handshake responses, identity-safe POSIX tombstone handling, and secure fallback for a relative `XDG_RUNTIME_DIR`. Runtime tool policy, resource routing, MCP behavior, documentation, and CI remain owned by the other final-fix streams.

## Finding verification

All reported findings were confirmed against the baseline at `47c1a63`:

- The Windows client used a synchronous `WriteFile` before starting its response timer, so a peer that accepted but did not read could block beyond the caller deadline.
- Windows response reads polled path state rather than issuing deadline-bound overlapped exact reads; the server's exact read/write helpers waited infinitely.
- Windows handshake responses retained the general 128 MiB cap instead of the 64 KiB unauthenticated cap already used on POSIX.
- Transport handling exposed only error text. `runtime_tools` had to infer timeout/unknown-outcome state from message prefixes.
- Malformed Windows responses returned an error without closing the connection, allowing a desynchronized route to be reused.
- POSIX retirement used `fstatat` followed by `unlinkat`; no verification can make those two pathname operations atomic against a final substitution.
- A relative `XDG_RUNTIME_DIR` returned an error instead of falling back to the UID-qualified temporary registry.

## RED evidence

The Windows raw named-pipe tests were added first. The initial run was **78 passed, 6 failed, 84 total**. Five failures were Wave D REDs:

- `IPC.Win32WriteDeadline` took longer than 300 ms because the 8 MiB synchronous write waited for the stalled peer to close.
- `IPC.Win32TrickleState` had no structured request-started/unknown-outcome state.
- `IPC.Win32HandshakeCap` waited for the advertised 256 KiB handshake payload instead of rejecting its header immediately.
- `IPC.Win32PostAcceptFailure` had no structured transport data.
- `IPC.Win32ServerFrameDeadline` left a connection alive after a partial request header stalled for 1.5 seconds.

The sixth failure, `RuntimeRouting.WrongKindToolDispatch`, was concurrent Wave E work and became green without a Wave D edit. The fragmented-header case was already green and was retained as a regression. POSIX findings were demonstrated by code-path inspection and platform-gated tests; local WSL became available but its Docker WSL distribution contains no C/C++ compiler, CMake, Ninja, or Make.

## Implementation

### Exact Windows transport

- Client named-pipe handles are opened with `FILE_FLAG_OVERLAPPED`.
- Exact read/write operations use a single `steady_clock` deadline, resume partial completions, handle completion/cancellation races, and keep each `OVERLAPPED` alive until cancellation is reaped.
- The client deadline covers request write, four-byte response header, and the complete response payload. A negative deadline preserves the documented definitive-response wait.
- Windows handshake responses are capped at 64 KiB; other frames retain the 128 MiB protocol cap.
- Server request header and payload share one one-second monotonic deadline. Response writes have a separate one-second deadline and server stop interrupts either operation.
- The server no longer calls blocking `FlushFileBuffers` after a failed/stalled response write.

### Structured failure state

`include/didi/common/ipc_channel.hpp` now exposes:

- `ipc::TransportFailureState { request_started, outcome_unknown, timed_out }`
- `ipc::transportFailure(message, state)`
- `ipc::transportFailureState(const Error&)`

Transport failures use code 504 when deadline-expired and 502 otherwise, with the stable JSON shape:

```json
{"transport":{"request_started":true,"outcome_unknown":true,"timed_out":true}}
```

A failed/incomplete request write reports not-started and known-not-executed. Once the complete request frame is written, response read, framing, size, and parse failures report started with an unknown outcome. Every transport/framing/malformed-response failure closes the client handle/socket before return. Definitive server error responses remain ordinary application errors and do not receive transport state.

### POSIX retirement and registry

- POSIX has no portable object-bound unlink for an already-open regular file. `unlinkat(fd, "", AT_EMPTY_PATH)` is not a supported unlink primitive, and a second `fstatat` cannot close the remaining substitution window.
- Retirement therefore performs the cryptographic no-replace move, validates ownership and native identity from the opened object, removes the active `.json` discovery name, and retains the unpredictable `.didi-retired-<session-id>-<32hex>` tombstone with `retained_unavailable`.
- Windows retains object-bound deletion through `SetFileInformationByHandle` and normally returns `deleted`.
- A final-delete test seam injects a replacement after the last pathname check. POSIX detects/retains both objects and returns `retained_collision_or_race`; it never deletes the replacement.
- A relative `XDG_RUNTIME_DIR` now falls back to `<system-temp>/didi-sessions-<effective-uid>`. A valid absolute XDG path still uses `$XDG_RUNTIME_DIR/didi-sessions`.

## GREEN evidence

- Release build: `didi_core`, `didi`, `didi_extension`, and `didi_tests` all built successfully.
- Complete native suite: **85 passed, 0 failed, 85 total**.
- Godot 4.5.1 full integration: passed.
- Godot 4.7.2 full integration: passed.
- `git diff --check` on all owned source/test/report files: clean.
- WSL platform probe: Linux kernel started successfully, but the installed Docker WSL distribution has no native compiler/build toolchain; POSIX branches were statically audited and remain covered by platform-gated native tests for CI.

## Files changed

- `include/didi/common/ipc_channel.hpp`
- `include/didi/runtime/session_client.hpp`
- `src/common/ipc_channel_win32.cpp`
- `src/runtime/session_client.cpp`
- `tests/test_ipc.cpp`
- `tests/test_runtime_sessions.cpp`
- `.superpowers/sdd/2026-08-27-phase-3-runtime-eval/final-fix-d-report.md`

## Self-review

- Windows I/O deadlines never reset between write, header, and payload phases.
- Cancellation is followed by `GetOverlappedResult(..., TRUE)` so stack-owned `OVERLAPPED` storage and buffers outlive kernel access.
- The client is quarantined for all unknown framing states, including invalid sizes and malformed JSON.
- The handshake cap is method-specific and enforced before response allocation.
- Server slow-frame and stalled-write paths are bounded and stop-aware.
- POSIX retirement makes the conservative API outcome explicit instead of claiming deletion that cannot be identity-bound.
- No runtime tool/resource/MCP policy file, documentation file, or CI file is included in this wave.
