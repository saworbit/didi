"""Launch one agent session as a subprocess and find what it left behind.

A session cannot be started from inside another session: it needs its own client
and its own MCP connection, which is the whole point of the trial. So the loop
shells out. Keeping command construction pure means the flags that bound cost and
blast radius are unit-tested rather than discovered in a runaway run.
"""

from __future__ import annotations

import json
import shutil
import subprocess
from pathlib import Path

CLAUDE = "claude"
CODEX = "codex"
ENGINES = (CLAUDE, CODEX)
DEFAULT_PROJECTS_ROOT = Path.home() / ".claude" / "projects"


def build_command(
    session_id: str,
    budget_usd: float,
    mcp_config: str | None = None,
    permission_mode: str = "acceptEdits",
    allowed_tools: list[str] | None = None,
    add_dirs: list[str] | None = None,
    model: str | None = None,
) -> list[str]:
    """The exact argv for one non-interactive agent run.

    The prompt is deliberately absent: it goes in on stdin. Several of these
    options take a variable number of values, so a trailing positional prompt is
    swallowed by whichever variadic flag happens to come last. That is not a
    hypothetical ordering worry, it is how the first live cycle failed.

    `--max-budget-usd` is the ceiling that makes an unattended loop safe to walk
    away from, and `--session-id` is what makes the transcript findable instead
    of guessed at, which matters because the transcript is the only honest record
    of which tools a run actually reached for.
    """
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
    return command


def toml_value(value: object) -> str:
    """A TOML literal for a `codex -c key=value` override.

    Literal strings, because every value that goes through here is a Windows
    path or an argument list holding one, and a basic string would need every
    backslash doubled by whoever wrote the caller. A literal string cannot
    express a quote or a newline, so those are refused here rather than emitted
    as something TOML will misread.
    """
    if isinstance(value, (list, tuple)):
        return "[" + ",".join(toml_value(item) for item in value) + "]"
    text = str(value)
    if "'" in text or "\n" in text:
        raise ValueError(f"Cannot express {text!r} as a TOML literal string")
    return f"'{text}'"


def codex_mcp_overrides(mcp_config: dict) -> list[str]:
    """The `-c` flags that put the seed's MCP servers in front of a Codex tester.

    Codex configures MCP servers in `config.toml` rather than from a file the
    way the Claude client does, so the seed's `.mcp.json` is translated rather
    than duplicated. One seed artifact then describes the server under test for
    both engines, which is what makes two runs comparable at all.
    """
    overrides: list[str] = []
    for name, server in (mcp_config.get("mcpServers") or {}).items():
        command = server.get("command")
        if not command:
            raise ValueError(f"MCP server {name} in the seed has no command to launch")
        overrides += ["-c", f"mcp_servers.{name}.command={toml_value(command)}"]
        arguments = server.get("args") or []
        if arguments:
            overrides += ["-c", f"mcp_servers.{name}.args={toml_value(arguments)}"]
    return overrides


def build_codex_command(
    mcp_config: dict | None = None,
    add_dirs: list[str] | None = None,
    model: str | None = None,
    reasoning_effort: str | None = None,
) -> list[str]:
    """The exact argv for one non-interactive Codex run.

    Three flags carry the weight and none of them is optional:

    `--json` is the transcript. Codex files a rollout of its own under
    `CODEX_HOME`, but naming that file means guessing at a timestamp, while the
    event stream is handed straight to the caller and holds a completed MCP call
    as one record with the server, the tool and the result together.

    `--ignore-user-config` is this engine's `--strict-mcp-config`. Without it a
    tester sees every MCP server and plugin the machine happens to have enabled,
    and a trial scored against a different tool set than it was seeded with is
    not a trial. It drops the model too, which is why the model is passed
    explicitly rather than left to the config that was just discarded.

    `--skip-git-repo-check` because a seeded trial directory is deliberately not
    a repository: the brief forbids committing anything, so there is nothing for
    one to hold.

    There is no cost ceiling to set. Codex has no equivalent of
    `--max-budget-usd`, so the timeout in `run_agent` is the only bound on a run
    that has stopped finishing, and that is worth knowing before walking away
    from one rather than after.
    """
    command = [
        CODEX,
        "exec",
        "--json",
        "--ignore-user-config",
        "--skip-git-repo-check",
        "--sandbox", "danger-full-access",
        "--dangerously-bypass-approvals-and-sandbox",
    ]
    if model:
        command += ["--model", model]
    if reasoning_effort:
        command += ["-c", f"model_reasoning_effort={toml_value(reasoning_effort)}"]
    if mcp_config:
        command += codex_mcp_overrides(mcp_config)
    for directory in add_dirs or []:
        command += ["--add-dir", directory]
    # The prompt goes in on stdin, as it does for the other engine and for the
    # same reason: --add-dir and -c both take a value, and a trailing positional
    # is read as one of them whenever the last flag is the one that takes it.
    return command


def read_mcp_config(path: Path) -> dict:
    return json.loads(Path(path).read_text(encoding="utf-8"))


def transcript_slug(absolute_path: str) -> str:
    """The directory name the client files a transcript under.

    Separate from `transcript_path` because it is the only part of that lookup
    with a platform-independent answer. `Path.resolve()` is not: given a Windows
    path it returns it unchanged on Windows and glues the current directory onto
    the front of it on Linux, so a test that asserts a slug for a Windows path
    through the resolving function passes on the machine the loop runs on and
    fails in CI. The rule itself, colon and both separators become hyphens, holds
    everywhere and is what is worth asserting.
    """
    return absolute_path.replace(":", "-").replace("\\", "-").replace("/", "-")


def transcript_path(
    working_directory: Path,
    session_id: str,
    projects_root: Path | None = None,
) -> Path:
    """Where the client will have written this session's transcript.

    The client keys transcripts by a slug of the working directory, so the
    directory is resolved to the absolute form the client itself would have seen.
    """
    root = projects_root or DEFAULT_PROJECTS_ROOT
    return root / transcript_slug(str(Path(working_directory).resolve())) / f"{session_id}.jsonl"


def resolve_executable(name: str = CLAUDE) -> str:
    """The real path to the client, because a bare name does not launch on Windows.

    npm installs the CLI as `claude.cmd`, and CreateProcess will not resolve a
    bare `claude` to it. Without this the loop dies before its first agent turn.
    """
    found = shutil.which(name)
    if not found:
        raise FileNotFoundError(
            f"{name} is not on PATH. A cycle cannot run without the client that hosts the agent."
        )
    return found


def run_agent(
    command: list[str],
    prompt: str,
    working_directory: Path,
    timeout_seconds: int,
    log_path: Path | None = None,
) -> subprocess.CompletedProcess:
    """Run the agent to completion, recording everything it printed.

    A timeout is not the cost ceiling; `--max-budget-usd` is. This is the guard
    against a run that has stopped spending and stopped finishing.
    """
    if not prompt.strip():
        raise ValueError("An agent run needs a prompt")
    resolved = [resolve_executable(command[0]), *command[1:]]
    completed = subprocess.run(
        resolved,
        input=prompt,
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
