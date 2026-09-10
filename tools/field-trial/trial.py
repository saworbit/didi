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
import subprocess
import uuid
from datetime import datetime, timezone
from pathlib import Path

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
        "baseline": baseline,
        "coverage": delta,
        "bridge": bridge_report,
        "issues_filed": issues or [],
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

    issues = summary.get("issues_filed") or []
    if issues:
        lines += ["", "## Issues filed", ""]
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

    def record(name: str, status: str, detail: str = "") -> None:
        phases.append({"name": name, "status": status, "detail": detail})

    def finish(outcome: str) -> int:
        summary = trial_summary(
            trial_id, phases, outcome, baseline, delta, bridge_report, issues,
            engine=args.engine, model=args.model,
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

    if args.dry_run:
        for name in ("run", "score", "bridge", "report"):
            record(name, "skipped", "dry run")
        return finish("dry_run")

    session_id = str(uuid.uuid4())
    brief = (HERE / "TRIAL_BRIEF.md").read_text(encoding="utf-8")
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
            allowed_tools=["Bash", "Read", "Edit", "Write", "Glob", "Grep", "mcp__didi"],
            add_dirs=[str(REPOSITORY)],
            model=args.model,
        )
    started = datetime.now(timezone.utc).isoformat()
    try:
        completed = runner.run_agent(
            command, brief, target, args.timeout_seconds, target / "agent.log"
        )
    except subprocess.TimeoutExpired:
        # Kept, not discarded. A tester that ran out of clock still built
        # something, and the ledger it left is the most valuable artifact here.
        record("run", "failed", f"tester exceeded {args.timeout_seconds}s")
        return finish("run_timeout")
    except OSError as error:
        record("run", "failed", f"could not launch the tester: {error}")
        return finish("tester_unavailable")
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
        transcript = runner.transcript_path(target, session_id)
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
    record("report", "ok", f"{len(issues)} issue(s) filed during the run")
    return finish("scored")


if __name__ == "__main__":
    raise SystemExit(main())
