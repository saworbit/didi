"""Drive a Didi server over stdio the way a client would, one call at a time.

Every other harness in this repository asserts something. `tests/didi_tests.exe`
asserts unit behaviour, `tests/run_godot_integration.ps1` asserts a scripted
live scenario, and the field trial in `tools/field-trial` scores what an agent
managed to do unattended. None of them let a person sit down and *poke* the
server: call a tool with a deliberately wrong argument, look at what comes
back, and follow the smell. That exploratory pass is where the argument-shaped
bugs live, because a scripted suite only ever sends arguments its author
already believed in.

This module is the bottom of that stack: a JSON-RPC client that speaks the
MCP handshake and hands back parsed results.

Two shapes, because the difference matters:

* :class:`Session` keeps one server process alive across calls. Anything with
  server-side state needs it -- a confirmation token minted by a `dry_run` is
  held in that process's memory and dies with it, so a dry-run and its confirm
  must be the same `Session` or the token is simply gone.
* :func:`batch` starts a process, sends a list of requests, and reads what
  comes back. Cheaper to write, and right for read-only probing.

Both return the parsed `result` object rather than the transport envelope,
because the interesting failures are *inside* a successful envelope: a
`tools/call` that reports `isError: false` while having done something other
than what was asked is the bug class this harness exists to find.
"""

from __future__ import annotations

import importlib.util
import json
import os
import subprocess
from pathlib import Path
from typing import Any, Iterable

REPOSITORY_ROOT = Path(__file__).resolve().parents[2]

PROTOCOL_VERSION = "2024-11-05"
CLIENT_INFO = {"name": "didi-vibe", "version": "1"}

INITIALIZE = {
    "jsonrpc": "2.0",
    "id": 0,
    "method": "initialize",
    "params": {
        "protocolVersion": PROTOCOL_VERSION,
        "capabilities": {},
        "clientInfo": CLIENT_INFO,
    },
}
INITIALIZED = {"jsonrpc": "2.0", "method": "notifications/initialized"}


