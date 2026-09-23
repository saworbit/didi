#!/usr/bin/env python3
"""Install a release archive the way its README says, and check that it works.

The release workflow checks a great deal about an archive before it is
published: exactly which files it holds, what each binary links against, and
that the staged server answers an MCP handshake. What no runner does is the
thing a user does next -- copy `addons/didi` into a Godot project, open the
editor, and see whether the plugin comes up. A runner has no Godot, and the
live harness drives its own fixture addon rather than the shipped one.

That gap is not hypothetical. v2.0.0's extension printed
`ERROR: Attempt to get non-existent interface function` into every 4.5.1
editor that loaded it, and nothing in CI could have seen it. This tool opens
the archive's own addon in a fresh project on every editor it is given and
reports what the engine said.

For each archive it checks, in order:

* **Layout.** One top-level folder, holding exactly README.md, LICENSE,
  THIRD_PARTY_NOTICES.txt, the server and its class reference, and the addon
  this checkout tracks plus the library built for it. Nothing missing, nothing
  extra, nothing empty. Run it from the checkout of the tag being released, or
  the addon list is the wrong one to compare against.
* **Version.** What `bin/didi --version` reports, and that the addon's
  `plugin.cfg` and the README's title agree with it.
* **MCP handshake.** `initialize` and `tools/list` against the fresh project,
  answered by the archive's own server.
* **A fresh install, per editor.** A new project with the archive's addon
  enabled in `project.godot`, opened by a headless editor until the plugin
  says it is active. Any ERROR or WARNING line, or a library the engine could
  not open, is a finding, and so is a run in which the extension never
  published a session.

The editor checks run the host's own binaries, so a Windows machine checks the
Windows archive; the other two need a Mac or a Linux box with Godot on it.

Usage::

    python tools/check_release_archive.py didi-windows-x64.zip \\
        --godot C:/Godot/Godot_v4.5.1-stable_win64_console.exe \\
        --godot C:/Godot/Godot_v4.6.2-stable_win64_console.exe \\
        --godot C:/Godot/Godot_v4.7.2-stable_win64_console.exe \\
        --expect-version 2.0.1

    python tools/check_release_archive.py didi-linux-x64.tar.gz --no-editor

`--godot` may be repeated and defaults to `GODOT_BIN`. On Windows use the
`_console` build, since the plain one prints nothing a caller can read. The
exit status is 1 when anything was found.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import threading
import time
import zipfile
from dataclasses import dataclass, field
from pathlib import Path
from typing import Iterable, Sequence

REPO_ROOT = Path(__file__).resolve().parent.parent

# Written by addons/didi/didi_plugin.gd once the plugin has entered the tree.
PLUGIN_ACTIVE = "Editor Plugin active"

# What the engine prints when something is wrong at load time. Godot prefixes
# its own diagnostics with ERROR: or WARNING:, a GDScript failure with SCRIPT
# ERROR:, and the two library messages are what a missing or foreign-arch
# extension produces.
PROBLEM = re.compile(
    r"\b(ERROR|WARNING)\b|Can't open dynamic library|No GDExtension library found"
)

VERSION_LINE = re.compile(r"^didi \(godot-mcp-native\) v(\S+)$")

DOCUMENTS = ("README.md", "LICENSE", "THIRD_PARTY_NOTICES.txt")

PLATFORMS = {
    "windows": ("didi.exe", "didi_extension.dll"),
    "linux": ("didi", "libdidi_extension.so"),
    "macos": ("didi", "libdidi_extension.dylib"),
}


@dataclass
class EditorRun:
    godot: str
    plugin_active: bool = False
    sessions_published: int = 0
    problems: list[str] = field(default_factory=list)
    timed_out: bool = False

    @property
    def ok(self) -> bool:
        return self.plugin_active and self.sessions_published > 0 and not self.problems


@dataclass
class Report:
    archive: str
    platform: str = ""
    version: str = ""
    tools_listed: int = 0
    findings: list[str] = field(default_factory=list)
    editors: list[EditorRun] = field(default_factory=list)

    @property
    def ok(self) -> bool:
        return not self.findings and all(run.ok for run in self.editors)


def unpack(archive: Path, destination: Path) -> Path:
    """Unpack *archive* into *destination* and return its one top-level folder.

    A directory is accepted as already unpacked. An archive that does not hold
    exactly one top-level folder is refused, because unpacking it would scatter
    files into whatever directory the user was in.
    """

    if archive.is_dir():
        return archive
    name = archive.name.lower()
    if name.endswith(".zip"):
        with zipfile.ZipFile(archive) as bundle:
            bundle.extractall(destination)
    elif name.endswith((".tar.gz", ".tgz")):
        with tarfile.open(archive, "r:gz") as bundle:
            if hasattr(tarfile, "data_filter"):
                bundle.extractall(destination, filter="data")
            else:  # pragma: no cover - Python without the extraction filters
                bundle.extractall(destination)
    else:
        raise ValueError(f"{archive.name}: not a .zip or .tar.gz")
    entries = [entry for entry in destination.iterdir()]
    if len(entries) != 1 or not entries[0].is_dir():
        names = ", ".join(sorted(entry.name for entry in entries)) or "nothing"
        raise ValueError(f"{archive.name}: expected one top-level folder, found {names}")
    return entries[0]


def detect_platform(root: Path) -> str:
    """Which platform an unpacked archive is for, from the library it carries."""

    library_dir = root / "addons" / "didi" / "bin"
    for platform, (_, library) in PLATFORMS.items():
        if (library_dir / library).is_file():
            return platform
    raise ValueError(f"{root.name}: no Didi extension library under addons/didi/bin")


def tracked_addon_files(repo_root: Path = REPO_ROOT) -> list[str]:
    """The addon files this checkout tracks, as archive-relative paths."""

    result = subprocess.run(
        ["git", "-C", str(repo_root), "ls-files", "addons/didi"],
        capture_output=True,
        text=True,
        check=True,
    )
    return sorted(line for line in result.stdout.splitlines() if line.strip())


def expected_files(platform: str, addon_files: Iterable[str]) -> set[str]:
    server, library = PLATFORMS[platform]
    return {
        *DOCUMENTS,
        f"bin/{server}",
        "bin/didi_class_reference.json",
        f"addons/didi/bin/{library}",
        *addon_files,
    }


def check_layout(root: Path, platform: str, addon_files: Iterable[str]) -> list[str]:
    """Every way the unpacked tree differs from what the release promises."""

    expected = expected_files(platform, addon_files)
    actual = {
        path.relative_to(root).as_posix()
        for path in root.rglob("*")
        if path.is_file() or path.is_symlink()
    }
    findings = [f"missing: {path}" for path in sorted(expected - actual)]
    findings += [f"unexpected: {path}" for path in sorted(actual - expected)]
    findings += [
        f"empty: {path}"
        for path in sorted(expected & actual)
        if (root / path).is_file() and (root / path).stat().st_size == 0
    ]
    return findings


def parse_version(output: str) -> str:
    """The version from `didi --version`, or "" when the first line is not one."""

    first = output.strip().splitlines()[0] if output.strip() else ""
    match = VERSION_LINE.match(first.strip())
    return match.group(1) if match else ""


def check_version(root: Path, reported: str, expected: str | None) -> list[str]:
    findings: list[str] = []
    if not reported:
        return ["the server did not report a version"]
    if expected and reported != expected:
        findings.append(f"the server reports {reported}, expected {expected}")
    plugin = (root / "addons" / "didi" / "plugin.cfg").read_text(encoding="utf-8")
    if f'version="{reported}"' not in plugin:
        findings.append(f"addons/didi/plugin.cfg does not say version=\"{reported}\"")
    readme = (root / "README.md").read_text(encoding="utf-8").splitlines()
    if not readme or f" {reported} " not in f"{readme[0]} ":
        findings.append(f"README.md's title does not name {reported}")
    return findings


def problem_lines(lines: Iterable[str]) -> list[str]:
    """The engine output lines that are findings."""

    return [line.rstrip() for line in lines if PROBLEM.search(line)]


def write_project(root: Path, project: Path) -> None:
    """A new project with the archive's addon copied in and enabled."""

    shutil.copytree(root / "addons" / "didi", project / "addons" / "didi")
    (project / "project.godot").write_text(
        "config_version=5\n\n"
        "[application]\n\n"
        'config/name="didi_archive_check"\n\n'
        "[editor_plugins]\n\n"
        'enabled=PackedStringArray("res://addons/didi/plugin.cfg")\n',
        encoding="utf-8",
    )


