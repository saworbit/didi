# Security Policy

## Supported Versions

The current documented release is **1.6.0**. Security fixes are provided only for the current minor line.

| Version | Supported |
| ------- | --------- |
| 1.6.x | :white_check_mark: |
| 1.5.x | :x: |
| <=1.4.x | :x: |

## Security Boundary

Didi is local development tooling, not a remote or hostile-host isolation boundary. Phase 6 requires an explicit Godot project, includes a stable project key in each process-unique endpoint, and uses an OS-backed per-session lock to permit one MCP client at a time. Requests remain authenticated with a private 64-hex session token. POSIX defaults are owner-only; Windows grants the owning SID and local administrators and fails startup before pipe creation if that DACL cannot be constructed.

Mutation confirmation tokens are 64 lowercase hex characters, expire after 120 seconds, are single-use, and are bound to the exact tool, arguments, canonical project, execution mode, session ID, and route generation. Treat them as short-lived capabilities: do not log, persist, publish, or reuse them. Dry-run previews do not call mutation handlers.

[Managed Recovery](docs/MANAGED_RECOVERY.md) is opt-in process ownership and saved-file protection, not an OS sandbox. It launches only its owned headless editor against a new project copy. Launch and restart execute project code that may access external files or services with the local account's permissions; even an ordinary authorized read can trigger the single automatic restart. `runtime_recovery_status`, dry runs, and confirmation previews do not relaunch.

Recovery workspaces, checkpoints, preserved project copies, journals, and editor logs contain project-sensitive material and inherit local filesystem permissions. Checkpoint traversal refuses symlinks, reparse points, hardlinks, and special files and enforces portable path naming, but malicious same-account path-replacement races remain outside the security boundary. Five completed checkpoints are retained within per-copy bounds; accumulated containers, preserved copies, journals, and logs have no overall automatic cap. Operators must inspect, protect, archive, and clean up retained artifacts. Existing containers are for salvage and cannot be reused as a new managed workspace.

The blackboard is shared state between agents, not a trust boundary between them. Anything on a board was written by whatever called the tool, and Didi neither interprets nor executes it: values are stored and returned verbatim. Treat a value read from a board with the same caution as any other tool result, and never write a session token, confirmation token, or credential onto one. Boards are files under `.didi/blackboard/` inside the project, so they inherit the project's filesystem permissions and nothing narrower. A task lease records who claimed work; it is an agreement between cooperating agents, not an authentication check, and an agent that supplies another's `agent_id` is not prevented from doing so.

Never include a real session token or descriptor file in an issue, log excerpt, screenshot, test fixture, or documentation example. The editor console's **Copy report** is safe to paste: it reports the plugin and engine versions, the project, and the state of each check, and it cannot contain a token because the console never reads one. Its descriptor reader copies the fields it names and the token is not among them, and a test fails the build if any addon script starts naming it. `DIDI_SESSION_DIR` is a controlled deployment/test override: the operator is responsible for ensuring that override is not shared across OS users and has access controls appropriate to the host.

---

## Reporting a Vulnerability

If you discover a security vulnerability in Didi, please do **not** open a public issue.

Instead, report it through GitHub Private Vulnerability Reporting or contact the project maintainers directly. Include the Didi version, operating system, Godot version, session kind (`editor` or `game`), whether the default or an overridden descriptor directory was used, and the smallest safe reproduction. Redact tokens, user-specific paths, project content, and unrelated logs.

We appreciate your efforts to responsibly disclose findings and will investigate and patch confirmed issues promptly.
