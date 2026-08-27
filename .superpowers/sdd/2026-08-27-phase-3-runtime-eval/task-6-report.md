# Task 6 implementer report

Status: DONE

## Outcome

Expanded the disposable concurrent editor/game acceptance harness and hardened confirmed descriptor/log boundaries. The harness now uses a build-local `DIDI_SESSION_DIR`, tracks descriptor engine PIDs for deterministic cleanup, validates malformed descriptor diagnostics and token redaction, exercises attach/get/detach/reattach, exact one- and three-frame stepping, the 10,000-node traversal cap with a 10,001-child fixture, active-step shutdown cancellation, structured log shape, additional expression injection classes, and checked-in fixture cleanliness. All existing Phase 1/2 request coverage remains in place.

## RED evidence

1. Offline structured-log schema:
   - Added a real resource read assertion requiring `details: null` on the offline record.
   - RED: `Resources.DefaultRegistration` failed with `parsed["records"][0].contains("details")`; 52 passed, 1 failed.
   - Minimal fix: add `details: null` to the synthesized offline record.

2. Runtime tree cap and multi-frame state:
   - Added exact three-frame counter/eval/tree assertions and a true 10,001-child tree traversal.
   - RED against the unchanged 1,024-child fixture: `Runtime tree did not stop at the 10,000-node cap.`
   - Minimal fixture change: create 10,001 children and assert 10,000 returned nodes, 9,999 returned children, `children_truncated=true`, and `truncated=true`.

3. Endpoint prefix binding:
   - Added descriptor regressions for a prefix-only pipe and PID-mismatched endpoint.
   - RED: `RuntimeSessions.DescriptorRejectsWrongTokenLength` failed because `godot_didi_unrelated` was accepted.
   - Minimal fix: require the exact process/session-derived endpoint (`godot_didi_<pid>_<session_id>`; `.sock` on POSIX).

4. Active-step shutdown:
   - Added a dedicated game fixture mode that observes pause, exits on the first resumed step frame, and forces a pending 60-frame call through shutdown.
   - RED before fixture behavior: `Game shutdown did not cancel the active 60-frame step.`
   - GREEN: the waiting caller receives the engine's explicit `Godot main loop is shutting down` error and the game exits.

5. PID reuse metadata:
   - Added a descriptor using the current live PID with `started_at_ms=1`.
   - RED: `RuntimeSessions.RejectsReusedPidMetadata` reported the stale descriptor alive.
   - Minimal fix: compare descriptor start metadata to the OS process creation time (Windows and Linux, 30-second launch tolerance) before reporting liveness.

## GREEN evidence

