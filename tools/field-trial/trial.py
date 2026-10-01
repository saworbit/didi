"""Run one field trial unattended: seed, brief a fresh tester, score what it did.

Three trials have now been run by hand, and every one of them produced defects
the suites could not. What made them expensive was not the agent's hour, it was
the maintainer's: seeding by hand, remembering which manifest to score against,
and, in trial 03, spending an hour on a stale bridge that no artifact recorded.

This is that loop with the hand-work taken out. It is deliberately not the fix
cycle in `cycle.py`: that one is cheap, deterministic and gates a patch, while
this one is stochastic and expensive and gates nothing. A single trial is not a
verdict on a change. It is a source of findings, and the value is in the diff
between two runs of the same seed.

What it does not do is decide anything. No fix is applied, no issue is closed,
and the run's own account of itself is never trusted for coverage: that comes
from the transcript, and which build served it comes from the transcript too.
"""

from __future__ import annotations

import argparse
import importlib.util
import json
import os
import re
import shutil
import subprocess
import tempfile
import uuid
from datetime import datetime, timezone
from pathlib import Path
from typing import Mapping

HERE = Path(__file__).resolve().parent
REPOSITORY = HERE.parents[1]


def _load(name: str):
    spec = importlib.util.spec_from_file_location(name, HERE / f"{name}.py")
    if spec is None or spec.loader is None:
        raise ImportError(f"Cannot load {name} from {HERE}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


bridge = _load("bridge")
coverage = _load("coverage")
runner = _load("runner")
seed_trial = _load("seed_trial")


def coverage_delta(previous: dict | None, current: dict) -> dict:
    """What moved between two runs of the same seed.

    The absolute coverage figure is close to meaningless on its own and gets
    quoted anyway: trial 03 read as a regression at 32.1% against trial 01's
    39.6% purely because the implemented surface grew from 91 to 112 underneath
    it. So the denominators travel with the number here, and the sets that
    actually matter are the ones that changed hands: a tool nobody had reached
    for before, and a tool that stopped being reached for.
    """
    current_totals = current.get("totals", {})
    if previous is None:
        return {
            "previous": None,
            "current": current_totals,
            "newly_called": [],
            "no_longer_called": [],
        }
    previous_called = set(previous.get("called", {}))
    current_called = set(current.get("called", {}))
    return {
        "previous": previous.get("totals", {}),
        "current": current_totals,
        "newly_called": sorted(current_called - previous_called),
        "no_longer_called": sorted(previous_called - current_called),
    }


# The Claude tester's whole environment, pinned so one run can be compared with
# the next (#1007). ToolSearch stays, because Didi's tools arrive deferred and
# the tester loads them through it; without it the server under test is out of
# reach. Only the project's own settings load, so the host's hooks and plugins
# do not, and skills are off. Measured with Claude Code 2.1.220: a tester
# launched this way can call exactly these seven tools, and the Didi tools
# through ToolSearch.
CLAUDE_TESTER_ENVIRONMENT = {
    "tools": ["Bash", "Read", "Edit", "Write", "Glob", "Grep", "ToolSearch"],
    "skills": "disabled",
    "setting_sources": "project",
    "mcp_servers": "strict",
}


def keep_transcript(target: Path, session_id: str, projects_root: Path | None = None) -> Path:
    """The Claude tester's transcript, copied into the trial directory.

    The client files it under its own projects store, and a trial scored there
    kept only the numbers: trial 05's transcript is gone, and nothing left in its
    directory can reproduce its coverage or its bridge verdict (#1005). The copy
    is what gets scored, the same name trial 03's hand-made copy used, so every
    trial directory holds its own evidence whichever client hosted it.
    """
    filed = runner.transcript_path(target, session_id, projects_root)
    kept = target / "transcript.jsonl"
    if filed.is_file():
        shutil.copyfile(filed, kept)
    return kept


DRAFTS_FILE = "ISSUE_DRAFTS.md"

# The brief's filing section as a drafts run hands it over. Everything else in
# the brief stays as it is, because the task and the ledger are what two runs
# are compared on. Trial 06 made this edit by hand, and a tester whose report
# blamed the server for what its own client did showed why a finding should be
# read before it is public (#1000, #1008).
DRAFTS_SECTION = """## Issue drafts

Do not file issues in this run. `gh` is not available to you, and nothing is to be written to GitHub in any way. Write each report as a draft in `ISSUE_DRAFTS.md` in your working directory instead. The drafts are read before anything is filed. Drafting is deliberately expensive. Before you write one:

1. Re-read the relevant part of Didi's `docs` folder. Behaviour that is documented and wrong is still worth reporting, under a different label.
2. Reduce it to a minimal reproduction: exact tool name, exact arguments, exact response.
3. Choose a label. `bug` when behaviour contradicts the documentation. `documentation` when the documentation is wrong or missing. `enhancement` when the capability is simply absent.

Start each draft with a `## ` heading that holds its title. Under it give its label and the fields the bug report template asks for: Didi version, Godot version, operating system, reproduction, expected, actual.

One draft per root cause, never one per occurrence. Stop after twenty. Later findings go in the ledger with a note that the cap was reached.

Write in first person and in plain sentences. No em dashes, no emoji. Do not describe the report as generated, and do not name a model, an assistant, an agent, or a tool as its author.

Do not commit, branch, push, or open a pull request anywhere.
"""

# The other two places the brief assumes an issue has a number.
DRAFTS_REWORDINGS = (
    ("Issue:     issue number, or none and why", "Issue:     draft title, or none and why"),
    ("issues filed, and the one change", "issue drafts written, and the one change"),
)

# What gh reads a credential from besides its own store.
DRAFTS_TOKEN_VARIABLES = ("GH_TOKEN", "GITHUB_TOKEN", "GH_ENTERPRISE_TOKEN", "GITHUB_ENTERPRISE_TOKEN")

GH_STUB_MESSAGE = "gh is disabled for this field trial. Write the finding to ISSUE_DRAFTS.md instead."


# What the brief names that only a run knows. A run from another checkout, or
# seeded with another Godot, was told to read D:\didi and launch 4.7.2, which
# it had not been given (#1105).
BRIEF_PLACEHOLDERS = ("{repository}", "{godot_exe}")


def filled_brief(brief: str, repository: Path, godot_exe: Path) -> str:
    """The brief naming the checkout the tester is handed and the Godot it was seeded with.

    Refuses rather than handing over a path the tester was not given: a brief
    that no longer names one of them, or one left with a placeholder this does
    not fill.
    """
    for placeholder in BRIEF_PLACEHOLDERS:
        if placeholder not in brief:
            raise ValueError(f"TRIAL_BRIEF.md no longer says {placeholder}, "
                             "so the tester would not be told where it is")
    filled = brief.replace("{repository}", str(repository)).replace("{godot_exe}", str(godot_exe))
    leftover = sorted(set(re.findall(r"\{[a-z_]+\}", filled)))
    if leftover:
        raise ValueError(f"TRIAL_BRIEF.md has placeholders nothing fills: {', '.join(leftover)}")
    return filled


def drafts_brief(brief: str) -> str:
    """The brief with its filing section swapped for the drafts one.

    Refuses rather than guessing when the brief no longer has the text it
    expects. A brief that still says to file with gh, handed to a run that was
    asked to hold its findings, would file them live.
    """
    start = brief.find("## Filing issues\n")
    end = brief.find("\n## ", start + 1)
    if start < 0 or end < 0:
        raise ValueError("TRIAL_BRIEF.md has no '## Filing issues' section followed by another, "
                         "so a drafts run cannot replace it")
    swapped = brief[:start] + DRAFTS_SECTION + brief[end:]
    for old, new in DRAFTS_REWORDINGS:
        if swapped.count(old) != 1:
            raise ValueError(f"TRIAL_BRIEF.md no longer says {old!r} exactly once, "
                             "so a drafts run cannot reword it")
        swapped = swapped.replace(old, new)
    return swapped


def write_gh_stub(directory: Path) -> Path:
    """A gh that refuses, for the front of the tester's PATH.

    Written for both shells a tester reaches for: a shell script for Bash and a
    batch file for cmd and PowerShell. It is a guard, not a sandbox. The real gh
    is still further down the PATH and reads its own credential store, which is
    why the report phase still reads the tracker after a drafts run.
    """
    directory.mkdir(parents=True, exist_ok=True)
    script = directory / "gh"
    script.write_text(f'#!/bin/sh\necho "{GH_STUB_MESSAGE}" >&2\nexit 1\n',
                      encoding="utf-8", newline="\n")
    script.chmod(0o755)
    for name in ("gh.cmd", "gh.bat"):
        (directory / name).write_text(f"@echo {GH_STUB_MESSAGE} 1>&2\r\n@exit /b 1\r\n",
                                      encoding="utf-8", newline="")
    return directory


def drafts_environment(base: Mapping[str, str], stub_directory: Path) -> dict[str, str]:
    """The tester's environment for a drafts run: no token, and the stub first."""
    environment = {key: value for key, value in base.items()
                   if key.upper() not in DRAFTS_TOKEN_VARIABLES}
    path_key = next((key for key in environment if key.upper() == "PATH"), "PATH")
    environment[path_key] = str(stub_directory) + os.pathsep + environment.get(path_key, "")
    return environment


def read_drafts(path: Path) -> list[str]:
    """The drafts' titles, one per `## ` heading."""
    if not path.is_file():
        return []
    lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    return [line[3:].strip() for line in lines if line.startswith("## ")]


def trial_summary(
    trial_id: str,
    phases: list[dict],
    outcome: str,
    baseline: dict | None = None,
    delta: dict | None = None,
    bridge_report: dict | None = None,
    issues: list[dict] | None = None,
    engine: str = runner.CLAUDE,
    model: str | None = None,
    tester_environment: dict | None = None,
    drafts: list[str] | None = None,
) -> dict:
    return {
        "trial_id": trial_id,
        "outcome": outcome,
        # Which client hosted the tester, recorded beside the result rather than
        # in the baseline, because the seed is the same either way. That is the
        # point: an engine is a variable of the run, not of the apparatus, and
        # the first thing anyone comparing two runs needs to know.
        "engine": engine,
        "model": model,
        # What the tester could use, so a change to it shows up between runs
        # rather than in a transcript someone has to read (#1007).
        "tester_environment": tester_environment,
        "baseline": baseline,
        "coverage": delta,
        "bridge": bridge_report,
        "issues_filed": issues or [],
        # A drafts run holds its findings for review, so what it wrote is listed
        # where the filed issues go on a live run (#1008).
        "filing": "live" if drafts is None else "drafts",
        "issue_drafts": drafts or [],
        "phases": phases,
        "failed_phase": next((p["name"] for p in phases if p["status"] == "failed"), None),
        "finished_utc": datetime.now(timezone.utc).isoformat(),
    }


def render_summary(summary: dict) -> str:
    """The page a reviewer reads first.

    The bridge verdict goes above the coverage table on purpose. Coverage from a
    run whose live half was served by an unknown build is a number about
    something nobody has identified, and putting it first invites reading it as
    if it were not.
    """
    tester = summary.get("engine") or runner.CLAUDE
    if summary.get("model"):
        tester += f" ({summary['model']})"
    lines = [
        f"# Field trial {summary['trial_id']}",
        "",
        f"Outcome: {summary['outcome']}",
        f"Tester: {tester}",
    ]
    baseline = summary.get("baseline") or {}
    if baseline:
        lines += [
            f"Commit: {baseline.get('commit', 'unknown')}",
            f"Server build: {baseline.get('server_build_id') or 'not reported'}",
        ]

    report = summary.get("bridge")
    if report:
        lines += ["", "## Bridge", "", f"**{report['verdict']}.** {report['note']}"]
        if (baseline.get("addon") or {}).get("repository_copy_is_stale"):
            lines.append(
                "\nThe repository's own `addons/didi` was present and differs from the built "
                "one, so the tester had two indistinguishable addons to choose between."
            )

    delta = summary.get("coverage")
    if delta and delta.get("current"):
        current = delta["current"]
        previous = delta.get("previous")
        lines += [
            "",
            "## Coverage",
            "",
            "| Measure | Previous | This run |",
            "| :--- | ---: | ---: |",
            f"| Distinct tools called | {previous.get('distinct_called', '-') if previous else '-'} "
            f"| {current.get('distinct_called', '-')} |",
            f"| Implemented surface | {previous.get('implemented', '-') if previous else '-'} "
            f"| {current.get('implemented', '-')} |",
            f"| Invocations | {previous.get('invocations', '-') if previous else '-'} "
            f"| {current.get('invocations', '-')} |",
        ]
        if delta.get("newly_called"):
            lines.append(f"\nNewly reached: {', '.join(delta['newly_called'])}")
        if delta.get("no_longer_called"):
            lines.append(f"\nNo longer reached: {', '.join(delta['no_longer_called'])}")

    drafting = summary.get("filing") == "drafts"
    if drafting:
        drafts = summary.get("issue_drafts") or []
        lines += ["", "## Issue drafts", "",
                  f"{len(drafts)} held in `{DRAFTS_FILE}` for review. None of them is filed."]
        if drafts:
            lines.append("")
            lines += [f"- {title}" for title in drafts]

    issues = summary.get("issues_filed") or []
    if issues:
        lines += ["", "## Issues filed", ""]
        if drafting:
            lines += ["This was a drafts run and should have filed nothing.", ""]
        lines += [f"- #{issue['number']} {issue['title']}" for issue in issues]

    lines += ["", "## Phases", "", "| Phase | Status | Detail |", "| :--- | :--- | :--- |"]
    for phase in summary["phases"]:
        detail = (phase.get("detail") or "").replace("|", "\\|")[:160]
        lines.append(f"| {phase['name']} | {phase['status']} | {detail} |")
    return "\n".join(lines) + "\n"


def signed_in(engine: str, result: subprocess.CompletedProcess) -> bool:
    """Whether the client just said it holds credentials.

    Each client answers in its own shape and neither reads the other's. Codex
    prints a sentence, and prints it on **stderr**, which is what broke the
    first attempt at this run: a preflight reading only stdout refused a client
    that was signed in the whole time. Claude prints JSON on stdout and says so
    in a field.

    Both are read as: a clean exit plus an affirmative answer. A client that
    cannot be asked is not signed in, because a run that starts unauthenticated
    burns the seed and the clock and leaves no transcript to score.
    """
    if result.returncode != 0:
        return False
    if engine == runner.CODEX:
        return "logged in" in ((result.stdout or "") + (result.stderr or "")).lower()
    try:
        return json.loads(result.stdout or "{}").get("loggedIn") is True
    except json.JSONDecodeError:
        return False


def run(command: list[str], cwd: Path) -> subprocess.CompletedProcess:
    return subprocess.run(
        command, cwd=str(cwd), capture_output=True, text=True,
        encoding="utf-8", errors="replace", check=False,
    )


def issues_filed_since(repo: str, since_iso: str) -> list[dict]:
    """The field-trial issues that appeared during the run.

    Read from the tracker rather than from the ledger, for the same reason
    coverage is: the run's account of itself is the thing under test. A failure
    here is reported as an empty list rather than raised, because a trial whose
    findings are already on disk should not be lost to a network hiccup.
    """
    result = run(
        ["gh", "issue", "list", "--repo", repo, "--label", "field-trial",
         "--state", "all", "--limit", "50", "--json", "number,title,createdAt"],
        REPOSITORY,
    )
    if result.returncode != 0:
        return []
    try:
        issues = json.loads(result.stdout or "[]")
    except json.JSONDecodeError:
        return []
    return [issue for issue in issues if issue.get("createdAt", "") >= since_iso]


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", default="saworbit/didi")
    parser.add_argument("--target", type=Path, help="Trial working directory (default: dated)")
    parser.add_argument("--artifacts", type=Path, default=Path("D:/didi-trials"))
    parser.add_argument("--didi-exe", type=Path, default=REPOSITORY / "build-ninja" / "didi.exe")
    parser.add_argument("--manifest", type=Path,
                        default=REPOSITORY / "build-ninja" / "tool-manifest.json")
    parser.add_argument("--godot-exe", type=Path,
                        default=Path(r"C:\Godot\Godot_v4.7.2-stable_win64_console.exe"))
    parser.add_argument("--budget-usd", type=float, default=40.0,
                        help="Cost ceiling for the tester. Claude only; Codex has no equivalent")
    parser.add_argument("--timeout-seconds", type=int, default=10800)
    parser.add_argument("--engine", choices=runner.ENGINES, default=runner.CLAUDE,
                        help="Which client hosts the tester")
    parser.add_argument("--model", help="Model for the tester session")
    parser.add_argument("--reasoning-effort",
                        help="Reasoning effort for the tester session (Codex)")
    parser.add_argument("--compare", type=Path, help="A previous run's coverage.json")
    parser.add_argument("--drafts", action="store_true",
                        help="Hold findings in ISSUE_DRAFTS.md for review instead of filing them")
    parser.add_argument("--dry-run", action="store_true",
                        help="Exercise the orchestration without launching a tester")
    args = parser.parse_args(argv)

    trial_id = datetime.now(timezone.utc).strftime("trial-%Y%m%d-%H%M%S")
    target = args.target or (args.artifacts / trial_id)
    phases: list[dict] = []
    baseline: dict | None = None
    delta: dict | None = None
    bridge_report: dict | None = None
    issues: list[dict] = []
    drafts: list[str] | None = [] if args.drafts else None

    def record(name: str, status: str, detail: str = "") -> None:
        phases.append({"name": name, "status": status, "detail": detail})

    def finish(outcome: str) -> int:
        summary = trial_summary(
            trial_id, phases, outcome, baseline, delta, bridge_report, issues,
            engine=args.engine, model=args.model,
            tester_environment=CLAUDE_TESTER_ENVIRONMENT if args.engine == runner.CLAUDE else None,
            drafts=drafts,
        )
        destination = target if target.exists() else args.artifacts / trial_id
        destination.mkdir(parents=True, exist_ok=True)
        (destination / "trial.json").write_text(
            json.dumps(summary, indent=2, sort_keys=True), encoding="utf-8"
        )
        (destination / "TRIAL.md").write_text(render_summary(summary), encoding="utf-8")
        print(render_summary(summary))
        print(f"Artifacts: {destination}")
        return 0 if outcome == "scored" else 1

    # Preflight. Everything that can refuse the run happens before the run costs
    # anything, because the client reads its own credential store and this
    # session being signed in says nothing about whether the tester can start.
    # The client is looked up only when a tester is actually going to be
    # launched. A dry run exercises the orchestration and the seed, and both are
    # worth being able to check on a machine that has no client installed at
    # all, which includes every CI runner this suite runs on.
    if args.dry_run:
        record("preflight", "skipped", "dry run: the client is not looked up")
    else:
        try:
            client = runner.resolve_executable(args.engine)
        except FileNotFoundError as error:
            record("preflight", "failed", str(error))
            return finish("client_unavailable")
        if args.engine == runner.CODEX:
            status = run([client, "login", "status"], REPOSITORY)
            remedy = "run `codex login`"
        else:
            status = run([client, "auth", "status"], REPOSITORY)
            remedy = "run `claude auth login`"
        if not signed_in(args.engine, status):
            record("preflight", "failed",
                   f"the {args.engine} client is not signed in; {remedy} and try again")
            return finish("not_authenticated")
        record("preflight", "ok", f"{args.engine} at {client}")

    try:
        baseline = seed_trial.seed(
            target=target, didi_exe=args.didi_exe, godot_exe=args.godot_exe,
            repository=REPOSITORY, manifest=args.manifest,
        )
    except (FileNotFoundError, FileExistsError) as error:
        record("seed", "failed", str(error))
        return finish("seed_failed")
    if baseline.get("manifest_source") == "none":
        # Checked here, before a tester is launched, because the alternative is
        # discovering after the money is spent that the run cannot be scored.
        record("seed", "failed",
               "no tool manifest: the binary would not dump one and no usable fallback was given, "
               "so this run could not be scored against the surface it was handed")
        return finish("no_manifest")
    record("seed", "ok", f"{target} at {baseline['commit'][:9]}, "
                         f"build {baseline.get('server_build_id') or 'unreported'}, "
                         f"manifest {baseline['manifest_source']}")

    # Filled and swapped before a dry run returns, so a dry run proves both
    # still apply to the brief as it stands.
    brief = (HERE / "TRIAL_BRIEF.md").read_text(encoding="utf-8")
    try:
        brief = filled_brief(brief, REPOSITORY, args.godot_exe)
        if args.drafts:
            brief = drafts_brief(brief)
    except ValueError as error:
        record("brief", "failed", str(error))
        return finish("brief_failed")

    if args.dry_run:
        for name in ("run", "score", "bridge", "report"):
            record(name, "skipped", "dry run")
        return finish("dry_run")

    session_id = str(uuid.uuid4())
    if args.engine == runner.CODEX:
        command = runner.build_codex_command(
            mcp_config=runner.read_mcp_config(target / ".mcp.json"),
            add_dirs=[str(REPOSITORY)],
            model=args.model,
            reasoning_effort=args.reasoning_effort,
        )
    else:
        command = runner.build_command(
            session_id=session_id,
            budget_usd=args.budget_usd,
            mcp_config=str(target / ".mcp.json"),
            permission_mode="bypassPermissions",
            add_dirs=[str(REPOSITORY)],
            model=args.model,
            tools=CLAUDE_TESTER_ENVIRONMENT["tools"],
            disable_slash_commands=True,
            setting_sources=CLAUDE_TESTER_ENVIRONMENT["setting_sources"],
        )
    started = datetime.now(timezone.utc).isoformat()
    environment = None
    stub_directory = None
    if args.drafts:
        stub_directory = Path(tempfile.mkdtemp(prefix="didi-trial-gh-"))
        environment = drafts_environment(os.environ, write_gh_stub(stub_directory))
    try:
        completed = runner.run_agent(
            command, brief, target, args.timeout_seconds, target / "agent.log",
            env=environment,
        )
    except subprocess.TimeoutExpired:
        # Kept, not discarded. A tester that ran out of clock still built
        # something, and the ledger it left is the most valuable artifact here.
        record("run", "failed", f"tester exceeded {args.timeout_seconds}s")
        return finish("run_timeout")
    except OSError as error:
        record("run", "failed", f"could not launch the tester: {error}")
        return finish("tester_unavailable")
    finally:
        if stub_directory is not None:
            shutil.rmtree(stub_directory, ignore_errors=True)
    record("run", "ok" if completed.returncode == 0 else "failed",
           f"exit {completed.returncode}")

    if args.engine == runner.CODEX:
        # Codex prints its transcript rather than filing one under a path the
        # harness can name, so the run's own stdout is the record. Written even
        # when the run failed: a tester that died halfway still called tools,
        # and the ledger it left is the most valuable artifact here.
        transcript = target / "agent.jsonl"
        transcript.write_text(completed.stdout or "", encoding="utf-8")
    else:
        transcript = keep_transcript(target, session_id)
    if not transcript.is_file():
        record("score", "failed", f"no transcript at {transcript}")
        return finish("no_transcript")

    manifest_path = target / "tool-manifest.baseline.json"
    if not manifest_path.is_file():
        record("score", "failed", "the seed copied no manifest, so there is nothing to score against")
        return finish("no_manifest")

    with transcript.open(encoding="utf-8", errors="replace") as handle:
        counts = coverage.extract_invocations(handle)
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    report = coverage.build_report(counts, manifest["names"]["implemented"])
    (target / "coverage.json").write_text(
        json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    previous = json.loads(args.compare.read_text(encoding="utf-8")) if args.compare else None
    delta = coverage_delta(previous, report)
    record("score", "ok", f"{report['totals']['distinct_called']} distinct tools, "
                          f"{report['totals']['invocations']} invocations")

    with transcript.open(encoding="utf-8", errors="replace") as handle:
        observations = bridge.extract_observations(handle)
    bridge_report = bridge.bridge_verdict(observations, baseline.get("server_build_id"))
    (target / "bridge.json").write_text(
        json.dumps(bridge_report, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    record("bridge", "ok" if bridge_report["verdict"] == bridge.MATCHED else "failed",
           bridge_report["note"][:150])

    issues = issues_filed_since(args.repo, started)
    if args.drafts:
        drafts = read_drafts(target / DRAFTS_FILE)
        record("report", "ok" if not issues else "failed",
               f"{len(drafts)} draft(s) in {DRAFTS_FILE}, {len(issues)} issue(s) filed during the run")
    else:
        record("report", "ok", f"{len(issues)} issue(s) filed during the run")
    return finish("scored")


if __name__ == "__main__":
    raise SystemExit(main())
