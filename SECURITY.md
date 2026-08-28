# Security Policy

## Supported Versions

The current documented release is **1.4.0**. Security fixes are provided only for the current minor line.

| Version | Supported |
| ------- | --------- |
| 1.4.x | :white_check_mark: |
| 1.3.x | :x: |
| <=1.2.x | :x: |

## Security Boundary

Didi is local development tooling, not a remote or hostile-host isolation boundary. Phase 3 publishes one access-controlled descriptor and process-unique named pipe or Unix-domain socket for each editor/game process. Requests are authenticated with a private 64-hex session token. POSIX defaults are owner-only; Windows grants the owning SID and local administrators and fails startup before pipe creation if that DACL cannot be constructed.

Never include a real session token or descriptor file in an issue, log excerpt, screenshot, test fixture, or documentation example. `DIDI_SESSION_DIR` is a controlled deployment/test override: the operator is responsible for ensuring that override is not shared across OS users and has access controls appropriate to the host.

---

## Reporting a Vulnerability

If you discover a security vulnerability in Didi, please do **not** open a public issue.

Instead, report it through GitHub Private Vulnerability Reporting or contact the project maintainers directly. Include the Didi version, operating system, Godot version, session kind (`editor` or `game`), whether the default or an overridden descriptor directory was used, and the smallest safe reproduction. Redact tokens, user-specific paths, project content, and unrelated logs.

We appreciate your efforts to responsibly disclose findings and will investigate and patch confirmed issues promptly.
