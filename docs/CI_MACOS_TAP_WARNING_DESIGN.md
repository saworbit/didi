# macOS CI Tap-Warning Cleanup Design

## Context

The `macos-latest` CI job completes successfully but emits a Homebrew tap-trust
annotation. The runner image contains `aws/tap`, and
`hendrikmuhs/ccache-action@v1.2.23` invokes `brew install ccache`. Homebrew checks
all configured taps during that command and warns because `aws/tap` is not
trusted. Didi does not use packages from `aws/tap`.

## Decision

Add a macOS-only workflow step immediately before compiler-cache setup. The step
will test whether `aws/tap` is configured and untap it when present. It will do
nothing when the runner image no longer includes that tap.

This keeps compiler caching enabled on macOS and preserves Homebrew's trust
checks. The workflow will not trust the entire third-party tap and will not set
`HOMEBREW_NO_REQUIRE_TAP_TRUST`.

## Repository Contract

Extend the dependency-free workflow validator so a macOS matrix using
`hendrikmuhs/ccache-action` must remove `aws/tap` before the cache action. A
fixture with the cleanup missing or ordered after cache setup must fail. A
fixture with the cleanup before cache setup must pass.

## Verification

The change is complete only when:

1. The new regression test is observed failing before the workflow change and
   passing afterward.
2. All repository-contract and native tests pass locally.
3. Pull-request and post-merge CI pass on Windows, Linux, and macOS.
4. The final `main` check runs contain zero GitHub annotations.
5. `main` matches `origin/main`, with no open pull requests, feature branches,
   extra worktrees, or uncommitted files.

## Scope

This change only removes the unused runner-image tap before cache setup. It does
not alter Didi's build products, release tag, package contents, or Homebrew trust
policy.
