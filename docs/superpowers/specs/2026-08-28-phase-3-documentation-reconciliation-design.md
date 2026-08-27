# Phase 3 Documentation Reconciliation Design

**Date:** 2026-08-28  
**Status:** Approved  
**Scope:** Current-facing project documentation and its automated drift checks

## Objective

Reconcile every current-facing Didi document with the Phase 3 implementation merged in version 1.3.0, repair the stale supported-version policy, and prevent the same high-value facts from silently drifting in later releases.

## Sources of truth

Documentation claims must be checked against these sources, in descending order:

1. Runtime `initialize`, `tools/list`, and `resources/list` responses for public version, registration, capability, and availability behavior.
2. Production constants and schemas for protocol versions, timeouts, payload limits, session identity, and evaluator bounds.
3. Native and Godot integration tests for supported behavior and verified engine versions.
4. The merged Phase 3 acceptance record in `CHANGELOG.md` and `docs/ROADMAP.md`.

When prose conflicts with executable discovery or tests, the prose changes. Historical design specs, implementation plans, and review reports remain records of decisions at their original time and are not rewritten to describe the current release.

## Documentation changes

### Security and support policy

`SECURITY.md` will mark only the current `1.3.x` minor line as supported. Versions `1.2.x` and earlier will be marked unsupported. The policy will describe Phase 3's local attachment boundary, private session tokens and descriptors, the operator-owned `DIDI_SESSION_DIR` override, and the kinds of security reports maintainers need to reproduce an issue without exposing secrets.

### User and integration guidance

`README.md`, `docs/QUICKSTART.md`, and `docs/INTEGRATION_GUIDE.md` will retain the verified 68-canonical/78-total surface and Phase 3 session workflow. Platform-specific executable and extension-library names will be stated without presenting Windows `.exe`/`.dll` names as the universal topology. The README will identify version 1.3.0 as the current documented release and link directly to the security policy.

### Architecture and operations guidance

`docs/ARCHITECTURE.md`, `docs/API_SPECIFICATION.md`, and `docs/ADMIN_GUIDE.md` will use consistent `named pipe / Unix-domain socket` terminology. They will preserve the authenticated local-only threat model, owner-only POSIX defaults, Windows owner/administrator ACL, descriptor registry rules, deadline semantics, and the distinction between structured Didi logs and process stdout.

### Contributor and release guidance

`CONTRIBUTING.md` and `docs/DEVELOPER_GUIDE.md` will include a documentation reconciliation checklist. A release-changing contribution must update the CMake project version, MCP server version, standalone version output, addon plugin version, changelog, capability matrix, README, and security support table together. Tool-surface changes must update discovery tests, tool reference, capability matrix, roadmap, LLM instructions, and integration examples.

`CHANGELOG.md` will record this documentation and validation work under `Unreleased`; the historical 1.3.0 entry will not be edited to pretend the follow-up shipped in the original merge.

## Automated drift validation

A dependency-free Python validator will run in the fast documentation workflow and locally. It will:

- require the critical current-facing Markdown files;
- derive the project version from `CMakeLists.txt` and require the MCP protocol header, standalone version output, addon plugin manifest, README, capability matrix, changelog, and security policy to agree;
- require the current supported minor line and reject an older line marked supported;
- require the authoritative Phase 3 registration facts (`68` canonical, `10` legacy, `78` total, `50` implemented, `18` unimplemented) in their designated current-reference documents;
- validate relative Markdown file targets and heading anchors across tracked current and historical documentation;
- emit file-specific failures and return a nonzero exit status.

The validator will not parse C++ registration statements to calculate tool counts. The built executable's MCP smoke test remains authoritative for counts; the documentation validator checks that the current reference pages match the release contract locked by that smoke test.

## Verification

The reconciliation is complete when:

1. The new validator passes locally.
2. The existing native suite remains green because workflow changes must not alter runtime behavior.
3. The fast documentation workflow invokes the validator on pushes and pull requests that touch Markdown, the validator, version sources, tool registration, or capability tests.
4. A manual scan finds no current-facing references to an older release as current, Phase 3 as future work, or fixed single-process IPC as the normal session topology.
5. `git diff --check` reports no whitespace errors.

## Non-goals

- Rewriting Phase 1–3 design specs, plans, or review reports to current tense.
- Claiming support for remote or hostile-host attachment.
- Expanding Phase 3 behavior, tool schemas, or supported Godot versions.
- Adding a documentation framework, site generator, or third-party linter.
