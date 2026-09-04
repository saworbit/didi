"""The checks that decide whether a fix cycle is allowed to produce a pull request.

Pure functions over data, deliberately. Every one of these answers a question the
fixing agent must not be trusted to answer about itself, so they take paths,
diffs and exit statuses rather than the agent's account of what it did.
"""

from __future__ import annotations

from typing import Iterable, Sequence

# Where a fix may write. Everything else is either generated, someone else's
# concern, or a way to change the rules the cycle is judged by.
ALLOWED_PREFIXES = ("src/", "include/", "tests/", "docs/")

# Substrings that mean a line is doing the actual checking. Removing one is how a
# green suite gets bought rather than earned, so a patch may add these and never
# take one away.
ASSERTION_MARKERS = (
    "assert",
    "Assert-True",
    "EXPECT_",
    "ASSERT_",
    "self.assert",
)

AGENT_READY_LABEL = "agent-ready"


def select_issue(issues: Iterable[dict]) -> dict | None:
    """The oldest open issue a human has marked as carrying a reproduction.

    The label is an input control, not a priority: a task with a failing case
    attached resolves cleanly, and one without it turns into debt. Oldest first
    so the queue drains in order and nothing starves.

    An issue whose fix is already sitting in an open pull request is skipped
    however old it is. The label survives the cycle that acted on it, because
    the loop deliberately never closes anything, so the queue's oldest entry is
    routinely one that is already done: #214 kept its label while #222 waited to
    merge, and the next cycle would have spent a full agent run rebuilding a fix
    that already existed.
    """
    ready = [
        candidate
        for candidate in issues
        if any(
            label.get("name") == AGENT_READY_LABEL
            for label in candidate.get("labels", [])
        )
        and not candidate.get("closedByPullRequestsReferences")
    ]
    if not ready:
        return None
    return min(ready, key=lambda candidate: (candidate.get("createdAt", ""), candidate.get("number", 0)))


def _target_path(diff_line: str) -> str | None:
    _, separator, remainder = diff_line.partition(" b/")
    if not separator:
        return None
    return remainder.strip() or None


def diff_policy_violations(
    diff_text: str, allowed_prefixes: Sequence[str] = ALLOWED_PREFIXES
) -> list[str]:
    """Reasons this patch must not become a pull request, one per offending file.

    Coding agents reliably find the cheap way to a green result: editing the
    suite, hardcoding the expected value, replacing an assertion with something
    that always passes. None of that is prevented by asking nicely, so the
    opportunity is removed instead. Adding a test is always allowed; weakening
    one never is.
    """
    violations: list[str] = []
    seen: set[tuple[str, str]] = set()
    current: str | None = None

    def record(path: str, kind: str, message: str) -> None:
        if (path, kind) in seen:
            return
        seen.add((path, kind))
        violations.append(message)

    for line in diff_text.splitlines():
        if line.startswith("diff --git "):
            current = _target_path(line)
            if current is None:
                continue
            if current.startswith(".github/workflows/"):
                record(current, "workflow",
                       f"{current}: a fix must not change the CI workflows that judge it")
            elif not current.startswith(tuple(allowed_prefixes)):
                record(current, "outside",
                       f"{current}: outside the paths a fix may change "
                       f"({', '.join(allowed_prefixes)})")
        elif line.startswith("deleted file mode") and current:
            record(current, "delete", f"{current}: a fix must not delete files")
        elif line.startswith("-") and not line.startswith("---") and current:
            if current.startswith("tests/") and any(
                marker in line for marker in ASSERTION_MARKERS
            ):
                record(current, "assertion",
                       f"{current}: removes an existing test assertion. A fix may add "
                       f"checks and may not weaken one")

    return violations


def evaluate_red_green(pre_fix_failed: bool, post_fix_passed: bool) -> str:
    """Whether the reproduction earned its place, run by the pipeline, not the agent.

    A check that passed before the fix proves nothing, however green it looks
    afterwards. That is not hypothetical: the first regression test written for
    #213 passed before the fix, because it used a Control whose parent was not a
    Control and so never reproduced the discard at all.
    """
    if not pre_fix_failed:
        return "not_red"
    if not post_fix_passed:
        return "not_green"
    return "ok"
