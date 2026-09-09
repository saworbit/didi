# Security Policy

## Supported Versions

The current documented release is **1.7.0**. Security fixes are provided only for the current minor line.

| Version | Supported |
| ------- | --------- |
| 1.7.x | :white_check_mark: |
| 1.6.x | :x: |
| <=1.5.x | :x: |

## Security Boundary

Didi is local development tooling, not a remote or hostile-host isolation boundary. Phase 6 requires an explicit Godot project, includes a stable project key in each process-unique endpoint, and uses an OS-backed per-session lock to permit one MCP client at a time. Requests remain authenticated with a private 64-hex session token. POSIX defaults are owner-only; Windows grants the owning SID and local administrators and fails startup before pipe creation if that DACL cannot be constructed.

Mutation confirmation tokens are 64 lowercase hex characters, expire after 120 seconds, are single-use, and are bound to the exact tool, arguments, canonical project, execution mode, session ID, and route generation. Treat them as short-lived capabilities: do not log, persist, publish, or reuse them. Dry-run previews do not call mutation handlers.

[Managed Recovery](docs/MANAGED_RECOVERY.md) is opt-in process ownership and saved-file protection, not an OS sandbox. It launches only its owned headless editor against a new project copy. Launch and restart execute project code that may access external files or services with the local account's permissions; even an ordinary authorized read can trigger the single automatic restart. `runtime_recovery_status`, dry runs, and confirmation previews do not relaunch.

Recovery workspaces, checkpoints, preserved project copies, journals, and editor logs contain project-sensitive material and inherit local filesystem permissions. Checkpoint traversal refuses symlinks, reparse points, hardlinks, and special files and enforces portable path naming, but malicious same-account path-replacement races remain outside the security boundary. Five completed checkpoints are retained within per-copy bounds; accumulated containers, preserved copies, journals, and logs have no overall automatic cap. Operators must inspect, protect, archive, and clean up retained artifacts. Existing containers are for salvage and cannot be reused as a new managed workspace.

The blackboard is shared state between agents, not a trust boundary between them. Anything on a board was written by whatever called the tool, and Didi neither interprets nor executes it: values are stored and returned verbatim. Treat a value read from a board with the same caution as any other tool result, and never write a session token, confirmation token, or credential onto one. Boards are files under `.didi/blackboard/` inside the project, so they inherit the project's filesystem permissions and nothing narrower. A task lease records who claimed work; it is an agreement between cooperating agents, not an authentication check, and an agent that supplies another's `agent_id` is not prevented from doing so.

Never include a real session token or descriptor file in an issue, log excerpt, screenshot, test fixture, or documentation example. The editor console's **Copy report** is safe to paste: it reports the plugin and engine versions, the project, and the state of each check, and it cannot contain a token because the console never reads one. Its descriptor reader copies the fields it names and the token is not among them, and a test fails the build if any addon script starts naming it. `DIDI_SESSION_DIR` is a controlled deployment/test override: the operator is responsible for ensuring that override is not shared across OS users and has access controls appropriate to the host.

---

## Automated Checks

