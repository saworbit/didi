# macOS CI Tap-Warning Cleanup Implementation Plan

> **Status:** Completed historical implementation plan. Commands and repository-state expectations below record the delivery process and are not current setup guidance.

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Remove the unused `aws/tap` from macOS runners before compiler-cache setup so pull-request and `main` CI complete with zero GitHub annotations.

**Architecture:** Keep the change at the workflow boundary: an idempotent macOS-only shell step removes the runner-image tap before `hendrikmuhs/ccache-action` invokes Homebrew. Extend the existing dependency-free workflow validator to enforce that cleanup and ordering, using focused temporary-workflow fixtures as the regression contract.

**Tech Stack:** GitHub Actions YAML, Python 3 standard library, `unittest`, CMake native tests.

## Global Constraints

- Keep compiler caching enabled on macOS.
- Do not trust `aws/tap` and do not set `HOMEBREW_NO_REQUIRE_TAP_TRUST`.
- The cleanup must do nothing when `aws/tap` is absent.
- Do not alter Didi build products, the `v1.4.0` release, package contents, or Homebrew trust policy.
- Completion requires zero annotations on final `main` checks and a clean, synchronized repository with no open pull requests, feature branches, or extra worktrees.

---

## File Structure

- `tests/test_documentation_validator.py`: fixtures for missing, misordered, and correctly ordered cleanup.
- `tools/validate_documentation.py`: dependency-free validation of the cleanup step and its position before ccache.
- `.github/workflows/ci.yml`: idempotent macOS runner cleanup immediately before compiler-cache setup.
- `docs/CI_MACOS_TAP_WARNING_DESIGN.md`: approved design and acceptance criteria.
- `docs/CI_MACOS_TAP_WARNING_IMPLEMENTATION_PLAN.md`: this execution and verification record.

### Task 1: Add the Failing Workflow Contract

**Files:**
- Modify: `tests/test_documentation_validator.py`
- Test: `tests/test_documentation_validator.py`

**Interfaces:**
- Consumes: `VALIDATOR.validate_repository(root: pathlib.Path) -> list[str]`
- Produces: regression expectations for an error containing `aws/tap` and `ccache` when cleanup is absent or follows cache setup, and no error when guarded cleanup precedes it.

- [x] **Step 1: Write the failing tests**

Add `test_requires_macos_aws_tap_cleanup_before_ccache` with two subtests. Each writes a macOS matrix workflow using `hendrikmuhs/ccache-action@v1.2.23`; one omits cleanup and one puts it after ccache. Assert both yield an error containing `aws/tap` and `ccache`.

Add `test_accepts_macos_aws_tap_cleanup_before_ccache`. Its fixture puts this guarded step before ccache and asserts `validate_repository(root) == []`:

```yaml
- name: Remove unused Homebrew tap
  if: runner.os == 'macOS'
  run: |
    if brew tap | grep -qx 'aws/tap'; then
      brew untap aws/tap
    fi
```

- [x] **Step 2: Run the focused tests and observe red**

Run: `python -m unittest tests.test_documentation_validator.DocumentationValidatorTests.test_requires_macos_aws_tap_cleanup_before_ccache tests.test_documentation_validator.DocumentationValidatorTests.test_accepts_macos_aws_tap_cleanup_before_ccache -v`

Expected: the required-cleanup test fails because the validator does not yet emit an `aws/tap`/`ccache` contract error. Record this red state before implementation.

### Task 2: Enforce and Satisfy the Workflow Contract

**Files:**
- Modify: `tools/validate_documentation.py`
- Modify: `.github/workflows/ci.yml`
- Test: `tests/test_documentation_validator.py`

**Interfaces:**
- Consumes: `_workflow_steps(text: str) -> list[str]` and Task 1's fixture expectations.
- Produces: `_has_macos_aws_tap_cleanup(step: str) -> bool` plus a repository error if a macOS ccache step lacks earlier guarded cleanup.

