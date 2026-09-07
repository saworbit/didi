# Managed editor recovery

Managed mode gives autonomous MCP work its own editor and project copy. It is opt-in at Didi startup; ordinary attachment keeps its existing behavior.

```powershell
didi --project "D:\MyGame" --managed-editor "C:\Godot\Godot_v4.5.1-stable_win64.exe" --recovery-workspace "D:\MyGame-recovery-01"
```

Enable the matching Didi addon in the source project first. Both new options are required together; the workspace must be outside the source project and its parent directory must exist. The Godot executable must be an absolute file path, and the workspace directory must not already exist. Didi copies saved project content into `<workspace>/project` and launches a headless editor there. MCP file tools target that copy. Your source project is unchanged; review and copy intended changes back yourself. Project code can access external files: this workspace is not an OS sandbox.

## Checkpoints and coverage

Managed mutations automatically snapshot saved files before dispatch and after successful completion. Supported scene edits also call Godot's checked `save_scene` for the active scene before the post-edit snapshot. Protected mutation results and selected managed-recovery error responses carry a compact `recovery` receipt: operation outcome, checkpoint ID, coverage and whether reconciliation is required. Successful ordinary reads do not carry this receipt. Query `runtime_recovery_status` for authoritative recovery state, especially after a successful read that may have triggered a restart, and for the full snapshot list and paths.

Startup and reattachment share a 30-second deadline for discovery, exact-child attachment, four quiet readiness polls, and a stable saved-file checkpoint before admitting edits. The internal `editor.getRecoveryState` method reports `filesystem_scanning`; importing or scanning is not readiness. Source inventory and content hashes are checked again before publishing each snapshot; a detected concurrent writer causes refusal, not a partial success. This is not an atomic filesystem transaction: coordinate external writers. Diagnose readiness failures in the current `editor-N.log`.

A checkpoint protects saved project files. It does not protect unsaved script buffers, unsaved external resources, other unsaved scenes, editor undo history, game state, or `user://`/external side effects. `shader_set_uniform`, `audio_configure_bus`, `signal_emit`, and `viewport_toggle_debug_draw` are tracked by name as transient/unprotected mutations. `runtime_checkpoint` snapshots files already on disk; it is not a command to save every editor buffer. Never treat a successful scene save as proof every resource was saved.

Five completed checkpoints are retained. Each holds at most 256 MiB and 10,000 files; traversal visits at most 30,000 paths. Windows-compatible path segments and case-insensitive uniqueness are required on every platform. `.git`, `.godot`, `.didi`, and `.worktrees` are excluded. Symlinks, reparse points, hardlinks and special files are refused. A failed snapshot blocks the next mutation; a post-edit failure reports that the operation may already have applied. Incomplete staging directories are never recovery points. Full manifests include SHA-256 content checks and remain on disk; tool responses return compact summaries.

## Recovery workflow

- `runtime_recovery_status`: inspect owned PID, checkpoint inventory, pending operation and recovery instructions. No restart occurs.
- `runtime_recover_editor`: use the single automatic restart budget after an abnormal exit. The exact newly spawned editor must authenticate before attachment. This never repeats an interrupted operation or clears its uncertainty.
- `runtime_checkpoint`: create an explicit saved-file checkpoint. If an outcome is unresolved, `accept_current_files: true` is allowed only once the uncertain editor is proven stopped. While that original editor is alive, use restore to discard in-memory uncertainty. After it has terminated and a new editor has reloaded saved files, explicit acceptance can reconcile those inspected files.
- `runtime_restore_checkpoint` with `checkpoint_id`: validate and stage a snapshot, stop the owned editor, preserve the old project as `<workspace>/preserved-...`, install the staged copy and launch a new editor. This is destructive and uses the existing dry-run/confirmation flow. `--yolo` can disable confirmation for an intentionally unattended host.

Recovery also runs before the next ordinary authorized tool request, without a background IPC thread. If that request is a mutation and recovery changes the session, the mutation is not started: inspect the recovered state and submit a fresh request. `runtime_recovery_status`, dry runs, and confirmation previews do not relaunch. Other authorized reads may invoke `ensureEditor()`, consume the single automatic restart, and execute project startup code before dispatch, even when the tool advertises `readOnlyHint: true`. Normal editor closure and a living/hung/unverifiable process do not trigger automatic restart. After one restart, another abnormal exit stops automation until explicit restore or a new managed invocation. Restore explicitly launches an editor but does not reset the automatic restart budget; only a new managed host invocation gets a fresh budget.

A transport failure may mean an edit completed but its response was lost. Didi preserves that result and blocks further mutations until reconciled. It never replays the edit. Managed mode refuses manual attach/detach to prevent crossing into a human editor. Closing the MCP host stops its owned child; it never stops an editor attached by ordinary mode.

## Artifacts and limits

`recovery.json`, `checkpoints/`, and per-launch `editor-N.log` live outside the mutable project. Crash reports stay in the project `.didi/crash`; restoring preserves them with the old project. Retained workspaces and logs are not automatically deleted or capped: inspect and remove them when no longer needed. A restored workspace can reimport assets because `.godot` is excluded.

The owned editor is stopped when Didi exits. On Windows the child is normally created inside a job object marked kill on close, so the kernel ends it when the last handle to that job goes with the process. If job-object creation or assignment fails, Didi logs the fallback and starts the editor without that containment; an abnormal host exit can then leave the managed editor running, so stop Didi normally or end the editor yourself.

On Linux the child asks for `SIGKILL` when its parent dies, so a killed host does not leave the editor behind. macOS has no equivalent mechanism and its spawn path cannot run code in the child, so on that platform an abnormally killed host also leaves the editor running; stop Didi normally there, or end the editor yourself.

This feature recovers a Godot editor crash while the MCP host survives. It does not automatically resume an existing container after an MCP/OS crash or claim power-loss durability. Every new managed invocation needs a new workspace directory. Retained containers, journals and completed checkpoints are for inspection and salvage, not reuse as a new `--recovery-workspace` or automatic host resume. Filesystem checks defend against malformed snapshots and ordinary links, not a malicious process racing path replacement under the same account.

## Verification

`tests/test_managed_recovery_live.py` and `tests/test_managed_recovery_adversarial.py` launch real Godot through MCP stdio, kill only their owned child, and check persistence, one restart, no mutation replay, restoration, source preservation, dry runs and corrupt-snapshot refusal. Set `DIDI_TEST_BINARY` to the built host and `DIDI_RECOVERY_GODOT` to an absolute Godot executable; then run `python -m unittest discover -s tests -p "test_managed_recovery*.py"`.