These run continuously so a defect does not depend on someone thinking to look
for it. Findings land in the repository's
[Security tab](https://github.com/saworbit/didi/security).

| Check | What it covers | When it runs |
| --- | --- | --- |
| [CodeQL](.github/workflows/codeql.yml) | The C++ that parses JSON-RPC off a pipe, the Python tooling, and the workflows. `security-extended` query set. | Every pull request that touches code, and weekly |
| [OpenSSF Scorecard](.github/workflows/supply-chain.yml) | Branch protection, token permissions, pinned dependencies, dangerous workflow patterns. Score is public behind the README badge. | Push to `main`, weekly, and when branch rules change |
| [Dependency review](.github/workflows/supply-chain.yml) | A dependency arriving with a known vulnerability or a copyleft licence. | Every pull request |
| [zizmor and actionlint](.github/workflows/lint.yml) | Workflow security: injectable `${{ }}` interpolation, over-broad tokens, credentials left on disk by `checkout`. | Every pull request |
| [Sanitizers](.github/workflows/ci.yml) | ASan and UBSan over the whole native suite: real allocation and lifetime paths. | Every pull request that touches code |
| Secret scanning with push protection | A credential committed by accident, blocked at push time. | Every push |
| Dependabot | Security and version updates for the GitHub Actions and the pinned Python dependency. | Weekly and monthly |

Every action a workflow runs is pinned to a commit SHA rather than a tag, and
`tools/validate_documentation.py` fails the build on any workflow that is not.
See [THIRD_PARTY.md](THIRD_PARTY.md) for the vendored sources no scanner can
reach.

---

## Verifying a Release

Didi ships prebuilt binaries, so the question "did this archive come from that
source" has to be answerable by someone who has only the download. Every
release carries signed [SLSA build provenance](https://slsa.dev/provenance/v1)
generated by the release workflow through Sigstore. There is no long-lived
signing key: the certificate is issued to the workflow run's own OIDC identity
and expires in minutes, so there is nothing for a maintainer to leak, rotate,
or lose.

Each release contains, alongside the platform archives:

| Asset | What it is for |
| --- | --- |
| `SHA256SUMS` | The archives are intact and match what was built. |
| `didi-<tag>.intoto.jsonl` | The signed provenance bundle, for verifying offline. |

**The check worth running.** Each constraint is doing work, and dropping any of
them weakens the answer:

```bash
gh attestation verify didi-linux-x64.tar.gz \
  --repo saworbit/didi \
  --signer-workflow saworbit/didi/.github/workflows/release.yml \
  --source-ref refs/tags/v1.6.0
```

| Constraint | What it rules out |
| --- | --- |
| `--repo` | An attestation from some other project entirely. |
| `--signer-workflow` | Anything in this repository other than the release workflow. An attacker can sign something of their own; they cannot produce a signature attributed to this workflow. |
| `--source-ref` | **A rehearsal artifact.** The release workflow can also be run manually, and those runs sign too, so their provenance carries the same repository and the same signer workflow. Only the ref separates a published release from a dry run, so pin it to the tag you are verifying. |

**Offline**, using the bundle from the release rather than the GitHub API. The
constraints are the same: verifying offline is about not calling the API, not
about checking less.

```bash
gh attestation verify didi-linux-x64.tar.gz \
  --bundle didi-v1.6.0.intoto.jsonl \
  --repo saworbit/didi \
  --signer-workflow saworbit/didi/.github/workflows/release.yml \
  --source-ref refs/tags/v1.6.0
```

**Checksums**, if all you want to know is that the download is undamaged:

```bash
sha256sum --check --ignore-missing SHA256SUMS
```

`SHA256SUMS` is itself covered by the attestation, so it cannot be swapped
independently of the archives it describes. A failed verification means the
file is not what this project published: do not run it, and please
[report it](#reporting-a-vulnerability).

---

## Reporting a Vulnerability

If you discover a security vulnerability in Didi, please do **not** open a public issue.

Report it through [GitHub Private Vulnerability Reporting](https://github.com/saworbit/didi/security/advisories/new), which is enabled on this repository, or contact the project maintainers directly. Include the Didi version, operating system, Godot version, session kind (`editor` or `game`), whether the default or an overridden descriptor directory was used, and the smallest safe reproduction. Redact tokens, user-specific paths, project content, and unrelated logs.

You can expect an acknowledgement within 5 working days and an assessment of whether the report is confirmed within 14. A confirmed vulnerability is fixed on the current minor line and disclosed through a GitHub Security Advisory once a fix is available. If a report turns out to fall outside the security boundary described above, that is said plainly with the reasoning, rather than left unanswered.

We appreciate your efforts to responsibly disclose findings and will investigate and patch confirmed issues promptly.
