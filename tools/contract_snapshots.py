#!/usr/bin/env python3
"""Record what Didi shows a client, and fail when it changes without review.

Q3 in docs/BUILD_QUEUE.md. The next queue items change what the tools list
and what they answer. A reviewer should see each such change as a diff in the
pull request, not learn of it from a client that broke. So the contract is
committed under ``tests/contract_snapshots``:

* ``offline.json``: ``initialize``, ``tools/list``, ``resources/list``,
  ``resources/templates/list`` and ``prompts/list`` from a server with no
  engine. Every tool's schema, description, annotations and ``_meta`` is here.
* ``live-<line>.json``, one per Godot line CI drives: the same listings once an
  editor is attached, stored as what differs from the offline ones, and the
  answers to the read-only calls in ``calls.json`` against
  ``tests/contract_fixture``.

Values that change from run to run without the contract changing (the session
id, the pid, the pipe endpoint, the build id, the temporary project path) are
replaced by placeholders. They are replaced by value, read from the session
Didi reports, so an identity is caught wherever it appears, inside a string or
under a key nobody listed. Tools are keyed by name because the wire order is a
hash map's, which differs between standard libraries.

Usage::

    python tools/contract_snapshots.py                      # regenerate offline.json
    python tools/contract_snapshots.py --godot <exe> ...    # and live-<line>.json per engine
    python tools/contract_snapshots.py --check [--godot <exe> ...]

Regenerating records everything twice, with a fresh project and a fresh editor
each time, and refuses to write when the two disagree: that is a value this
file does not yet normalise, and committing it would fail every later run.
``--check`` records once, prints a diff against the committed snapshots, and
with ``--out`` writes what it recorded so it can be inspected or adopted.
"""

from __future__ import annotations

import argparse
import difflib
import importlib.util
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from typing import Any

REPO_ROOT = Path(__file__).resolve().parent.parent
SNAPSHOT_DIR = REPO_ROOT / "tests" / "contract_snapshots"
FIXTURE = REPO_ROOT / "tests" / "contract_fixture"
CALLS = SNAPSHOT_DIR / "calls.json"
OFFLINE = SNAPSHOT_DIR / "offline.json"
SCHEMA = 1

# A fixed client, so the handshake answer does not depend on who records it.
PROTOCOL_VERSION = "2025-06-18"
CLIENT_INFO = {"name": "didi-contract-snapshots", "version": "1"}
LISTINGS = ("tools/list", "resources/list", "resources/templates/list", "prompts/list")

# The marker left where a text block repeats structuredContent exactly. Storing
# the copy would double every answer and every diff; when the two differ, the
# text is kept in full and the difference shows.
SAME_AS_STRUCTURED = "<the text of structuredContent>"
ABSENT = "<absent>"

# Measurements, not identities: the same call takes a different time on every
# run, so these fields keep their key and lose their value.
MEASURED_KEYS = frozenset({"elapsed_ms", "duration_seconds", "engine_duration_seconds"})
# Wall-clock times inside strings. Didi's own log lines start with one, and a
# helper process's log reaches answers that report raw engine output.
TIMESTAMP = re.compile(r"\d{4}-\d{2}-\d{2}[ T]\d{2}:\d{2}:\d{2}(?:\.\d+)?")

# Extensions a Godot editor imports. The addon's icons are the only such files
# in a copy of the fixture, and the editor imports them during its first scan,
# which can still be running after the plugin has published its session.
IMPORTED_EXTENSIONS = frozenset({
    ".svg", ".png", ".jpg", ".jpeg", ".webp", ".bmp", ".tga", ".exr", ".hdr",
    ".wav", ".ogg", ".mp3", ".glb", ".gltf", ".fbx", ".obj", ".dae", ".blend",
    ".ttf", ".otf", ".woff", ".woff2", ".csv",
})

# A slow CI runner draws about one editor frame in 0.6 s, and a cold editor
# imports the fixture before it publishes its session.
READY_TIMEOUT_SECONDS = 180
SERVER_TIMEOUT_SECONDS = 180


