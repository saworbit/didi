# Phase 3 final fix wave A: secure discovery, transport, and retirement

## Scope

This wave owns final-review findings 1, 2, and 9 only: per-user POSIX session discovery, bounded exact POSIX stream I/O, and identity-safe descriptor retirement. Routing, MCP/tool policy, bridge behavior, documentation, and CI are intentionally left to the other final-fix streams.

## RED evidence

The registry/retirement tests were written before the implementation changes. The initial Windows native run was **54 passed, 5 failed, 59 total**. Four failures were in this wave:

- `RuntimeSessions.RejectsReusedPidMetadata` returned a proven-stale descriptor instead of retiring it.
- `RuntimeSessions.StaleRetirementPreservesReplacement` did not preserve a pathname replacement injected after validated discovery.
- `RuntimeSessions.HostPublishesAtomicallyAndRemovesOnlyOwnedDescriptor` left normal-shutdown tombstones behind.
- `RuntimeSessions.HostRetirementCollisionPreservesSelectedDestination` retained the eventual successful retirement destination as well as the real collision.

The fifth failure was concurrent work outside this wave. POSIX-only regressions additionally encode the pre-fix failures directly:

- a response header split across four writes was rejected after the first short read;
- slow payload trickle received a fresh timeout on every fragment;
- an unauthenticated handshake could advertise a response up to 128 MiB;
- a signal-interrupted/short request write was treated as a terminal write failure.

A Linux execution attempt was made through WSL, but the host denied distribution startup with `Wsl/Service/CreateInstance/E_ACCESSDENIED`. The POSIX RED cases therefore remain platform-gated source tests for Linux/macOS CI rather than claimed local executions.

## Implementation and security decisions

### Per-user registry

- `DIDI_SESSION_DIR` remains the explicit override and is resolved lexically so a final symlink is not silently canonicalized past `O_NOFOLLOW` validation.
- POSIX defaults to `$XDG_RUNTIME_DIR/didi-sessions` when the XDG path is absolute; otherwise the fallback is the UID-qualified system-temporary path `didi-sessions-<effective-uid>`.
- Discovery opens the registry directory with `O_DIRECTORY | O_NOFOLLOW`, then validates its type, effective-UID ownership, and absence of group/other permission bits from `fstat` on that handle.
- Descriptors are opened relative to that directory with `openat(..., O_NOFOLLOW)`, validated as owner-only regular files from `fstat`, size-bounded to 64 KiB, and read from the same descriptor.
- POSIX publication opens and validates the directory handle, creates the temporary descriptor owner-only with `openat`, validates and syncs the opened file, then publishes with directory-relative `renameat` and syncs the directory.

### POSIX stream transport

- Client and server sockets are nonblocking and close-on-exec. `SO_NOSIGPIPE` on supporting systems or `MSG_NOSIGNAL` elsewhere prevents peer disconnects from terminating the process.
- Exact read/write helpers resume after `EINTR` and partial transfers and wait only for the remaining time under a monotonic deadline.
- A client request uses one deadline across request write, response header, and response payload. A slow peer cannot renew the budget by trickling bytes.
- The handshake response cap is 64 KiB; other frames retain the existing 128 MiB protocol cap.
- The server uses exact framed reads/writes with one five-second deadline per incoming frame and a bounded response-write deadline. Shutdown interrupts the active accepted socket.

### Descriptor retirement

- Normal shutdown and proven-stale discovery first inspect the exact owned descriptor and capture its native file identity.
- Retirement uses a cryptographically random, no-replace destination. A destination collision retries without overwriting it.
- After the move, content ownership and native file identity must still match. Deletion is issued against the verified Windows handle; POSIX deletion is directory-handle-relative after a final no-follow identity check.
- A successful normal or stale retirement deletes its tombstone. Files are retained only when an actual destination collision exhausts retries, identity/path replacement is observed, or an atomic/cryptographic operation is unavailable.
- Malformed or unverifiable descriptors are diagnosed and retained; only proven-stale process instances are retired.

## GREEN evidence at commit boundary

- Windows Release focused build: `didi_core` and `didi_tests` linked successfully.
- Shared native suite: **65 passed, 4 failed, 69 total**. Every registry, descriptor lifecycle, retirement, and Windows IPC regression owned by this wave passed. The four failures were concurrent Wave B RED tests (`RuntimeRouting.AutoAttachFirstAvailability`, `RuntimeRouting.AutoAttachEditorPreferenceAndAmbiguity`, `RuntimeRouting.AutoAttachRollback`) and its in-flight `Tools.CaptureViewportWithIpc` interaction.
- `git diff --check` on all six owned source/test files: clean.
- Linux live verification: attempted but unavailable because WSL distribution startup was access-denied. The implementation was manually audited for Linux/macOS declarations and feature guards (`renameat2`/`RENAME_NOREPLACE`, `renamex_np`/`RENAME_EXCL`, `getrandom`, `SO_NOSIGPIPE`/`MSG_NOSIGNAL`).
- Both-engine verification is intentionally deferred until immediately after this ownership-unblocking commit, per parent coordination; Wave B required `session_client.cpp` ownership to proceed and its intentional RED tests make the shared suite non-green at this boundary.

## Files changed

- `include/didi/runtime/session_client.hpp`
- `src/runtime/session_client.cpp`
- `src/gdextension/session_host.cpp`
- `src/common/ipc_channel_win32.cpp`
- `tests/test_runtime_sessions.cpp`
- `tests/test_ipc.cpp`
- `.superpowers/sdd/2026-08-27-phase-3-runtime-eval/final-fix-a-report.md`

## Self-review

- No routing, MCP server, runtime-tools, GDExtension router/bridge, tool-registry, documentation, or CI file is staged by this wave.
- Registry validation and descriptor reads use opened handles; pathname validation is not followed by a second untrusted stream reopen.
- Retirement never replaces an existing destination and never deletes an object whose native identity or descriptor ownership differs from the original.
- Timeout checks use `steady_clock`; no read or write loop resets its deadline.
- Transport failures quarantine the client socket before returning, preventing reuse after an unknown framing state.
- Existing Windows and Phase 1/2 native coverage remains enabled and passed except for explicitly identified concurrent Wave B RED cases.