def mcp_handshake(server: Path, project: Path) -> tuple[int, list[str]]:
    """Ask the archive's server for its tools. Returns (tool count, findings)."""

    requests = "\n".join(
        json.dumps(message)
        for message in (
            {"jsonrpc": "2.0", "id": 1, "method": "initialize",
             "params": {"protocolVersion": "2024-11-05"}},
            {"jsonrpc": "2.0", "id": 2, "method": "tools/list", "params": {}},
        )
    ) + "\n"
    result = subprocess.run(
        [str(server), "--project", str(project)],
        input=requests,
        capture_output=True,
        text=True,
        timeout=60,
    )
    answers: dict[int, dict] = {}
    for line in result.stdout.splitlines():
        try:
            message = json.loads(line)
        except json.JSONDecodeError:
            continue
        if isinstance(message, dict) and isinstance(message.get("id"), int):
            answers[message["id"]] = message
    findings: list[str] = []
    handshake = answers.get(1, {}).get("result", {})
    if handshake.get("protocolVersion") != "2024-11-05":
        findings.append("the server did not answer initialize")
    tools = answers.get(2, {}).get("result", {}).get("tools", [])
    if not tools:
        findings.append("the server listed no tools")
    return len(tools), findings


def stop(process: subprocess.Popen) -> None:
    """Stop an editor and everything it started.

    On Windows the `_console` build is a launcher that starts the real editor
    as a child, so killing the process this tool started is not enough to be
    sure the editor is gone. `taskkill /T` takes the tree.
    """

    if process.poll() is None:
        if os.name == "nt":
            subprocess.run(["taskkill", "/T", "/F", "/PID", str(process.pid)],
                           capture_output=True, check=False)
        else:
            process.kill()
    process.wait(timeout=30)