class SnapshotError(RuntimeError):
    """Recording could not produce a snapshot worth comparing."""


def resolve_binary(explicit: str | None) -> Path:
    """The server to drive: --binary, else the resolver the Python suites use."""
    if explicit:
        path = Path(explicit).resolve()
        if not path.is_file():
            raise SnapshotError(f"--binary names {path}, which is not a file.")
        return path
    module_path = REPO_ROOT / "tests" / "didi_binary.py"
    spec = importlib.util.spec_from_file_location("didi_binary", module_path)
    if spec is None or spec.loader is None:
        raise SnapshotError(f"Cannot load the binary resolver from {module_path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    try:
        return Path(module.resolve()).resolve()
    except Exception as error:  # unittest.SkipTest is how it says nothing is built
        raise SnapshotError(f"No didi binary to record: {error}") from error


def addon_for(binary: Path) -> Path:
    """The addon the build assembled beside this binary, never the source tree's."""
    for candidate in (binary.parent / "addons" / "didi", binary.parent.parent / "addons" / "didi"):
        if (candidate / "plugin.cfg").is_file():
            return candidate
    raise SnapshotError(f"No built addon beside {binary}; build the project first.")


def engine_line(godot: Path) -> tuple[str, str]:
    """The engine's minor line ("4.5") and its full version string."""
    result = subprocess.run(
        [str(godot), "--version"], capture_output=True, text=True, timeout=60
    )
    version = (result.stdout or "").strip().splitlines()
    match = re.match(r"^(\d+)\.(\d+)", version[-1] if version else "")
    if not match:
        raise SnapshotError(f"{godot} --version printed {result.stdout!r}, not a Godot version.")
    return f"{match.group(1)}.{match.group(2)}", version[-1]


def live_path(line: str) -> Path:
    return SNAPSHOT_DIR / f"live-{line}.json"


def load_calls(path: Path = CALLS) -> dict:
    return json.loads(path.read_text(encoding="utf-8"))


# --------------------------------------------------------------------------
# Talking to the server


def request(request_id: int, method: str, params: dict | None = None) -> dict:
    return {"jsonrpc": "2.0", "id": request_id, "method": method, "params": params or {}}


def tool_call(request_id: int, name: str, arguments: dict) -> dict:
    return request(request_id, "tools/call", {"name": name, "arguments": arguments})


def handshake() -> list[dict]:
    return [
        request(0, "initialize", {
            "protocolVersion": PROTOCOL_VERSION, "capabilities": {}, "clientInfo": CLIENT_INFO,
        }),
        {"jsonrpc": "2.0", "method": "notifications/initialized"},
    ]


def exchange(binary: Path, project: Path, env: dict, messages: list[dict]) -> dict[int, dict]:
    """Send every message to one server process and return its replies by id.

    One process per exchange, the way the live harness drives it. Server
    notifications carry no id and are dropped; they are not answers.
    """
    payload = "".join(json.dumps(message) + "\n" for message in messages)
    try:
        completed = subprocess.run(
            [str(binary), "--project", str(project)],
            input=payload.encode("utf-8"), capture_output=True, env=env,
            timeout=SERVER_TIMEOUT_SECONDS,
        )
    except subprocess.TimeoutExpired as error:
        raise SnapshotError(f"didi did not answer within {SERVER_TIMEOUT_SECONDS} s") from error
    replies: dict[int, dict] = {}
    for line in completed.stdout.decode("utf-8").splitlines():
        if not line.startswith("{"):
            continue
        message = json.loads(line)
        if "id" in message and message["id"] is not None:
            replies[int(message["id"])] = message
    wanted = [m["id"] for m in messages if "id" in m]
    missing = [i for i in wanted if i not in replies]
    if missing:
        tail = completed.stderr.decode("utf-8", "replace")[-2000:]
        raise SnapshotError(
            f"didi exited {completed.returncode} without answering request(s) {missing}.\n{tail}"
        )
    return replies


def result_of(reply: dict, what: str) -> Any:
    if "error" in reply:
        raise SnapshotError(f"{what} failed at the protocol level: {json.dumps(reply['error'])}")
    return reply["result"]


def structured(reply: dict, what: str) -> dict:
    result = result_of(reply, what)
    if result.get("isError"):
        raise SnapshotError(f"{what} answered with an error: {result['content'][0]['text']}")
    return result["structuredContent"]


# --------------------------------------------------------------------------
# Normalising


def spellings(path: Path) -> list[str]:
    """Every way a path can be written in an answer, longest first."""
    forms = {str(path), path.as_posix()}
    for form in list(forms):
        if len(form) > 1 and form[1] == ":":
            forms.add(form[0].swapcase() + form[1:])
    return sorted(forms, key=len, reverse=True)


class Identities:
    """Values that differ between runs, each with the placeholder it becomes."""

    def __init__(self) -> None:
        self.substrings: list[tuple[str, str]] = []
        self.exact: dict[str, str] = {}
        self.keyed: dict[str, str] = {key: "<measured>" for key in MEASURED_KEYS}

    def path(self, path: Path, placeholder: str) -> None:
        for form in spellings(path):
            self.substrings.append((form, placeholder))
        resolved = path.resolve()
        if resolved != path:
            self.path(resolved, placeholder)

    def value(self, value: Any, placeholder: str) -> None:
        """An identity long enough to be unmistakable anywhere in a string."""
        if isinstance(value, str) and len(value) >= 8:
            self.substrings.append((value, placeholder))
        elif value is not None:
            self.exact[str(value)] = placeholder

    def key(self, key: str, placeholder: str) -> None:
        """A field whose value is an identity wherever it appears."""
        self.keyed[key] = placeholder

    def session(self, descriptor: dict) -> None:
        for key in ("session_id", "endpoint", "build_id"):
            if descriptor.get(key):
                self.value(descriptor[key], f"<{key}>")
        for key in ("pid", "started_at_ms"):
            self.key(key, f"<{key}>")

    def normalise(self, node: Any, key: str | None = None) -> Any:
        if key in self.keyed and not isinstance(node, (dict, list)):
            return self.keyed[key]
        if isinstance(node, dict):
            return {k: self.normalise(v, k) for k, v in node.items()}
        if isinstance(node, list):
            return [self.normalise(item) for item in node]
        if isinstance(node, str):
            if node in self.exact:
                return self.exact[node]
            # Longest first, so a path inside the project becomes <project>/x
            # rather than <work>/project/x.
            for needle, placeholder in sorted(self.substrings, key=lambda p: len(p[0]), reverse=True):
                if needle in node:
                    node = node.replace(needle, placeholder)
            return TIMESTAMP.sub("<timestamp>", node)
        return node


def fold_text_copy(result: Any) -> Any:
    """Make each text block reviewable.

    A block that repeats structuredContent becomes a marker. Any other block
    that holds JSON, which is every error, is stored parsed under text_json,
    so a diff names the field that moved instead of one long line.
    """
    if not isinstance(result, dict):
        return result
    content = result.get("content")
    if not isinstance(content, list):
        return result
    folded = []
    for block in content:
        if isinstance(block, dict) and block.get("type") == "text":
            try:
                parsed = json.loads(block.get("text", ""))
            except ValueError:
                parsed = None
            if "structuredContent" in result and parsed == result["structuredContent"]:
                block = {**block, "text": SAME_AS_STRUCTURED}
            elif isinstance(parsed, (dict, list)):
                block = {k: v for k, v in block.items() if k != "text"}
                block["text_json"] = parsed
        folded.append(block)
    return {**result, "content": folded}


def keyed_listing(method: str, result: dict) -> dict:
    """A listing with its entries keyed by name, so wire order cannot move it."""
    field = {
        "tools/list": "tools",
        "resources/list": "resources",
        "resources/templates/list": "resourceTemplates",
        "prompts/list": "prompts",
    }[method]
    entries = result.get(field, [])
    key = "uriTemplate" if field == "resourceTemplates" else ("uri" if field == "resources" else "name")
    keyed = {entry[key]: entry for entry in entries}
    if len(keyed) != len(entries):
        raise SnapshotError(f"{method} lists the same {key} twice.")
    return {**result, field: dict(sorted(keyed.items()))}


def overlay(base: Any, other: Any) -> Any:
    """What `other` changes about `base`, or None when nothing does."""
    if isinstance(base, dict) and isinstance(other, dict):
        changed = {}
        for key in sorted(set(base) | set(other)):
            if key not in other:
                changed[key] = ABSENT
            elif key not in base:
                changed[key] = other[key]
            else:
                inner = overlay(base[key], other[key])
                if inner is not None:
                    changed[key] = inner
        return changed or None
    return None if base == other else other


def render(snapshot: dict) -> str:
    return json.dumps(snapshot, indent=2, sort_keys=True, ensure_ascii=False) + "\n"


# --------------------------------------------------------------------------
# Recording


class Workspace:
    """A fresh copy of the fixture, the built addon if an editor will load it,
    and a private session directory."""

    def __init__(self, binary: Path, keep: bool = False, with_addon: bool = False) -> None:
        self.keep = keep
        # resolve() because a runner's TEMP can be an 8.3 short name, and the
        # engine reports the long one.
        self.root = Path(tempfile.mkdtemp(prefix="didi-contract-")).resolve()
        self.project = self.root / "project"
        shutil.copytree(FIXTURE, self.project, ignore=shutil.ignore_patterns(".gitattributes"))
        if with_addon:
            shutil.copytree(addon_for(binary), self.project / "addons" / "didi")
        self.sessions = self.root / "sessions"
        self.sessions.mkdir()
        # The editor's own settings and project list, so nothing the person
        # recording has configured reaches an answer, and their project manager
        # does not collect a temporary project per run.
        self.appdata = self.root / "appdata"
        self.appdata.mkdir()
        self.env = dict(os.environ, DIDI_SESSION_DIR=str(self.sessions))
        self.identities = Identities()
        self.identities.path(self.project, "<project>")
        self.identities.path(self.root, "<work>")
        self.identities.path(binary.parent, "<build>")

    def close(self) -> None:
        if self.keep:
            print(f"Kept the work directory: {self.root}", file=sys.stderr)
            return
        # A stopped editor holds its copy of the extension for a moment.
        for _ in range(20):
            shutil.rmtree(self.root, ignore_errors=True)
            if not self.root.exists():
                return
            time.sleep(0.5)


def record_listings(binary: Path, workspace: Workspace, prefix: list[dict]) -> dict[str, dict]:
    base = len(prefix) + 10
    messages = handshake() + prefix + [request(base + i, m) for i, m in enumerate(LISTINGS)]
    replies = exchange(binary, workspace.project, workspace.env, messages)
    for message in prefix:
        if "id" in message:
            structured(replies[message["id"]], message["params"].get("name", message["method"]))
    listings = {"initialize": result_of(replies[0], "initialize")}
    for i, method in enumerate(LISTINGS):
        listings[method] = keyed_listing(method, result_of(replies[base + i], method))
    return listings


def record_offline(binary: Path, keep: bool = False) -> dict:
    workspace = Workspace(binary, keep)
    try:
        listings = record_listings(binary, workspace, [])
        version = listings["initialize"].get("serverInfo", {}).get("version")
        if version:
            workspace.identities.value(version, "<version>")
        return {"schema": SCHEMA, **workspace.identities.normalise(listings)}
    finally:
        workspace.close()


def wait_for_editor(binary: Path, workspace: Workspace, editor: subprocess.Popen) -> dict:
    """The attached editor's session descriptor, once its scene is open."""
    deadline = time.monotonic() + READY_TIMEOUT_SECONDS
    descriptor = None
    while descriptor is None:
        if editor.poll() is not None:
            raise SnapshotError(f"The editor exited with {editor.returncode} before publishing a session.")
        if time.monotonic() > deadline:
            raise SnapshotError(f"No editor session within {READY_TIMEOUT_SECONDS} s.")
        replies = exchange(binary, workspace.project, workspace.env, handshake() + [
            tool_call(1, "runtime_list_sessions", {"project_path": str(workspace.project)}),
        ])
        sessions = structured(replies[1], "runtime_list_sessions").get("sessions", [])
        live = [s for s in sessions if s.get("kind") == "editor" and s.get("alive")]
        if live:
            descriptor = live[0]
        else:
            time.sleep(0.5)
    # A cold editor can answer before it can open a scene; the harness waits
    # the same way, through the authenticated bridge rather than a log line.
    scene = load_calls()["scene"]
    while True:
        replies = exchange(binary, workspace.project, workspace.env, handshake() + [
            tool_call(1, "runtime_attach_session", {"session_id": descriptor["session_id"]}),
            tool_call(2, "scene_open", {"scene_path": scene}),
        ])
        opened = result_of(replies[2], "scene_open")
        if not opened.get("isError") and opened.get("structuredContent", {}).get("opened") is True:
            break
        if editor.poll() is not None:
            raise SnapshotError(f"The editor exited with {editor.returncode} before opening {scene}.")
        if time.monotonic() > deadline:
            raise SnapshotError(f"The editor did not open {scene} within {READY_TIMEOUT_SECONDS} s.")
        time.sleep(0.5)
    # Answers that read the project's files see the first scan's imports, so
    # wait for the files themselves rather than for a flag Didi does not report.
    while not imports_settled(workspace.project):
        if editor.poll() is not None:
            raise SnapshotError(f"The editor exited with {editor.returncode} during its first import.")
        if time.monotonic() > deadline:
            raise SnapshotError(f"The editor did not finish importing within {READY_TIMEOUT_SECONDS} s.")
        time.sleep(0.5)
    return descriptor


def imports_settled(project: Path) -> bool:
    """Whether every importable file has its sidecar, its output and the .md5
    Godot writes after the output."""
    for source in project.rglob("*"):
        if ".godot" in source.relative_to(project).parts or source.suffix.lower() not in IMPORTED_EXTENSIONS:
            continue
        sidecar = source.with_name(source.name + ".import")
        if not sidecar.is_file():
            return False
        declared = re.search(r"^dest_files=\[(.*)\]$", sidecar.read_text(encoding="utf-8", errors="replace"), re.M)
        if declared is None:
            return False
        for output in re.findall(r'"res://([^"]+)"', declared.group(1)):
            path = project / output
            if not path.is_file() or not path.with_suffix(".md5").is_file():
                return False
    return True


def substitute(arguments: Any, workspace: Workspace) -> Any:
    """calls.json writes the fixture's absolute path as <project>."""
    if isinstance(arguments, dict):
        return {k: substitute(v, workspace) for k, v in arguments.items()}
    if isinstance(arguments, list):
        return [substitute(v, workspace) for v in arguments]
    if isinstance(arguments, str):
        return arguments.replace("<project>", str(workspace.project))
    return arguments


def stop(editor: subprocess.Popen, pid: Any) -> None:
    """Stop the editor, and the process that published the session if it differs.

    The console build is a launcher, so the pid in the descriptor can be a
    child of the process started here.
    """
    if editor.poll() is None:
        editor.kill()
    try:
        editor.wait(timeout=30)
    except subprocess.TimeoutExpired:
        pass
    if isinstance(pid, int) and pid != editor.pid:
        if os.name == "nt":
            subprocess.run(["taskkill", "/F", "/T", "/PID", str(pid)], capture_output=True)
        else:
            try:
                os.kill(pid, 9)
            except OSError:
                pass


def record_live(binary: Path, godot: Path, offline: dict, keep: bool = False) -> tuple[str, dict, list[str]]:
    """The engine line, its snapshot, and every call that answered unlike calls.json says."""
    line, version = engine_line(godot)
    calls = load_calls()
    workspace = Workspace(binary, keep, with_addon=True)
    env = dict(workspace.env, APPDATA=str(workspace.appdata), GODOT_BIN=str(godot))
    workspace.env = env
    log = open(workspace.root / "editor.log", "wb")
    editor = subprocess.Popen(
        [str(godot), "--headless", "--editor", "--path", str(workspace.project)],
        stdout=log, stderr=subprocess.STDOUT, env=env,
    )
    descriptor: dict = {}
    first = 100
    try:
        try:
            descriptor = wait_for_editor(binary, workspace, editor)
            attach = tool_call(1, "runtime_attach_session", {"session_id": descriptor["session_id"]})
            listings = record_listings(binary, workspace, [attach])
            messages = handshake() + [attach] + [
                tool_call(first + i, call["tool"], substitute(call.get("arguments", {}), workspace))
                for i, call in enumerate(calls["calls"])
            ]
            replies = exchange(binary, workspace.project, env, messages)
            structured(replies[1], "runtime_attach_session")
        finally:
            stop(editor, descriptor.get("pid"))
            log.close()
    except SnapshotError as error:
        # On a runner the kept directory is gone with the machine, so the
        # editor's own last words go into the error itself.
        workspace.keep = True
        workspace.close()
        lines = (workspace.root / "editor.log").read_text(encoding="utf-8", errors="replace").splitlines()
        raise SnapshotError(
            f"Godot {line}: {error}\nThe editor's last output:\n  " + "\n  ".join(lines[-40:])
        ) from error
    identities = workspace.identities
    identities.session(descriptor)
    identities.path(godot, "<godot>")
    identities.path(godot.parent, "<godot_dir>")
    version_string = listings["initialize"].get("serverInfo", {}).get("version")
    if version_string:
        identities.value(version_string, "<version>")

    problems = []
    answers = []
    for i, call in enumerate(calls["calls"]):
        reply = replies[first + i]
        if "error" in reply:
            problems.append(f"{call['tool']} failed at the protocol level: {json.dumps(reply['error'])}")
            answers.append({"tool": call["tool"], "arguments": call.get("arguments", {}), "error": reply["error"]})
            continue
        result = reply["result"]
        if bool(result.get("isError")) != bool(call.get("expect_error")):
            text = result.get("content", [{}])[0].get("text", "")
            said = "answered with an error" if result.get("isError") else "succeeded, and calls.json expects an error"
            problems.append(f"{call['tool']} {json.dumps(call.get('arguments', {}))} {said}: {text[:400]}")
        answers.append({
            "tool": call["tool"],
            "arguments": call.get("arguments", {}),
            "result": fold_text_copy(result),
        })
    if problems:
        # The editor's log is the first thing to read about an answer nobody expected.
        workspace.keep = True
    workspace.close()

    connected = identities.normalise(listings)
    differences = {}
    for method in LISTINGS:
        changed = overlay(offline[method], connected[method])
        if changed is not None:
            differences[method] = changed
    return line, {
        "schema": SCHEMA,
        "engine": version,
        "listings_when_attached": differences,
        "calls": identities.normalise(answers),
    }, [f"Godot {line}: {problem}" for problem in problems]


# --------------------------------------------------------------------------
# Comparing and writing


def difference(label: str, expected: str, actual: str) -> list[str]:
    return list(difflib.unified_diff(
        expected.splitlines(), actual.splitlines(),
        fromfile=f"committed/{label}", tofile=f"recorded/{label}", lineterm="", n=3,
    ))


def first_disagreement(a: Any, b: Any, path: str = "$") -> str | None:
    """The JSON path of the first value two recordings disagree on."""
    if isinstance(a, dict) and isinstance(b, dict):
        for key in sorted(set(a) | set(b)):
            if key not in a or key not in b:
                return f"{path}.{key}"
            found = first_disagreement(a[key], b[key], f"{path}.{key}")
            if found:
                return found
        return None
    if isinstance(a, list) and isinstance(b, list):
        if len(a) != len(b):
            return f"{path} (length {len(a)} and {len(b)})"
        for i, (x, y) in enumerate(zip(a, b)):
            found = first_disagreement(x, y, f"{path}[{i}]")
            if found:
                return found
        return None
    return None if a == b else f"{path}: {json.dumps(a)[:120]} then {json.dumps(b)[:120]}"


def stable(label: str, first: dict, second: dict) -> None:
    where = first_disagreement(first, second)
    if where:
        raise SnapshotError(
            f"{label} differed between two recordings at {where}. That value changes from run "
            f"to run; normalise it in tools/contract_snapshots.py before committing a snapshot."
        )


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--check", action="store_true",
                        help="compare with the committed snapshots instead of writing them")
    parser.add_argument("--godot", action="append", default=[], type=Path,
                        help="a Godot executable to record a live snapshot with; repeat for each line")
    parser.add_argument("--binary", help="the didi server to record (default: the newest build)")
    parser.add_argument("--out", type=Path,
                        help="with --check, also write what was recorded to this directory")
    parser.add_argument("--keep-work", action="store_true",
                        help="keep each temporary project and its editor.log")
    args = parser.parse_args(argv)

    recorded: dict[Path, dict] = {}
    problems: list[str] = []
    try:
        binary = resolve_binary(args.binary)
        print(f"Recording {binary}", file=sys.stderr)
        offline = record_offline(binary, args.keep_work)
        recorded[OFFLINE] = offline
        if not args.check:
            stable("offline.json", offline, record_offline(binary, args.keep_work))
        for godot in args.godot:
            godot = godot.resolve()
            if not godot.is_file():
                raise SnapshotError(f"--godot names {godot}, which is not a file.")
            line, live, unexpected = record_live(binary, godot, offline, args.keep_work)
            print(f"Recorded Godot {line} ({live['engine']})", file=sys.stderr)
            recorded[live_path(line)] = live
            problems += unexpected
            if not args.check and not unexpected:
                _, again, unexpected = record_live(binary, godot, offline, args.keep_work)
                problems += unexpected
                stable(live_path(line).name, live, again)
    except SnapshotError as error:
        problems.append(str(error))

    if args.out:
        # Whatever was recorded, even from a run that failed, is the evidence.
        args.out.mkdir(parents=True, exist_ok=True)
        for path, snapshot in recorded.items():
            (args.out / path.name).write_text(render(snapshot), encoding="utf-8", newline="\n")
    if problems:
        for problem in problems:
            print(f"contract_snapshots: {problem}", file=sys.stderr)
        return 2

    if not args.check:
        SNAPSHOT_DIR.mkdir(parents=True, exist_ok=True)
        for path, snapshot in recorded.items():
            path.write_text(render(snapshot), encoding="utf-8", newline="\n")
            print(f"Wrote {path.relative_to(REPO_ROOT).as_posix()}")
        if not args.godot:
            print("No --godot given, so the live-<line>.json snapshots were not re-recorded.")
        return 0

    failed = False
    for path, snapshot in recorded.items():
        label = path.relative_to(REPO_ROOT).as_posix()
        actual = render(snapshot)
        if not path.is_file():
            print(f"{label} is not committed. Record it with: python tools/contract_snapshots.py --godot <exe>")
            failed = True
            continue
        expected = path.read_text(encoding="utf-8")
        if expected != actual:
            failed = True
            print(f"{label} does not match what this build answers:")
            print("\n".join(difference(label, expected, actual)))
        else:
            print(f"{label} matches.")
    if failed:
        print(
            "\nA tool's schema or a snapshotted answer changed. If the change is intended, regenerate "
            "in this pull request with python tools/contract_snapshots.py --godot <exe> for each engine "
            "line (see docs/DEVELOPER_GUIDE.md), and review the diff as part of it."
        )
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