def resolve_binary() -> Path:
    """The didi binary to drive.

    Reuses `tests/didi_binary.py` rather than re-deriving the search, because
    that file already carries the answer to the question that costs the most
    time on this machine: build/ and build-ninja/ both exist and one of them is
    usually stale. Loaded by path because `tests/` is not an importable package
    from here; `tools/field-trial/bridge.py` loads its own neighbour the same
    way.
    """
    module_path = REPOSITORY_ROOT / "tests" / "didi_binary.py"
    spec = importlib.util.spec_from_file_location("didi_binary", module_path)
    if spec is None or spec.loader is None:
        raise ImportError(f"Cannot load the binary resolver from {module_path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return Path(module.resolve())


def call(name: str, arguments: dict | None = None, request_id: int = 1) -> dict:
    """One `tools/call` request object."""
    return {
        "jsonrpc": "2.0",
        "id": request_id,
        "method": "tools/call",
        "params": {"name": name, "arguments": arguments if arguments is not None else {}},
    }


def unwrap(response: dict) -> tuple[Any, bool | None]:
    """The payload a caller actually wants, and whether the tool reported an error.

    A `tools/call` result carries the same JSON twice: once parsed in
    `structuredContent` and once as text in `content[0].text`. An error never
    sets `structuredContent`, so this falls back to the text rather than
    reporting nothing. Since #420 that text always parses as an error envelope
    with a code; it used to be a bare string for eighteen tools.
    """
    result = response.get("result")
    if result is None:
        return response.get("error"), True
    if "structuredContent" in result:
        return result["structuredContent"], result.get("isError")
    content = result.get("content") or [{}]
    text = content[0].get("text", "")
    try:
        return json.loads(text), result.get("isError")
    except (ValueError, TypeError):
        return text if text else result, result.get("isError")


class Session:
    """One long-lived server process, driven request by request.

    Use this whenever a later call depends on something an earlier call put in
    the server's memory: confirmation tokens, an attached session selection, or
    anything the blackboard holds for the life of the process.
    """

    def __init__(
        self,
        project: str | os.PathLike[str] | None = None,
        binary: str | os.PathLike[str] | None = None,
        extra_args: Iterable[str] = (),
        log_level: str = "ERROR",
        env: dict[str, str] | None = None,
    ) -> None:
        self.binary = Path(binary) if binary else resolve_binary()
        argv = [str(self.binary), "--log-level", log_level]
        if project:
            argv += ["--project", str(project)]
        argv += list(extra_args)
        self.argv = argv
        environment = dict(os.environ)
        if env:
            environment.update(env)
        self.process = subprocess.Popen(
            argv,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            env=environment,
        )
        self._next_id = 0
        self.initialize_result = self.request("initialize", INITIALIZE["params"])
        self.notify("notifications/initialized")

    def _write(self, message: dict) -> None:
        assert self.process.stdin is not None
        self.process.stdin.write((json.dumps(message) + "\n").encode())
        self.process.stdin.flush()

    def _read(self) -> dict:
        assert self.process.stdout is not None
        line = self.process.stdout.readline()
        if not line:
            raise RuntimeError(
                "The server closed stdout without answering. Its stderr is on "
                "this object's `stderr()` once it has exited."
            )
        return json.loads(line.decode())

    def notify(self, method: str, params: dict | None = None) -> None:
        message: dict = {"jsonrpc": "2.0", "method": method}
        if params is not None:
            message["params"] = params
        self._write(message)

    def request(self, method: str, params: dict | None = None) -> dict:
        """Send one request and return its whole envelope."""
        self._next_id += 1
        message: dict = {"jsonrpc": "2.0", "id": self._next_id, "method": method}
        if params is not None:
            message["params"] = params
        self._write(message)
        return self._read()

    def call(self, name: str, arguments: dict | None = None) -> tuple[Any, bool | None]:
        """Call a tool. Returns `(payload, is_error)`; see :func:`unwrap`."""
        response = self.request(
            "tools/call", {"name": name, "arguments": arguments if arguments is not None else {}}
        )
        return unwrap(response)

    def tools(self) -> list[dict]:
        """The published tool surface, schemas included.

        Worth calling first in any session. Several tools take a parameter
        whose name differs from the obvious guess -- `target_node` rather than
        `node_path`, `scene_path` rather than `output_path`, `setting` rather
        than `setting_path`. Until #397 landed, an unknown property was simply
        ignored, so a guessed name could look like it worked; the schema is now
        enforced, which turns the same mistake into a 400 naming the property.
        Read the schema anyway -- it is faster than a round trip.
        """
        return self.request("tools/list", {})["result"]["tools"]

    def stderr(self) -> str:
        """Whatever the server logged, available once the process has exited."""
        if self.process.stderr is None:
            return ""
        return self.process.stderr.read().decode(errors="replace")

    def close(self, timeout: float = 10.0) -> None:
        try:
            if self.process.stdin is not None:
                self.process.stdin.close()
            self.process.wait(timeout=timeout)
        except Exception:
            self.process.kill()

    def __enter__(self) -> "Session":
        return self

    def __exit__(self, *_exc: object) -> None:
        self.close()


def batch(
    requests: Iterable[dict],
    project: str | os.PathLike[str] | None = None,
    binary: str | os.PathLike[str] | None = None,
    extra_args: Iterable[str] = (),
    log_level: str = "ERROR",
    timeout: float = 90.0,
    env: dict[str, str] | None = None,
) -> tuple[list[dict], str]:
    """Send a whole list of requests to one short-lived server, then read.

    Returns every parsed line of stdout and the collected stderr. A line that
    is not JSON is returned as `{"__raw__": line}` rather than dropped: a
    server that prints something unexpected onto its own protocol stream is
    itself a finding, and swallowing it hides that.

    The handshake is prepended, so callers pass only what they are testing --
    unless a request in the list is itself an `initialize`, which is a fair
    thing to want to probe.
    """
    argv = [str(Path(binary) if binary else resolve_binary()), "--log-level", log_level]
    if project:
        argv += ["--project", str(project)]
    argv += list(extra_args)

    environment = dict(os.environ)
    if env:
        environment.update(env)

    payload = "".join(
        json.dumps(request) + "\n" for request in ([INITIALIZE, INITIALIZED] + list(requests))
    )
    process = subprocess.Popen(
        argv,
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        env=environment,
    )
    try:
        out, err = process.communicate(payload.encode(), timeout=timeout)
    except subprocess.TimeoutExpired:
        process.kill()
        out, err = process.communicate()
        err += b"\n[vibe] the server did not exit within the timeout and was killed"

    responses: list[dict] = []
    for line in out.decode(errors="replace").splitlines():
        line = line.strip()
        if not line:
            continue
        try:
            responses.append(json.loads(line))
        except ValueError:
            responses.append({"__raw__": line})
    return responses, err.decode(errors="replace")
