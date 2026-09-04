"""Launch one agent session as a subprocess and find what it left behind.

A session cannot be started from inside another session: it needs its own client
and its own MCP connection, which is the whole point of the trial. So the loop
shells out. Keeping command construction pure means the flags that bound cost and
blast radius are unit-tested rather than discovered in a runaway run.
"""

from __future__ import annotations

import subprocess
from pathlib import Path

CLAUDE = "claude"
DEFAULT_PROJECTS_ROOT = Path.home() / ".claude" / "projects"


def build_command(
    prompt: str,
    session_id: str,
    budget_usd: float,
    mcp_config: str | None = None,
    permission_mode: str = "acceptEdits",
    allowed_tools: list[str] | None = None,
    add_dirs: list[str] | None = None,
    model: str | None = None,
) -> list[str]:
    """The exact argv for one non-interactive agent run.

    `--max-budget-usd` is the ceiling that makes an unattended loop safe to walk
    away from, and `--session-id` is what makes the transcript findable instead
    of guessed at, which matters because the transcript is the only honest record
    of which tools a run actually reached for.
    """
    if not prompt.strip():
        raise ValueError("An agent run needs a prompt")
    if budget_usd <= 0:
        raise ValueError("budget_usd must be a positive ceiling")

    command = [
        CLAUDE,
        "--print",
        "--output-format", "json",
        "--session-id", session_id,
        "--max-budget-usd", str(budget_usd),
        "--permission-mode", permission_mode,
    ]
    if mcp_config:
        # Strict, so the run sees the server under test and nothing this machine
        # happens to have configured. A trial scored against a different tool set
        # than it was seeded with is not a trial.
        command += ["--mcp-config", mcp_config, "--strict-mcp-config"]
    if allowed_tools:
        command += ["--allowed-tools", *allowed_tools]
    if add_dirs:
        for directory in add_dirs:
            command += ["--add-dir", directory]
    if model:
        command += ["--model", model]
    command.append(prompt)
    return command


def transcript_path(
    working_directory: Path,
    session_id: str,
    projects_root: Path | None = None,
) -> Path:
    """Where the client will have written this session's transcript.

    The client keys transcripts by a slug of the working directory, replacing the
    drive colon and every separator with a hyphen.
    """
    root = projects_root or DEFAULT_PROJECTS_ROOT
    slug = str(Path(working_directory).resolve()).replace(":", "-").replace("\\", "-").replace("/", "-")
    return root / slug / f"{session_id}.jsonl"


def run_agent(
    command: list[str],
    working_directory: Path,
    timeout_seconds: int,
    log_path: Path | None = None,
) -> subprocess.CompletedProcess:
    """Run the agent to completion, recording everything it printed.

    A timeout is not the cost ceiling; `--max-budget-usd` is. This is the guard
    against a run that has stopped spending and stopped finishing.
    """
    completed = subprocess.run(
        command,
        cwd=str(working_directory),
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        timeout=timeout_seconds,
        check=False,
    )
    if log_path is not None:
        log_path.parent.mkdir(parents=True, exist_ok=True)
        log_path.write_text(
            (completed.stdout or "") + "\n--- stderr ---\n" + (completed.stderr or ""),
            encoding="utf-8",
        )
    return completed
