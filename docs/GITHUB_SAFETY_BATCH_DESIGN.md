# GitHub Safety-Issue Batch Design

> **Status:** Completed historical design record. Current runtime behavior is documented in [Current Capability Matrix](CAPABILITIES.md), [Tool Reference](TOOL_REFERENCE.md), and the [Changelog](../CHANGELOG.md).

## Context

The post-Phase 4 GitHub backlog contains eight open reports. Repository audit
confirmed that all eight point at current code. The first closure batch resolves
the three highest-priority reports, with code changes only for the two
reproducible bugs:

- #10: audit proves the reported unengaged-lease dereference is unreachable
  under the bound dispatcher invariant, so the issue closes with evidence and
  no speculative code change.
- #12: symbol patching can absorb ordinary `#` comments above a declaration and
  delete file headers or license text.
- #14: successful JSON-RPC responses replace an explicit `null` result with an
  empty object.

Issues #11, #13, #15, #16, and #17 remain explicitly outside this batch and
will stay open for subsequent ordered work.

## Delivery Strategy

Close #10 directly with the invariant and existing-test evidence. Use one pull
request with one test-first commit for #12 and one for #14. This keeps each code
fix independently reviewable and revertible while avoiding redundant
cross-platform CI and merge cycles. The pull request will use `Fixes #12` and
`Fixes #14` so GitHub closes those issues only when the verified changes land
on `main`.

The rejected alternatives are three separate pull requests, which add process
latency without reducing these small fixes' interaction risk, and one pull
request for all eight issues, which would combine unrelated parser, packaging,
indexing, and reflection work into an unsafe review surface.

## Issue #10: Invariant Audit and Evidence Closure

Each `LeaseDispatchClient::Binding` creates a fresh bound state with
`last_error == std::nullopt`. The only assignment to `last_error` occurs inside
`sendRequest` after the condition `state && state->lease.has_value()`. The later
`callTool` branch reads `lastError()` from that same thread-local bound state.
Consequently, a present dispatcher error implies that `lease` is engaged when
`lease->descriptor` is read.

Existing native coverage already exercises non-atomic and descriptorless route
providers and confirms they fail closed without dispatching. Because no failing
case can be constructed without breaking the class invariant itself, adding a
conditional dereference would be an untestable behavior-neutral change. Close
#10 with these exact source and test references and make no production or test
change for the report.

## Issue #12: Symbol-Patch Comment Boundaries

Only GDScript annotations beginning with `@` and documentation comments
beginning with `##` are definition metadata that should move with a patched
symbol. Ordinary single-hash comments are not absorbed, even when directly
adjacent to the declaration. A blank line continues to stop the backward scan
naturally.

Regression coverage will patch the first function in a script containing a
license/header block and confirm the header is byte-for-byte preserved. Existing
coverage will continue to verify replacement of the function body; an
additional case will confirm that directly attached annotations and `##` doc
comments still remain part of the replaceable symbol definition.

## Issue #14: Exact JSON-RPC Success Results

`JsonRpcResponse::toJson` will assign the stored success result without type
coercion. An explicit JSON `null` therefore serializes as `"result": null`,
while objects, arrays, strings, numbers, and booleans remain unchanged. Error
responses continue to omit `result` and serialize their existing error object.

The regression test will construct a successful response with `nullptr`,
assert that `result` exists and is null in both `toJson()` and serialized JSON,
and retain the existing object-response assertions.

## Documentation and Verification

`CHANGELOG.md` will describe the two code fixes under `[Unreleased]` and
reference their affected contracts without claiming a new release. The #10
issue closure comment records its invariant audit separately. No API
registration, tool count, version number, release asset, or capability status
changes.

Issues #12 and #14 each follow a red-green cycle: add the smallest native
regression, observe it fail for the reported reason, implement only the required
fix, run the focused test, and commit. Before the pull request, run a clean
build, the entire native suite, documentation-contract tests, the repository
validator, and `git diff --check`. An independent review must report no Critical
or Important issue. Pull-request and post-merge checks must pass on Windows,
Linux, and macOS with zero annotations before branch, worktree, and pull-request
cleanup is considered complete.