def remove_scratch(path: Path, attempts: int = 10) -> None:
    """Delete the scratch tree, waiting out a library the engine still maps.

    Godot copies a reloadable extension to `~name` and holds it until the
    process is fully gone, which on Windows can be a moment after it has been
    reaped. A leftover temporary directory is not a finding, so this gives up
    quietly after a few seconds rather than failing the check.
    """

    for _ in range(attempts):
        shutil.rmtree(path, ignore_errors=True)
        if not path.exists():
            return
        time.sleep(1)
    print(f"note: could not remove {path}; remove it once nothing holds it", file=sys.stderr)


def open_in_editor(godot: Path, project: Path, sessions: Path,
                   timeout: float = 90.0, linger: float = 5.0) -> EditorRun:
    """Open *project* in a headless editor and report what it said.

    The editor is stopped once the plugin has been active for *linger* seconds,
    or after *timeout* if it never is. The session directory is private to the
    run, so a published session can only be this editor's.
    """

    run = EditorRun(godot=godot.name)
    env = dict(os.environ, DIDI_SESSION_DIR=str(sessions))
    process = subprocess.Popen(
        [str(godot), "--headless", "--editor", "--path", str(project)],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        stdin=subprocess.DEVNULL,
        env=env,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    lines: list[str] = []
    active = threading.Event()

    def read() -> None:
        assert process.stdout is not None
        for line in process.stdout:
            lines.append(line)
            if PLUGIN_ACTIVE in line:
                active.set()

    reader = threading.Thread(target=read, daemon=True)
    reader.start()
    if active.wait(timeout):
        time.sleep(linger)
    else:
        run.timed_out = process.poll() is None
    stop(process)
    reader.join(timeout=10)

    run.plugin_active = active.is_set()
    run.problems = problem_lines(lines)
    run.sessions_published = sum(1 for _ in sessions.glob("*")) if sessions.is_dir() else 0
    return run


def check_archive(archive: Path, godots: Sequence[Path], expected_version: str | None,
                  addon_files: Sequence[str]) -> Report:
    report = Report(archive=archive.name)
    scratch_dir = Path(tempfile.mkdtemp(prefix="didi-archive-"))
    try:
        _check_unpacked(archive, godots, expected_version, addon_files, scratch_dir, report)
    finally:
        remove_scratch(scratch_dir)
    return report


def _check_unpacked(archive: Path, godots: Sequence[Path], expected_version: str | None,
                    addon_files: Sequence[str], scratch_dir: Path, report: Report) -> None:
    unpacked = scratch_dir / "unpacked"
    unpacked.mkdir()
    try:
        root = unpack(archive, unpacked)
        report.platform = detect_platform(root)
    except ValueError as error:
        report.findings.append(str(error))
        return

    report.findings += check_layout(root, report.platform, addon_files)
    server = root / "bin" / PLATFORMS[report.platform][0]
    if not server.is_file():
        return
    try:
        version = subprocess.run([str(server), "--version"], capture_output=True,
                                 text=True, timeout=30)
    except OSError as error:
        report.findings.append(f"the server did not run: {error}")
        return
    report.version = parse_version(version.stdout)
    report.findings += check_version(root, report.version, expected_version)

    handshake_project = scratch_dir / "handshake"
    handshake_project.mkdir()
    write_project(root, handshake_project)
    report.tools_listed, findings = mcp_handshake(server, handshake_project)
    report.findings += findings

    for index, godot in enumerate(godots):
        project = scratch_dir / f"editor{index}" / "project"
        project.parent.mkdir()
        write_project(root, project)
        report.editors.append(
            open_in_editor(godot, project, scratch_dir / f"editor{index}" / "sessions")
        )


def print_report(report: Report) -> None:
    verdict = "OK" if report.ok else "FAILED"
    print(f"{report.archive}: {verdict}")
    if report.platform:
        print(f"  platform {report.platform}, version {report.version or '?'}, "
              f"{report.tools_listed} tools listed")
    for finding in report.findings:
        print(f"  - {finding}")
    for run in report.editors:
        state = "OK" if run.ok else "FAILED"
        detail = (f"plugin {'active' if run.plugin_active else 'never active'}, "
                  f"{run.sessions_published} session(s) published, "
                  f"{len(run.problems)} problem line(s)")
        if run.timed_out:
            detail += ", timed out"
        print(f"  {run.godot}: {state} ({detail})")
        for line in run.problems[:15]:
            print(f"      {line}")


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("archives", nargs="+", type=Path,
                        help="release archives (.zip or .tar.gz), or folders already unpacked")
    parser.add_argument("--godot", action="append", type=Path, default=[],
                        help="a Godot editor to open the addon in; repeat for each version")
    parser.add_argument("--no-editor", action="store_true",
                        help="check layout, version and the MCP handshake only")
    parser.add_argument("--expect-version",
                        help="fail unless the server reports this version")
    arguments = parser.parse_args(argv)

    godots = [] if arguments.no_editor else list(arguments.godot)
    if not godots and not arguments.no_editor:
        if os.environ.get("GODOT_BIN"):
            godots = [Path(os.environ["GODOT_BIN"])]
        else:
            parser.error("pass --godot (or set GODOT_BIN), or --no-editor to skip the editor check")
    for godot in godots:
        if not godot.is_file():
            parser.error(f"no Godot executable at {godot}")

    addon_files = tracked_addon_files()
    reports = [check_archive(archive, godots, arguments.expect_version, addon_files)
               for archive in arguments.archives]
    for report in reports:
        print_report(report)
    return 0 if all(report.ok for report in reports) else 1


if __name__ == "__main__":
    sys.exit(main())