- Release build: succeeded (`didi_core`, `didi`, `didi_extension`, `didi_tests`).
- Native suite: 55 passed, 0 failed, 55 total.
- Godot 4.5.1: full integration passed, including Phase 1/2 workflows, concurrent Phase 3 editor/game sessions, 10,001-node cap, expression matrix, active-step shutdown, exact-PID cleanup, and empty disposable descriptor directory.
- Godot 4.7.2: same full integration passed.
- PowerShell parser: `tests/run_godot_integration.ps1` parsed successfully.
- `git diff --check`: clean (only Git's informational LF-to-CRLF warning for the PowerShell worktree file).

## Files changed

- `src/mcp/resource_registry.cpp`
- `src/runtime/session_client.cpp`
- `tests/godot_smoke/runtime_probe.gd`
- `tests/run_godot_integration.ps1`
- `tests/test_runtime_sessions.cpp`
- `tests/test_tools.cpp`

No change was needed in `project.godot`, `runtime_main.tscn`, headers, or CMake.

## Adversarial coverage added or retained

- Per-run descriptor isolation; malformed JSON, oversized descriptor, and non-file descriptor diagnostics.
- Exact endpoint/PID/session binding and PID-reuse start-time liveness.
- Token omission from list/get responses and token absence from public MCP transcripts and engine stdout/stderr.
- Transactional attach behavior remains covered natively; acceptance now explicitly covers attach/get/detach/reattach.
- Atomic descriptor retirement collision and eight-attempt exhaustion safety; collision destinations are never overwritten. The disposable harness removes retained tombstones only after all tracked engines are stopped.
- Incremental log cursors and no-repeat behavior; live and offline records have a uniform `details` field; existing native tests retain 2,000-record, 16 KiB message, 64 KiB details, 500-read, gap/filter starvation, malformed UTF-8, and sequence-exhaustion coverage.
- Exact 1-frame and 3-frame state advancement with final pause verification.
- 10,001-child traversal capped at 10,000 nodes with explicit truncation metadata.
- Escaped quote success; Unicode identifier, semicolon, newline, comment, whitespace-obfuscated callable, dynamic dispatch, and `Callable` const-call bypass rejection. Existing native/integration coverage retains depth, output-size, non-finite, mutation, traversal, callback, source-size, timeout, and context constraints.
- Active 60-frame step is cancelled when its game shuts down.
- Checked-in source fixture remains free of generated phase artifacts.

## Scope deviations / concerns

- A concurrent second `runtime_step` cannot currently reach `EditorHook` end-to-end because the per-session IPC server processes requests synchronously. The production-used `RuntimeStepGate` is therefore covered directly, while active-step shutdown/cancellation and multi-frame behavior are exercised end-to-end.
- Pause-verification failure is not safely injectable through the public engine API without a purpose-built test seam; successful pause/resume/re-pause observation and invalid unpaused stepping are covered.
- Symlink/reparse rejection remains covered by the production check and non-regular-entry native/integration tests; creating a Windows reparse point is privilege-dependent and was not made a CI prerequisite.
- Linux/macOS execution was unavailable on this Windows host. Their explicit-header, process-identity, anchored-descriptor-read, and exact-socket-parent branches were implemented behind platform guards and reviewed statically; Windows received the full executable verification matrix.
- Retired descriptor tombstones intentionally remain fail-safe in production because path deletion after ownership verification has a replacement race. The harness removes only exact known-session, regular, non-reparse retirement files after proving no active descriptor remains.

## Self-review

- Confirmed endpoint validation matches `SessionHost` output on Windows and POSIX.
- Confirmed all fake descriptors use exact protocol endpoints and current timestamps, so tests exercise the production validator rather than bypassing it.
- Confirmed no token or expression source is printed by new assertions.
- Confirmed cleanup resolves and checks each direct artifact and never recursively removes descriptor-directory entries.
- Confirmed existing Phase 1/2 requests and assertions were not deleted or weakened.
- Mutation checks: relaxing endpoint equality, dropping start-time comparison, changing the node cap, omitting log details, returning success on shutdown, leaking tokens, or accepting any added injection makes at least one native/integration assertion fail.

## Review-fix pass

### Technical decisions

All six Important review findings were confirmed against the implementation and fixed; none was pushed back as technically incorrect.

1. **POSIX compile surface:** `session_client.cpp` now includes `<cerrno>`, `<unistd.h>`, `<fcntl.h>`, and `<sys/stat.h>` explicitly on POSIX. Linux uses `_SC_CLK_TCK`; macOS uses `proc_pidinfo` from `<libproc.h>`. WSL execution was unavailable on this Windows host (`Wsl/EnumerateDistros/Service/E_ACCESSDENIED`), so Linux/macOS branches were reviewed for their native declarations and are guarded per platform.
2. **Finite attach handshake:** descriptor attach uses a 3,000 ms handshake deadline instead of `kWaitForDefinitiveResponse`. A black-hole candidate now proves both a finite timeout and transactional retention of the previously healthy route.
3. **Process-instance identity:** `started_at_ms` is now the actual OS process creation time published by `SessionHost`, not preparation wall time. Windows uses `GetProcessTimes`, Linux uses `/proc/<pid>/stat` plus `/proc/stat` `btime` and `_SC_CLK_TCK`, and macOS uses `proc_pidinfo(PROC_PIDTBSDINFO)`. PID ranges and unavailable identities fail closed. Comparison tolerance is only the reporting resolution (1 ms on Windows/macOS; one Linux clock tick rounded up), with old-process and one-beyond-resolution mismatch regressions.
4. **Descriptor TOCTOU:** discovery parses from the validated open object. POSIX anchors `openat` to the descriptor directory, uses `O_NOFOLLOW`, then `fstat` and reads the same FD. Windows opens with `FILE_FLAG_OPEN_REPARSE_POINT`, rejects reparse/directory/non-disk handles, verifies the final handle parent, sizes through the handle, and reads that same handle. A deterministic hook swaps the pathname after validation; attach still authenticates with the originally opened descriptor.
5. **Exact teardown identity:** the integration harness records each descriptor's PID and process-start identity and compares both immediately before any `Stop-Process`. Normal editor teardown uses `CloseMainWindow` only after the same exact-instance check, then waits on descriptor PID+start rather than the launcher wrapper.
6. **Descriptor cleanup:** after exact-instance shutdown the harness first requires zero active `*.json` entries. It then removes only direct, regular, non-reparse files matching the exact known-session retirement filename; it never recursively deletes arbitrary descriptor-directory entries. This assertion produced a useful RED when the editor was force-killed, and went GREEN after exact-instance graceful editor shutdown.

The bounded minor findings were also closed:

- POSIX endpoints require both the exact filename and the canonical system-temporary parent.
- Token scanning now includes every response phase, including shutdown/failure/final responses, plus all nine process/engine log files.
- `RuntimeStepGate` is a production-used atomic reservation seam. Its direct native regression proves a second pending step is rejected and release permits the next step; end-to-end multi-frame and active-step cancellation remain in the Godot harness.

### Additional RED evidence

- Handshake timeout regression: `g_lastHandshakeTimeoutMs > 0` failed because the value was `-1` (`kWaitForDefinitiveResponse`).
- Process identity regressions: host publication differed from process creation time, and a descriptor one resolution unit beyond the real process start was incorrectly live.
- Validated-handle race: the new three-argument client test initially failed to compile because no validated-open seam existed; after the seam was introduced, the unchanged reopen implementation would authenticate the replacement token rather than the opened token.
- Direct pending-step guard: native compilation failed because `RuntimeStepGate` did not exist.
- Active descriptor lifecycle: the strengthened harness failed with an editor `*.json` remaining after forced termination. Exact-instance graceful editor close makes the host retire it before cleanup.

### Final GREEN evidence

- Release build: succeeded (`didi_core`, `didi`, `didi_extension`, `didi_tests`).
- Native suite: **57 passed, 0 failed, 57 total**.
- Godot 4.5.1: full Phase 1/2-preserving and Phase 3 adversarial integration passed, including strict active-descriptor absence and recognized tombstone cleanup.
- Godot 4.7.2: same full integration passed.
- PowerShell parser: `tests/run_godot_integration.ps1` parsed with zero errors.
- `git diff --check`: clean except Git's informational LF-to-CRLF worktree warning.

### Review-fix files changed

- `include/didi/gdextension/editor_hook.hpp`
- `include/didi/runtime/session_client.hpp`
- `src/gdextension/editor_hook.cpp`
- `src/gdextension/session_host.cpp`
- `src/runtime/session_client.cpp`
- `tests/run_godot_integration.ps1`
- `tests/test_runtime_sessions.cpp`
- `tests/test_tools.cpp`
- `.superpowers/sdd/2026-08-27-phase-3-runtime-eval/task-6-report.md`

### Review-fix self-review

- The attach candidate is not published until its finite handshake is semantically valid; every failure path disconnects only the candidate and leaves the prior route intact.
- The descriptor hook is injectable only through client construction and production defaults to no hook; production reads never reopen the validated pathname.
- Process identity retrieval is shared by publication and discovery, while tests independently query OS identity to avoid tautological coverage.
- Every pending-step reset path releases the production gate under `m_stepMutex`: resume failure, successful frame completion, and shutdown cancellation.
- The harness preserves the primary failure if forced failure cleanup leaves an active descriptor, while successful runs require no active descriptor and an empty directory after strict tombstone cleanup.
- No test/build scratch directory is retained or committed.

## Review-fix pass 2

Status: DONE

### Remaining Important finding

The cleanup harness no longer verifies one PID lookup and terminates through another. `Invoke-IdentityBoundProcessAction` obtains and pins the verified `System.Diagnostics.Process.SafeHandle` with `DangerousAddRef`, compares the expected process-start identity while that exact handle is held, and invokes `Kill()` or `CloseMainWindow()` on the same associated `Process` object before releasing the handle. `Stop-Process -Id` was removed from both engine and launcher cleanup paths.

The focused mutation seam starts an owned target and replacement process. Its post-verification callback substitutes the replacement lookup candidate, then the identity-bound terminator stops the already-held target object. The regression requires the target to exit, the replacement to remain alive, and the action to receive the exact verified object. The replacement is then cleaned up through the same identity-bound helper.

The handle behavior was also checked against the exact runtime executing the harness: PowerShell 7.6.4 loads `System.Diagnostics.Process` 10.0.10 (`f7d90799ce4ef09a0bb257852a57248d2a8fb8dd`). In that source, `SafeHandle` calls `GetOrOpenProcessHandle`, which stores the long-lived native handle and sets `_haveProcessHandle`; `Kill()` then calls `GetProcessHandle`, whose `_haveProcessHandle` branch returns a non-owning wrapper around that stored native handle before `TerminateProcess`. It therefore does not reopen the PID after verification. `DangerousAddRef` keeps the stored handle valid through the action.

### Bounded minors

- Descriptor native resources now use non-copyable RAII guards immediately after `CreateFileW`, `open`, and `openat`. Every validation return, a throwing `DescriptorOpenedHook`, and a throwing string allocation therefore closes the Windows handle or POSIX FD. `RuntimeSessions.ClosesValidatedHandleOnException` injects 32 throwing callbacks and requires the process handle/FD count to remain bounded.
- The shutdown-cancellation game descriptor token is now read, shape-validated, and checked alongside the editor and primary-game tokens across the final response transcript and all process/engine logs.
- A fully injected `EditorHook` bridge unit test was not added: `EditorHook` and the concrete runtime bridge are compiled only into the GDExtension shared-library target, while native tests link `didi_core`; directly instantiating the singleton also requires live Godot API state. Expanding the test target or exporting a test-only GDExtension API would exceed this scoped cleanup round and conflict with concurrent CMake work. The production gate has a direct native acquire/reject/release regression, while successful completion, resume/re-pause observation, re-use across sequential steps, and shutdown cancellation remain exercised on both real engines. Resume-failure cleanup is covered by the guarded production branch but remains without a purpose-built injected Godot bridge.

### RED and GREEN evidence

- RED inspection/mutation: cleanup called `Stop-Process -Id` after start-time verification, permitting a fresh PID resolution. The mutation seam models replacement of that lookup candidate.
- GREEN focused seam: held target exited and substituted replacement remained alive.
- RED inspection: descriptor handles/FDs were manually closed after a callback and allocation that can throw.
- GREEN native exception regression: 32 injected callback exceptions left the process handle/FD count bounded.
- Release build: passed.
- Complete native suite after the concurrent release-documentation changes stabilized: **58 passed, 0 failed, 58 total**.
- Runtime-session focused tests: passed, including validated-object TOCTOU and exception-path RAII.
- Godot 4.5.1 full integration: passed with identity-bound cleanup mutation probe and all three tokens scanned.
- Godot 4.7.2 full integration: passed with the same matrix.
- PowerShell parser: passed.
- Successful integration cleanup: zero active descriptors and zero remaining Godot processes.

### Review-fix pass 2 files

- `src/runtime/session_client.cpp`
- `tests/run_godot_integration.ps1`
- `tests/test_runtime_sessions.cpp`
- `.superpowers/sdd/2026-08-27-phase-3-runtime-eval/task-6-report.md`
