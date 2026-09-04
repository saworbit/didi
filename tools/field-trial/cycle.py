"""Run one fix cycle: take a reproducible issue, let an agent fix it, prove it, open a draft.

The loop never merges and never closes anything. Its output is a draft pull
request plus the evidence for it, and a human decides.

Cheap deterministic checks do the gating here. The expensive field trial is a
separate loop on its own cadence, because one stochastic run cannot gate a change:
that is what pass@k exists to say, and it would put the most expensive component
where it helps least.
"""

from __future__ import annotations

import argparse
import json
import subprocess
import uuid
from datetime import datetime, timezone
from pathlib import Path

import importlib.util

HERE = Path(__file__).resolve().parent
REPOSITORY = HERE.parents[1]


def _load(name: str):
    spec = importlib.util.spec_from_file_location(name, HERE / f"{name}.py")
    if spec is None or spec.loader is None:
        raise ImportError(f"Cannot load {name} from {HERE}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


gates = _load("gates")
runner = _load("runner")

FIX_BRIEF = """You are fixing one reported defect in the Didi MCP server at {repository}.

Issue #{number}: {title}

{body}
{discussion}
Rules for this task:
1. Find the root cause before changing anything. Read the code paths the issue names.
2. Add a check that FAILS on the current build and PASSES once fixed. A check that
   already passes proves nothing. Native tests go in tests/, live editor behaviour
   goes in tests/run_godot_integration.ps1.
3. You may add test assertions. You must not modify or delete an existing one.
4. Change only src/, include/, tests/ and docs/. Never .github/workflows. Never delete a file.
5. Do not commit, branch, push, or open a pull request. Leave your work in the tree.
6. If the issue cannot be fixed through a supported API, stop and say so plainly
   rather than working around it. Reporting the limitation honestly is a valid outcome.

Build with: {build_command}
"""


def run(command: list[str], cwd: Path, check: bool = False) -> subprocess.CompletedProcess:
    return subprocess.run(
        command, cwd=str(cwd), capture_output=True, text=True,
        encoding="utf-8", errors="replace", check=check,
    )


def cycle_summary(
    cycle_id: str,
    issue_number: int | None,
    phases: list[dict],
    outcome: str,
    pull_request: str | None = None,
) -> dict:
    """The machine-readable record of one cycle.

    Shaped so cycles compare: the point of a loop is the trend across it, and a
    summary that only a human can read gives you anecdotes.
    """
    return {
        "cycle_id": cycle_id,
        "issue": issue_number,
        "outcome": outcome,
        "pull_request": pull_request,
        "phases": phases,
        "failed_phase": next((p["name"] for p in phases if p["status"] == "failed"), None),
        "finished_utc": datetime.now(timezone.utc).isoformat(),
    }


def out_of_policy_paths(status_porcelain: str, allowed=gates.ALLOWED_PREFIXES) -> list[str]:
    """Changed paths a fix is not allowed to touch, from git status.

    The diff and the commit have to describe the same change. Checking `git
    diff` and committing with `git add -A` does not: the first ignores untracked
    files and the second sweeps them in, so the first cycle shipped ten build
    artifacts through a policy that never saw them.
    """
    offenders = []
    for line in status_porcelain.splitlines():
        path = line[3:].strip().strip('"')
        if not path or path.startswith("build-ninja/"):
            continue
        if not path.startswith(tuple(allowed)):
            offenders.append(path)
    return offenders


def render_discussion(comments: list[dict] | None, limit: int = 10) -> str:
    """The issue's comments, for the brief.

    A report's first draft is often wrong and the correction arrives underneath
    it. #216 was filed as "float properties reject whole numbers"; the transcript
    later showed the client had sent the string "1.0" and the rejection was
    right. An agent handed the body alone inherits the mistaken premise with none
    of the evidence that overturned it, and sets out to fix a bug that is not
    there.

    Kept oldest-first and capped, because the brief is the agent's whole view of
    the problem and a long thread would crowd out the report itself.
    """
    if not comments:
        return ""
    lines = ["", "Discussion on the issue, oldest first. Later comments may correct the report:"]
    for comment in comments[-limit:]:
        author = (comment.get("author") or {}).get("login") or "unknown"
        body = (comment.get("body") or "").strip()
        if not body:
            continue
        lines.append(f"\n--- comment by {author} ---\n{body}")
    return "\n".join(lines) + "\n" if len(lines) > 2 else ""


def changed_paths(patch_text: str) -> set[str]:
    """Every path a patch touches, from its +++ lines."""
    return {
        line[6:].strip() for line in patch_text.splitlines()
        if line.startswith("+++ b/")
    }


def staged_patch_mismatch(approved: str, staged: str) -> str:
    """Why the index no longer matches what the gates graded, in words.

    Names the paths that came and went rather than printing two patches, because
    the whole failure is that a file quietly stopped being part of the change.
    """
    lost = sorted(changed_paths(approved) - changed_paths(staged))
    gained = sorted(changed_paths(staged) - changed_paths(approved))
    parts = []
    if lost:
        parts.append(f"missing from the commit: {', '.join(lost)}")
    if gained:
        parts.append(f"not in the graded patch: {', '.join(gained)}")
    if not parts:
        parts.append("the staged patch differs from the graded one")
    return "; ".join(parts)


def authentication_problem(status_stdout: str) -> str | None:
    """Why the client cannot run an agent, or None when it can.

    Checked before a cycle spends anything. The loop shells out to a separate
    client that reads its own credential store, so this session being signed in
    says nothing about whether the loop can run: the first live cycle built a
    worktree and briefed an agent before discovering the stored login had
    expired days earlier.
    """
    try:
        status = json.loads(status_stdout or "{}")
    except json.JSONDecodeError:
        return "could not read authentication status from the client"
    if not isinstance(status, dict):
        return "could not read authentication status from the client"
    if status.get("loggedIn") is True:
        return None
    return (
        f"the client is not signed in (authMethod={status.get('authMethod', 'unknown')}). "
        "Run `claude auth login` in a terminal and complete it in the browser, "
        "then run this cycle again."
    )


def agent_failure_detail(stdout: str, stderr: str) -> str:
    """Why the agent run failed, in words.

    The client reports failures as JSON on stdout with an empty stderr, so a
    cycle that reports stderr alone says a run failed and nothing else. The
    first authentication expiry surfaced as a blank cell in the summary table.
    """
    try:
        payload = json.loads(stdout.strip().splitlines()[-1])
    except (json.JSONDecodeError, IndexError):
        return (stderr or stdout or "no output").strip()[-300:]
    if isinstance(payload, dict) and payload.get("result"):
        turns = payload.get("num_turns", "?")
        cost = payload.get("total_cost_usd", 0)
        return f"{payload['result']} (turns={turns}, cost=${cost})"
    return (stderr or stdout or "no output").strip()[-300:]


def render_summary(summary: dict) -> str:
    lines = [
        f"# Cycle {summary['cycle_id']}",
        "",
        f"Issue: {summary['issue']}",
        f"Outcome: {summary['outcome']}",
        f"Pull request: {summary['pull_request'] or 'none'}",
        "",
        "| Phase | Status | Detail |",
        "| :--- | :--- | :--- |",
    ]
    for phase in summary["phases"]:
        detail = (phase.get("detail") or "").replace("|", "\\|")[:160]
        lines.append(f"| {phase['name']} | {phase['status']} | {detail} |")
    return "\n".join(lines) + "\n"


def select_from_tracker(repo: str, issue_number: int | None) -> dict | None:
    if issue_number is not None:
        result = run(["gh", "issue", "view", str(issue_number), "--repo", repo,
                      "--json", "number,title,body,createdAt,labels,comments,closedByPullRequestsReferences"], REPOSITORY)
        return json.loads(result.stdout) if result.returncode == 0 else None
    result = run(["gh", "issue", "list", "--repo", repo, "--state", "open",
                  "--label", gates.AGENT_READY_LABEL, "--limit", "50",
                  "--json", "number,title,body,createdAt,labels,comments,closedByPullRequestsReferences"], REPOSITORY)
    if result.returncode != 0:
        return None
    return gates.select_issue(json.loads(result.stdout or "[]"))


DEFAULT_VSDEVCMD = (
    r"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools"
    r"\Common7\Tools\VsDevCmd.bat"
)


def build_batch(worktree: Path, vsdevcmd: str = DEFAULT_VSDEVCMD) -> str:
    """A build script that compiles the worktree, not the checkout beside it.

    This has to configure its own build directory under the worktree. Reusing the
    main tree's `build-ninja` compiles the main tree's sources, so the agent's
    change is never built and both the red run and the green run grade code
    nobody edited. A cycle that does that verifies nothing while looking green.
    """
    tree = str(worktree)
    return (
        "@echo off\r\n"
        f'call "{vsdevcmd}" -no_logo -arch=x64 -host_arch=x64\r\n'
        f'cmake -S "{tree}" -B "{tree}\\build-ninja" -G Ninja '
        "-DCMAKE_BUILD_TYPE=Release || exit /b 1\r\n"
        f'cmake --build "{tree}\\build-ninja" --parallel || exit /b 1\r\n'
    )


def verify(worktree: Path, godot: str | None, batch: Path) -> tuple[bool, str]:
    """Build the worktree and run every suite. True only if all of them pass.

    The build script lives in the cycle's artifacts rather than in the worktree,
    so verification never shows up in the patch it is judging. The agent is given
    the same script, so what it checks its work with is what grades it.
    """
    built = run(["cmd", "/c", str(batch)], worktree)
    if built.returncode != 0:
        return False, f"build failed: {(built.stdout or built.stderr or '')[-400:]}"

    build_dir = worktree / "build-ninja"
    steps = [
        ([str(build_dir / "didi_tests.exe")], "native tests"),
        (["python", "-m", "unittest", "discover", "-s", "tests", "-t", "tests",
          "-p", "test_*.py"], "python tests"),
        (["python", "tools/validate_documentation.py"], "documentation"),
    ]
    if godot:
        steps.append((["powershell", "-File", "tests/run_godot_integration.ps1",
                       "-GodotExecutable", godot,
                       "-McpExecutable", str(build_dir / "didi.exe")],
                      "live harness"))
    for command, name in steps:
        result = run(command, worktree)
        if result.returncode != 0:
            return False, f"{name} failed: {(result.stderr or result.stdout or '')[-300:]}"
    return True, "all suites passed"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", default="saworbit/didi")
    parser.add_argument("--issue", type=int, help="Work this issue instead of the oldest ready one")
    parser.add_argument("--budget-usd", type=float, default=10.0)
    parser.add_argument("--timeout-seconds", type=int, default=3600)
    parser.add_argument("--godot", help="Godot executable for the live harness")
    parser.add_argument("--artifacts", type=Path, default=Path("D:/didi-trials"))
    parser.add_argument("--dry-run", action="store_true",
                        help="Exercise the orchestration without launching an agent")
    args = parser.parse_args(argv)

    cycle_id = datetime.now(timezone.utc).strftime("cycle-%Y%m%d-%H%M%S")
    output = args.artifacts / cycle_id
    output.mkdir(parents=True, exist_ok=True)
    phases: list[dict] = []
    issue_number: int | None = None
    pull_request: str | None = None

    def record(name: str, status: str, detail: str = "") -> None:
        phases.append({"name": name, "status": status, "detail": detail})

    def discard_worktree() -> None:
        """Remove a worktree that holds nothing worth reading.

        An unattended loop that leaves debris behind stops being unattended. A
        worktree is only kept when there is a patch in it to inspect.
        """
        run(["git", "worktree", "remove", "--force", str(worktree)], REPOSITORY)
        run(["git", "branch", "-D", branch], REPOSITORY)
        run(["git", "worktree", "prune"], REPOSITORY)

    def finish(outcome: str) -> int:
        summary = cycle_summary(cycle_id, issue_number, phases, outcome, pull_request)
        (output / "cycle.json").write_text(json.dumps(summary, indent=2), encoding="utf-8")
        (output / "CYCLE.md").write_text(render_summary(summary), encoding="utf-8")
        print(render_summary(summary))
        print(f"Artifacts: {output}")
        return 0 if outcome == "pull_request_opened" else 1

    dirty = run(["git", "status", "--porcelain"], REPOSITORY).stdout
    blocking = [l for l in dirty.splitlines() if l and "build-ninja" not in l]
    if blocking and not args.dry_run:
        record("preflight", "failed", f"working tree not clean: {blocking[:3]}")
        return finish("preflight_failed")
    if not args.dry_run:
        status = run([runner.resolve_executable(), "auth", "status"], REPOSITORY)
        problem = authentication_problem(status.stdout)
        if problem:
            record("preflight", "failed", problem)
            return finish("not_authenticated")
    record("preflight", "ok", "tree clean, client signed in")

    issue = select_from_tracker(args.repo, args.issue) if not args.dry_run else {
        "number": 0, "title": "dry run", "body": "dry run", "createdAt": "", "labels": []}
    if not issue:
        record("select", "skipped", f"no open issue labelled {gates.AGENT_READY_LABEL}")
        return finish("nothing_to_do")
    issue_number = issue["number"]
    record("select", "ok", f"#{issue_number} {issue['title'][:80]}")

    branch = f"fix/agent-{issue_number}-{cycle_id[-6:]}"
    worktree = args.artifacts / f"{cycle_id}-worktree"
    if args.dry_run:
        record("isolate", "skipped", "dry run")
        record("fix", "skipped", "dry run")
        record("gate_red_green", "skipped", "dry run")
        record("gate_diff_policy", "skipped", "dry run")
        record("report", "skipped", "dry run")
        return finish("dry_run")

    created = run(["git", "worktree", "add", "-b", branch, str(worktree), "main"], REPOSITORY)
    if created.returncode != 0:
        record("isolate", "failed", created.stderr[-300:])
        return finish("isolate_failed")
    record("isolate", "ok", str(worktree))

    build_script = output / "build.bat"
    build_script.write_text(build_batch(worktree), encoding="utf-8")

    session_id = str(uuid.uuid4())
    prompt = FIX_BRIEF.format(
        repository=worktree, number=issue_number, title=issue["title"], body=issue["body"],
        discussion=render_discussion(issue.get("comments")),
        build_command=f'cmd /c "{build_script}"',
    )
    command = runner.build_command(
        session_id=session_id, budget_usd=args.budget_usd,
        add_dirs=[str(worktree)],
        allowed_tools=["Bash", "Read", "Edit", "Write", "Glob", "Grep"],
    )
    try:
        completed = runner.run_agent(command, prompt, worktree, args.timeout_seconds,
                                     output / "fix" / "agent.log")
    except subprocess.TimeoutExpired:
        record("fix", "failed", f"agent exceeded {args.timeout_seconds}s")
        return finish("fix_timeout")  # kept: a timed-out agent may have left work
    except OSError as error:
        # A cycle that cannot start its agent must say so, not raise through the
        # orchestration and leave a worktree behind with an exit code of zero.
        record("fix", "failed", f"could not launch the agent: {error}")
        discard_worktree()
        return finish("agent_unavailable")
    if completed.returncode != 0:
        record("fix", "failed", agent_failure_detail(completed.stdout or "", completed.stderr or ""))
        discard_worktree()
        return finish("fix_failed")

    # Stage exactly what a fix may change, then read the patch back from the
    # index. What the policy inspects and what the commit carries are then the
    # same bytes by construction rather than by coincidence.
    run(["git", "add", "-A", "--", *gates.ALLOWED_PREFIXES], worktree)
    patch = run(["git", "diff", "--cached"], worktree).stdout
    stray = out_of_policy_paths(run(["git", "status", "--porcelain"], worktree).stdout)
    (output / "fix").mkdir(parents=True, exist_ok=True)
    (output / "fix" / "patch.diff").write_text(patch, encoding="utf-8")
    if not patch.strip():
        record("fix", "failed", "agent produced no changes")
        discard_worktree()
        return finish("no_changes")
    record("fix", "ok", f"{len(patch.splitlines())} diff lines")

    violations = gates.diff_policy_violations(patch) + [
        f"{path}: changed outside the paths a fix may touch" for path in stray
    ]
    if violations:
        record("gate_diff_policy", "failed", "; ".join(violations)[:400])
        return finish("diff_policy_violated")
    record("gate_diff_policy", "ok", "patch stays inside the allowed surface")

    # Red: the reproduction alone, without the fix, must fail.
    run(["git", "stash", "push", "--", "src", "include", "docs"], worktree)
    red_passed, red_detail = verify(worktree, args.godot, build_script)
    run(["git", "stash", "pop"], worktree)
    # Green: the whole patch must pass.
    green_passed, green_detail = verify(worktree, args.godot, build_script)

    # Keep the red run's output even when the gate passes. "Failed without the
    # fix" does not say which assertion failed, and a patch can carry one true
    # reproduction alongside assertions that already passed: the gate is then
    # satisfied by whichever one was red, which need not be the one the issue
    # describes. Reading that back afterwards took a code audit, because the
    # only record of it was a sentence saying it happened.
    (output / "gate" ).mkdir(parents=True, exist_ok=True)
    (output / "gate" / "red.txt").write_text(red_detail, encoding="utf-8")
    (output / "gate" / "green.txt").write_text(green_detail, encoding="utf-8")

    verdict = gates.evaluate_red_green(pre_fix_failed=not red_passed, post_fix_passed=green_passed)
    if verdict != "ok":
        detail = red_detail if verdict == "not_red" else green_detail
        record("gate_red_green", "failed", f"{verdict}: {detail}")
        return finish(verdict)
    record("gate_red_green", "ok", f"failed without the fix: {red_detail}")

    # Re-stage, then refuse to commit anything but the patch that was graded.
    # `git stash pop` restores the working tree and leaves the index alone, so
    # the source half of a fix comes back unstaged and a plain commit ships the
    # tests without it: a pull request whose new test has nothing to pass
    # against. Staging early was not enough. The index has to be checked against
    # the approved bytes at the moment of the commit, which is the only moment
    # that matters.
    run(["git", "add", "-A", "--", *gates.ALLOWED_PREFIXES], worktree)
    staged = run(["git", "diff", "--cached"], worktree).stdout
    if staged != patch:
        record("report", "failed", staged_patch_mismatch(patch, staged))
        return finish("staged_patch_mismatch")
    run(["git", "commit", "-m", f"Fix #{issue_number}"], worktree)
    pushed = run(["git", "push", "-u", "origin", branch], worktree)
    if pushed.returncode != 0:
        record("report", "failed", pushed.stderr[-300:])
        return finish("push_failed")
    opened = run(["gh", "pr", "create", "--repo", args.repo, "--draft", "--base", "main",
                  "--head", branch, "--title", f"Fix #{issue_number}: {issue['title'][:70]}",
                  "--body", f"Closes #{issue_number}\n\nOpened by a fix cycle. "
                            f"Reproduction failed without the change and passed with it. "
                            f"Evidence in `{output}`."], worktree)
    if opened.returncode != 0:
        record("report", "failed", opened.stderr[-300:])
        return finish("pr_failed")
    pull_request = opened.stdout.strip()
    record("report", "ok", pull_request)
    return finish("pull_request_opened")


if __name__ == "__main__":
    raise SystemExit(main())
