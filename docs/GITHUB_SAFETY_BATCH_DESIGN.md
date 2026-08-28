# GitHub Safety-Issue Batch Design

## Context

The post-Phase 4 GitHub backlog contains eight open reports. Repository audit
confirmed that all eight describe current code, rather than stale reports. The
first closure batch addresses the three highest-risk, independently testable
bugs:

- #10: an unengaged runtime-route lease can be dereferenced while converting a
  dispatch failure into the structured live-error envelope.
- #12: symbol patching can absorb ordinary `#` comments above a declaration and
  delete file headers or license text.
- #14: successful JSON-RPC responses replace an explicit `null` result with an
  empty object.

Issues #11, #13, #15, #16, and #17 remain explicitly outside this batch and
will stay open for subsequent ordered work.

## Delivery Strategy

Use one pull request with one test-first commit per issue. This keeps each fix
independently reviewable and revertible while avoiding three redundant
cross-platform CI and merge cycles. The pull request will use `Fixes #10`,
`Fixes #12`, and `Fixes #14` so GitHub closes the issues only when the verified
changes land on `main`.

The rejected alternatives are three separate pull requests, which add process
latency without reducing these small fixes' interaction risk, and one pull
request for all eight issues, which would combine unrelated parser, packaging,
indexing, and reflection work into an unsafe review surface.

## Issue #10: Optional Runtime-Route Provenance

`ToolRegistry::callTool` already treats the route lease as optional in its
policy and attribution paths. The dispatcher-error branch must follow the same
contract. When `LeaseDispatchClient::lastError()` is present, the structured
error receives `lease->descriptor` only when the outer lease is engaged;
otherwise it receives `std::nullopt`.

The regression test will exercise a live-capable dispatch path that produces a
recorded dispatcher error without an authenticated route lease. It must return
a structured error with a JSON `null` session and must not crash or invoke
undefined behavior. Existing structured-session provenance remains unchanged
when a descriptor is available.

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

`CHANGELOG.md` will describe all three fixes under `[Unreleased]` and reference
their affected contracts without claiming a new release. No API registration,
tool count, version number, release asset, or capability status changes.

Each issue follows a red-green cycle: add the smallest native regression,
observe it fail for the reported reason, implement only the required fix, run
the focused test, and commit. Before the pull request, run a clean build, the
entire native suite, documentation-contract tests, the repository validator,
and `git diff --check`. An independent review must report no Critical or
Important issue. Pull-request and post-merge checks must pass on Windows,
Linux, and macOS with zero annotations before branch, worktree, and pull-request
cleanup is considered complete.