- [x] **Step 1: Implement the minimal validator rule**

Add a helper requiring all three elements in one step: `if: runner.os == 'macOS'`, a `brew tap` presence check, and `brew untap aws/tap`:

```python
def _has_macos_aws_tap_cleanup(step: str) -> bool:
    return bool(
        re.search(r"^\s*if:\s*runner\.os\s*==\s*['\"]macOS['\"]\s*$", step, re.MULTILINE)
        and re.search(r"brew\s+tap\b", step)
        and re.search(r"brew\s+untap\s+['\"]?aws/tap['\"]?\b", step)
    )
```

In `validate_workflow_contract`, when text contains both `macos-latest` and `hendrikmuhs/ccache-action`, find the ccache step index and require an earlier step satisfying the helper. Otherwise append exactly:

```python
f"{relative_path}: remove aws/tap on macOS before hendrikmuhs/ccache-action"
```

- [x] **Step 2: Add the idempotent cleanup to CI**

Insert immediately before `Setup Compiler Cache (ccache / sccache)`:

```yaml
- name: Remove Unused Homebrew Tap (macOS)
  if: runner.os == 'macOS'
  shell: bash
  run: |
    if brew tap | grep -qx 'aws/tap'; then
      brew untap aws/tap
    fi
```

- [x] **Step 3: Run focused and full contract tests**

Run: `python -m unittest tests.test_documentation_validator -v`

Expected: every validator unit test passes.

Run: `python tools/validate_documentation.py`

Expected: `Documentation contract valid` with all Markdown files and version sources aligned.

- [x] **Step 4: Run native and hygiene verification**

Run: `build\Release\didi_tests.exe`

Expected: all 118 native tests pass.

Run: `git diff --check`

Expected: no output and exit status 0.

- [x] **Step 5: Commit the tested implementation**

Stage `.github/workflows/ci.yml`, `tools/validate_documentation.py`, `tests/test_documentation_validator.py`, and this plan, then commit with message `ci: remove unused macOS Homebrew tap`.

### Task 3: Red-Team, Integrate, and Close GitHub State

**Files:**
- Verify: `.github/workflows/ci.yml`
- Verify: `tools/validate_documentation.py`
- Verify: `tests/test_documentation_validator.py`

**Interfaces:**
- Consumes: a clean feature-branch commit and GitHub check-run/annotation APIs.
- Produces: merged `main`, zero annotations, and no residual branch, pull request, worktree, or uncommitted state.

- [x] **Step 1: Red-team the contract locally**

Evaluate representative workflow strings with cleanup missing, cleanup after ccache, cleanup without the macOS guard, cleanup without the tap-presence check, and valid guarded cleanup before ccache. Expected: only the valid guarded, correctly ordered case returns no `aws/tap` contract error. Do not commit temporary fixtures beyond Task 1's permanent regression tests.

- [ ] **Step 2: Push and open the pull request**

Push `codex/remove-macos-tap-warning`, create a pull request targeting `main`, and include the design decision, red/green evidence, native test count, and zero-annotation acceptance criterion in its body.

- [ ] **Step 3: Require green PR checks and zero annotations**

Wait for every pull-request check. Query every check run for the PR head SHA and sum `output.annotations_count`; expected total: `0`. If an annotation remains, inspect its exact check, job, and log, then iterate through the smallest new failing regression contract.

- [ ] **Step 4: Merge and verify `main`**

Merge the pull request, delete the remote feature branch, fetch/prune, switch to `main`, and fast-forward it to `origin/main`. Wait for all post-merge checks and require every check to succeed with a summed annotation count of `0`.

- [ ] **Step 5: Perform the final no-loose-ends audit**

Confirm: `git status --short --branch` is clean and synchronized; local and remote branch listings contain only `main` (apart from the `origin/HEAD` alias); `git worktree list` contains only `D:/didi`; `gh pr list --state open` is empty; `.worktrees` is absent or empty; and `gh release view v1.4.0` still lists all three published assets.
