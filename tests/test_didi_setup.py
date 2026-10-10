"""`didi setup` and `didi doctor`, driven through the real binary (Q12).

The offline half runs everywhere the server is built. From a directory holding
only project.godot, one command installs the addon that matches the binary,
enables it, and writes each project-scoped client's configuration; each of
those configurations is then started exactly as its client would start it, and
has to answer `initialize` and `tools/list`. A rerun has to change nothing, and
files that already hold someone else's work have to keep it.

The live half is opt-in. Set DIDI_SETUP_GODOT to one or more Godot editor
executables, separated by the platform's path separator, and each one is
started by `setup --godot ... --headless` on a fresh project. The server a
client would start then has to reach that editor, which is the session lock
being released as well as the editor answering.
"""

from __future__ import annotations

import hashlib
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

try:
    import didi_binary
except ImportError:
    from tests import didi_binary

BARE_PROJECT = 'config_version=5\n\n[application]\n\nconfig/name="Setup"\n'
PLUGIN = "res://addons/didi/plugin.cfg"
LIBRARY = ("didi_extension.dll" if sys.platform == "win32"
           else "libdidi_extension.dylib" if sys.platform == "darwin"
           else "libdidi_extension.so")
BUILD_ID = re.compile(rb"\d{1,4}\.\d{1,4}\.\d{1,4}\+(?:[0-9a-f]{12}|nogit)\.\d{8}T\d{6}")
CONFIG_FILES = {
    "claude-code": ".mcp.json",
    "cursor": ".cursor/mcp.json",
    "vscode": ".vscode/mcp.json",
    "codex": ".codex/config.toml",
}


def tree_digest(root: Path) -> dict[str, str]:
    """Every file under root and its hash, so "nothing changed" can be checked."""
    return {
        str(path.relative_to(root)).replace("\\", "/"): hashlib.sha256(path.read_bytes()).hexdigest()
        for path in sorted(root.rglob("*"))
        if path.is_file()
    }


def configured_server(project: Path, client: str) -> tuple[str, list[str]]:
    """The command and arguments a client reads out of the file setup wrote."""
    text = (project / CONFIG_FILES[client]).read_text(encoding="utf-8")
    if client == "codex":
        # Only the block setup writes. Python 3.9 has no tomllib, and the two
        # lines are all a client needs from it.
        command = re.search(r"^command = '([^']*)'$", text, re.M).group(1)
        args = re.findall(r"'([^']*)'", re.search(r"^args = \[(.*)\]$", text, re.M).group(1))
        return command, args
    entry = json.loads(text)["servers" if client == "vscode" else "mcpServers"]["didi"]
    return entry["command"], entry["args"]


def handshake(command: str, args: list[str], env: dict[str, str]) -> dict:
    """Start the server the way a client would and ask for its tools."""
    requests = "\n".join(json.dumps(message) for message in (
        {"jsonrpc": "2.0", "id": 1, "method": "initialize",
         "params": {"protocolVersion": "2025-03-26", "capabilities": {},
                    "clientInfo": {"name": "setup-test", "version": "1"}}},
        {"jsonrpc": "2.0", "method": "notifications/initialized"},
        {"jsonrpc": "2.0", "id": 2, "method": "tools/list", "params": {}},
    )) + "\n"
    result = subprocess.run([command, *args], input=requests, capture_output=True, text=True,
                            encoding="utf-8", timeout=120, env=env)
    answers = {}
    for line in result.stdout.splitlines():
        try:
            message = json.loads(line)
        except json.JSONDecodeError:
            continue
        if isinstance(message, dict) and "id" in message:
            answers[message["id"]] = message
    return answers


def patch_build_id(library: Path, stamp_year: str) -> str:
    """Rewrite the build id inside a library to one with another configure year.

    Same length, so the binary is otherwise untouched; nothing loads it here.
    """
    data = library.read_bytes()
    found = set(BUILD_ID.findall(data))
    assert len(found) == 1, found
    old = found.pop()
    new = old[:-15] + stamp_year.encode() + old[-11:]
    library.write_bytes(data.replace(old, new))
    return new.decode()


class SetupFixture(unittest.TestCase):
    def setUp(self):
        self.binary = didi_binary.resolve().resolve()
        self.temp = tempfile.TemporaryDirectory(prefix="didi-setup-")
        self.addCleanup(self._cleanup)
        self.root = Path(self.temp.name)
        self.project = self.root / "project"
        self.project.mkdir()
        (self.project / "project.godot").write_text(BARE_PROJECT, encoding="utf-8", newline="\n")
        self.env = os.environ.copy()
        # A session directory of the test's own, so an editor some other work
        # left running on this machine is not part of the answer.
        self.env["DIDI_SESSION_DIR"] = str(self.root / "sessions")
        self.env.pop("DIDI_PROJECT_ROOT", None)

    def _cleanup(self):
        for _ in range(20):
            try:
                self.temp.cleanup()
                return
            except OSError:
                time.sleep(0.5)

    def run_didi(self, *args: str, binary: Path | None = None, timeout: float = 120) -> subprocess.CompletedProcess:
        return subprocess.run([str(binary or self.binary), *args], capture_output=True, text=True,
                              encoding="utf-8", timeout=timeout, env=self.env)

    def setup_json(self, *args: str, expect: int = 0, binary: Path | None = None) -> dict:
        result = self.run_didi("setup", "--project", str(self.project), "--json", *args, binary=binary)
        self.assertEqual(result.returncode, expect, result.stdout + result.stderr)
        return json.loads(result.stdout)

    def doctor_json(self, expect: int) -> dict:
        result = self.run_didi("doctor", "--project", str(self.project), "--json")
        self.assertEqual(result.returncode, expect, result.stdout + result.stderr)
        return json.loads(result.stdout)

    @staticmethod
    def steps(report: dict) -> dict[str, dict]:
        return {step["step"]: step for step in report["steps"]}

    def bundled_addon(self) -> Path:
        for candidate in (self.binary.parent / "addons" / "didi", self.binary.parent.parent / "addons" / "didi"):
            if (candidate / "didi.gdextension").is_file():
                return candidate
        self.fail(f"no addon beside {self.binary}")


class SetupOffline(SetupFixture):
    def test_a_godot_that_cannot_start_is_reported_at_once(self):
        # On POSIX the detached launch returned before exec, so a --godot that
        # could not be executed showed only as the wait running out, which then
        # named three possible causes instead of the one (#1144).
        not_godot = self.root / "not_godot.txt"
        not_godot.write_text("not an executable\n", encoding="utf-8")
        started = time.monotonic()
        result = self.run_didi("setup", "--project", str(self.project), "--client", "claude-code",
                               "--godot", str(not_godot), "--headless", "--timeout", "90", "--json")
        elapsed = time.monotonic() - started
        editor = self.steps(json.loads(result.stdout))["editor"]
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertEqual(editor["state"], "fail", editor)
        self.assertIn("could not be started", editor["detail"])
        self.assertLess(elapsed, 45, editor)

    def test_bare_project_gets_a_working_session_in_each_client(self):
        report = self.setup_json("--client", "all")
        self.assertTrue(report["ok"])
        steps = self.steps(report)
        self.assertEqual(steps["addon"]["state"], "done")
        self.assertEqual(steps["addon"]["build_id"], report["server"]["build_id"])
        self.assertEqual(steps["plugin"]["state"], "done")
        self.assertEqual(steps["editor"]["state"], "skipped")
        for client in CONFIG_FILES:
            self.assertEqual(steps[f"client:{client}"]["state"], "done", client)

        # The addon is the one beside the binary, file for file.
        source = tree_digest(self.bundled_addon())
        installed = tree_digest(self.project / "addons" / "didi")
        for name, digest in source.items():
            self.assertEqual(installed.get(name), digest, name)
        # Enabled the way the editor's Plugins checkbox writes it.
        self.assertIn(f'enabled=PackedStringArray("{PLUGIN}")',
                      (self.project / "project.godot").read_text(encoding="utf-8"))
        guide = (self.project / "AGENTS.md").read_bytes()
        self.assertFalse(guide.startswith(b"\xef\xbb\xbf"), "the guide must be UTF-8 with no byte-order mark")
        self.assertIn(b"<!-- BEGIN didi -->", guide)
        self.assertIn(b"<!-- END didi -->", guide)

        # Each client's configuration starts a server that answers. That is the
        # session a client gets; the live half proves the editor end of it.
        for client in CONFIG_FILES:
            command, args = configured_server(self.project, client)
            self.assertEqual(Path(command).resolve(), self.binary, client)
            self.assertEqual(args[:2], ["--project", self.project.resolve().as_posix()], client)
            answers = handshake(command, args, self.env)
            self.assertEqual(answers.get(1, {}).get("result", {}).get("serverInfo", {}).get("name"),
                             "didi", client)
            self.assertTrue(answers.get(2, {}).get("result", {}).get("tools"), client)

    def test_a_rerun_changes_nothing(self):
        self.setup_json("--client", "all")
        before = tree_digest(self.project)
        report = self.setup_json("--client", "all")
        for step in report["steps"]:
            self.assertIn(step["state"], ("unchanged", "skipped"), step)
        self.assertEqual(tree_digest(self.project), before)

    def test_files_that_hold_other_work_keep_it(self):
        (self.project / "project.godot").write_text(
            BARE_PROJECT + '\n[editor_plugins]\n\nenabled=PackedStringArray("res://addons/other/plugin.cfg")\n',
            encoding="utf-8", newline="\n")
        (self.project / ".mcp.json").write_text(
            '{\n    "mcpServers": {\n        "other": {"command": "other", "args": []},\n'
            '        "didi": {"command": "old/didi", "args": [], "env": {"KEEP": "1"}}\n    },\n'
            '    "zeta": true\n}\n', encoding="utf-8", newline="\n")
        (self.project / "AGENTS.md").write_bytes(b"# House rules\r\n\r\nUse tabs.\r\n")
        (self.project / ".codex").mkdir()
        # A table a person added under the server Codex starts, as the JSON
        # clients keep the env above (#1144).
        (self.project / ".codex" / "config.toml").write_text(
            'model = "x"\n\n[mcp_servers.other]\ncommand = "o"\n\n[mcp_servers.didi.env]\nKEEP = "1"\n',
            encoding="utf-8", newline="\n")
        report = self.setup_json("--client", "claude-code", "--client", "codex")
        steps = self.steps(report)
        self.assertIn("old/didi", steps["client:claude-code"]["detail"])

        settings = (self.project / "project.godot").read_text(encoding="utf-8")
        self.assertIn(f'enabled=PackedStringArray("res://addons/other/plugin.cfg", "{PLUGIN}")', settings)

        text = (self.project / ".mcp.json").read_text(encoding="utf-8")
        config = json.loads(text)
        self.assertEqual(list(config), ["mcpServers", "zeta"], "key order is kept")
        self.assertEqual(list(config["mcpServers"]), ["other", "didi"])
        self.assertEqual(config["mcpServers"]["didi"]["env"], {"KEEP": "1"}, "keys the person added stay")
        self.assertEqual(Path(config["mcpServers"]["didi"]["command"]).resolve(), self.binary)
        self.assertTrue(text.startswith('{\n    "mcpServers"'), "the file's own indentation is kept")

        guide = (self.project / "AGENTS.md").read_bytes()
        self.assertTrue(guide.startswith(b"# House rules\r\n\r\nUse tabs.\r\n"))
        self.assertNotIn(b"\n", guide.replace(b"\r\n", b""), "CRLF endings are kept")

        toml = (self.project / ".codex" / "config.toml").read_text(encoding="utf-8")
        self.assertTrue(toml.startswith('model = "x"\n\n[mcp_servers.other]\ncommand = "o"\n'))
        self.assertIn("# BEGIN didi\n[mcp_servers.didi]\n", toml)
        self.assertIn('[mcp_servers.didi.env]\nKEEP = "1"\n', toml)
        try:
            import tomllib
        except ImportError:  # Python before 3.11 has no TOML reader.
            tomllib = None
        if tomllib is not None:
            server = tomllib.loads(toml)["mcp_servers"]["didi"]
            self.assertEqual(server["env"], {"KEEP": "1"})
            self.assertEqual(Path(server["command"]).resolve(), self.binary)

    def test_claude_md_takes_the_guide_when_the_project_has_one(self):
        (self.project / "CLAUDE.md").write_text("# Ours\n", encoding="utf-8", newline="\n")
        report = self.setup_json("--client", "claude-code")
        files = [Path(step["file"]).name for step in report["steps"] if step["step"] == "agent-guide"]
        self.assertEqual(files, ["CLAUDE.md"])
        self.assertFalse((self.project / "AGENTS.md").exists())
        self.setup_json("--client", "claude-code", "--client", "cursor")
        self.assertTrue((self.project / "AGENTS.md").exists())

    def test_a_claude_md_above_the_project_sends_the_guide_to_one_here(self):
        # Claude Code skips AGENTS.md below any CLAUDE.md, and the one above is
        # not the project's to write (#1144).
        (self.root / "CLAUDE.md").write_text("# Above\n", encoding="utf-8", newline="\n")
        report = self.setup_json("--client", "claude-code")
        steps = [step for step in report["steps"] if step["step"] == "agent-guide"]
        self.assertEqual([Path(step["file"]).name for step in steps], ["CLAUDE.md"])
        self.assertIn("does not read AGENTS.md below", steps[0]["detail"])
        self.assertIn("<!-- BEGIN didi -->", (self.project / "CLAUDE.md").read_text(encoding="utf-8"))
        self.assertFalse((self.project / "AGENTS.md").exists())
        self.assertEqual((self.root / "CLAUDE.md").read_text(encoding="utf-8"), "# Above\n")

    def test_a_file_it_cannot_reproduce_is_refused_and_left_alone(self):
        (self.project / ".vscode").mkdir()
        original = '{\n  // mine\n  "servers": {}\n}\n'
        (self.project / ".vscode" / "mcp.json").write_text(original, encoding="utf-8", newline="\n")
        report = self.setup_json("--client", "vscode", expect=1)
        step = self.steps(report)["client:vscode"]
        self.assertEqual(step["state"], "fail")
        self.assertIn("comments", step["detail"])
        self.assertEqual((self.project / ".vscode" / "mcp.json").read_text(encoding="utf-8"), original)

        (self.project / ".codex").mkdir()
        foreign = "[mcp_servers.didi]\ncommand = 'mine'\n"
        (self.project / ".codex" / "config.toml").write_text(foreign, encoding="utf-8", newline="\n")
        report = self.setup_json("--client", "codex", expect=1)
        self.assertIn("outside the block", self.steps(report)["client:codex"]["detail"])
        self.assertEqual((self.project / ".codex" / "config.toml").read_text(encoding="utf-8"), foreign)

    def test_a_newer_addon_is_never_replaced_without_saying_so(self):
        self.setup_json()
        library = self.project / "addons" / "didi" / "bin" / LIBRARY
        newer = patch_build_id(library, "2099")
        before = tree_digest(self.project)
        report = self.setup_json(expect=1)
        detail = self.steps(report)["addon"]["detail"]
        self.assertIn(newer, detail)
        self.assertIn("newer than", detail)
        self.assertIn(report["server"]["build_id"], detail)
        self.assertEqual(tree_digest(self.project), before, "a refusal changes nothing")

        report = self.setup_json("--replace-addon")
        self.assertEqual(self.steps(report)["addon"]["state"], "done")
        self.assertEqual(self.steps(report)["addon"]["previous_build_id"], newer)
        self.assertNotIn(newer.encode(), library.read_bytes())

    def test_an_older_or_incomplete_addon_is_replaced(self):
        self.setup_json()
        library = self.project / "addons" / "didi" / "bin" / LIBRARY
        older = patch_build_id(library, "2000")
        report = self.setup_json()
        step = self.steps(report)["addon"]
        self.assertEqual(step["state"], "done")
        self.assertIn(older, step["detail"])
        self.assertIn("newer", step["detail"])

        (self.project / "addons" / "didi" / "didi_console.gd").unlink()
        report = self.setup_json()
        self.assertEqual(self.steps(report)["addon"]["state"], "done")
        self.assertTrue((self.project / "addons" / "didi" / "didi_console.gd").is_file())
        leftovers = [p.name for p in (self.project / "addons").iterdir() if p.name != "didi"]
        self.assertEqual(leftovers, [], "staging and the replaced copy are cleaned up")

    def test_an_addon_from_another_build_is_not_installed(self):
        # The release archive's layout: bin/didi beside addons/didi.
        archive = self.root / "archive"
        (archive / "bin").mkdir(parents=True)
        binary = archive / "bin" / self.binary.name
        shutil.copy2(self.binary, binary)
        shutil.copytree(self.bundled_addon(), archive / "addons" / "didi")
        report = self.setup_json(binary=binary)
        self.assertEqual(self.steps(report)["addon"]["state"], "done")

        shutil.rmtree(self.project / "addons")
        stale = patch_build_id(archive / "addons" / "didi" / "bin" / LIBRARY, "2001")
        report = self.setup_json(binary=binary, expect=1)
        detail = self.steps(report)["addon"]["detail"]
        self.assertIn(stale, detail)
        self.assertFalse((self.project / "addons").exists(), "nothing is installed from a mismatched addon")

    def test_doctor_reports_each_check(self):
        report = self.doctor_json(expect=1)
        steps = self.steps(report)
        self.assertEqual(steps["extension"]["state"], "fail")
        self.assertEqual(steps["plugin"]["state"], "fail")

        self.setup_json("--client", "all")
        report = self.doctor_json(expect=0)
        steps = self.steps(report)
        for step in ("server", "extension", "addon-build", "plugin"):
            self.assertEqual(steps[step]["state"], "ok", steps[step])
        self.assertEqual(steps["bridge"]["state"], "warn", "no editor is open, which is a warning")
        for client in CONFIG_FILES:
            self.assertEqual(steps[f"client:{client}"]["state"], "ok", steps[f"client:{client}"])

        older = patch_build_id(self.project / "addons" / "didi" / "bin" / LIBRARY, "2000")
        steps = self.steps(self.doctor_json(expect=0))
        self.assertEqual(steps["addon-build"]["state"], "warn")
        self.assertIn(older, steps["addon-build"]["detail"])
        self.assertEqual(steps["client:claude-code"]["state"], "warn")
        self.assertIn("stale", steps["client:claude-code"]["detail"])

        other = self.root / "other"
        other.mkdir()
        (other / "project.godot").write_text(BARE_PROJECT, encoding="utf-8")
        config = json.loads((self.project / ".mcp.json").read_text(encoding="utf-8"))
        config["mcpServers"]["didi"]["args"][1] = str(other)
        (self.project / ".mcp.json").write_text(json.dumps(config), encoding="utf-8")
        steps = self.steps(self.doctor_json(expect=1))
        self.assertEqual(steps["client:claude-code"]["state"], "fail")
        self.assertIn("another project", steps["client:claude-code"]["detail"])

    def test_the_command_line_is_refused_when_it_cannot_be_read(self):
        for args, expected in (
            (["setup"], "project root is required"),
            (["setup", "--project", str(self.project), "--client", "notepad"], "unknown client notepad"),
            (["setup", "--project", str(self.project), "--headless"], "--headless"),
            (["setup", "--project", str(self.project), "--timeout", "0"], "--timeout"),
            (["setup", "--project", str(self.root)], "project.godot"),
            (["doctor", "--project", str(self.project), "stray"], "unexpected argument stray"),
        ):
            result = self.run_didi(*args)
            self.assertEqual(result.returncode, 2, (args, result.stdout, result.stderr))
            self.assertIn(expected, result.stderr, args)
        self.assertFalse((self.project / "addons").exists())
        for command in ("setup", "doctor"):
            result = self.run_didi(command, "--help")
            self.assertEqual(result.returncode, 0)
            self.assertIn(f"didi {command}", result.stdout)


# Prints the arguments the dock's Connect page would write, from its own code.
# Outside the editor its settings read as their defaults.
DOCK_ARGUMENTS = """extends SceneTree

func _init() -> void:
\tvar config = load("res://addons/didi/didi_client_config.gd")
\tprint("DIDI_DOCK_ARGS " + JSON.stringify(Array(config.arguments())))
\tquit()
"""


def editors_to_check() -> list[str]:
    value = os.environ.get("DIDI_SETUP_GODOT", "")
    return [path for path in value.split(os.pathsep) if path]


@unittest.skipUnless(editors_to_check(), "the live setup check is opt-in: set DIDI_SETUP_GODOT")
class SetupLive(SetupFixture):
    def stop_editor(self, report: dict):
        editor = self.steps(report).get("editor", {})
        launched = editor.get("launched_pid")
        published = editor.get("editor", {}).get("pid")
        if os.name == "nt":
            for pid in (launched, published):
                if pid:
                    subprocess.run(["taskkill", "/T", "/F", "/PID", str(pid)], capture_output=True)
        else:
            for pid in (published, launched):
                if pid:
                    try:
                        os.kill(pid, signal.SIGKILL)
                    except OSError:
                        pass
        # The engine keeps a copy of a reloadable library open for a moment
        # after it has gone.
        time.sleep(2)

    def test_the_dock_writes_the_arguments_setup_writes(self):
        # The dock's Connect page and didi setup each build the server's
        # arguments, and each names the other (#1148), but a change to one
        # alone still passed CI (#1144). The dock's own code, run by each
        # engine, has to give what setup wrote.
        self.setup_json("--client", "claude-code")
        _, written = configured_server(self.project, "claude-code")
        script = self.root / "dock_args.gd"
        script.write_text(DOCK_ARGUMENTS, encoding="utf-8", newline="\n")
        for godot in editors_to_check():
            with self.subTest(godot=Path(godot).name):
                result = subprocess.run([godot, "--headless", "--path", str(self.project), "--script", str(script)],
                                        capture_output=True, text=True, encoding="utf-8", errors="replace",
                                        timeout=180, env=self.env)
                printed = [line for line in result.stdout.splitlines() if line.startswith("DIDI_DOCK_ARGS ")]
                self.assertTrue(printed, result.stdout + result.stderr)
                dock = json.loads(printed[0][len("DIDI_DOCK_ARGS "):])
                # The project is named as Godot was opened on it, and setup
                # names it by its long path. On a host whose temp folder has
                # an 8.3 name (C:/Users/RUNNER~1 on the CI runners) the two
                # spell one folder differently, so it is compared as a folder
                # and everything else as written.
                dock_root, setup_root = dock[dock.index("--project") + 1], written[written.index("--project") + 1]
                self.assertTrue(os.path.samefile(dock_root, setup_root), (dock_root, setup_root))
                self.assertEqual([a for a in dock if a != dock_root], [a for a in written if a != setup_root])

    def test_one_command_gives_a_working_session(self):
        for godot in editors_to_check():
            with self.subTest(godot=Path(godot).name):
                if (self.project / "addons").exists():
                    shutil.rmtree(self.project / "addons", ignore_errors=True)
                    shutil.rmtree(self.project / ".godot", ignore_errors=True)
                    (self.project / "project.godot").write_text(BARE_PROJECT, encoding="utf-8", newline="\n")
                result = self.run_didi("setup", "--project", str(self.project), "--client", "claude-code",
                                       "--godot", godot, "--headless", "--timeout", "180", "--json", timeout=300)
                report = json.loads(result.stdout)
                self.addCleanup(self.stop_editor, report)
                editor = self.steps(report)["editor"]
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertEqual(editor["state"], "ok", editor)
                self.assertEqual(editor["editor"]["build_id"], report["server"]["build_id"])

                # The server the client would start reaches that editor: setup
                # let go of the session, and the bridge is this build.
                command, args = configured_server(self.project, "claude-code")
                server = subprocess.Popen([command, *args], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                          stderr=subprocess.DEVNULL, text=True, encoding="utf-8", env=self.env)
                try:
                    def send(message):
                        server.stdin.write(json.dumps(message) + "\n")
                        server.stdin.flush()

                    def answer(request_id):
                        while True:
                            line = server.stdout.readline()
                            self.assertTrue(line, "the server exited")
                            message = json.loads(line)
                            if message.get("id") == request_id:
                                return message

                    send({"jsonrpc": "2.0", "id": 1, "method": "initialize",
                          "params": {"protocolVersion": "2025-03-26", "capabilities": {},
                                     "clientInfo": {"name": "setup-live", "version": "1"}}})
                    answer(1)
                    send({"jsonrpc": "2.0", "method": "notifications/initialized"})
                    send({"jsonrpc": "2.0", "id": 2, "method": "tools/call",
                          "params": {"name": "didi_control_room", "arguments": {"log_limit": 0}}})
                    room = answer(2)["result"]["structuredContent"]
                    bridge = next(light for light in room["lights"] if light["label"] == "Bridge")
                    self.assertEqual(bridge["state"], "ok", bridge)

                    # Doctor beside a client's server: the editor is held, and
                    # that is a working bridge rather than a fault.
                    held = self.steps(self.doctor_json(expect=0))["bridge"]
                    self.assertEqual(held["state"], "ok", held)
                finally:
                    server.stdin.close()
                    try:
                        server.wait(timeout=20)
                    except subprocess.TimeoutExpired:
                        server.kill()
                    server.stdout.close()
                answered = self.steps(self.doctor_json(expect=0))["bridge"]
                self.assertEqual(answered["state"], "ok", answered)
                self.assertIn("answers", answered["detail"])

                # A rerun beside the open editor changes nothing and starts no
                # second editor on the same project.
                rerun = self.setup_json("--client", "claude-code", "--godot", godot, "--headless")
                steps = self.steps(rerun)
                for step in ("addon", "plugin", "client:claude-code"):
                    self.assertEqual(steps[step]["state"], "unchanged", steps[step])
                self.assertEqual(steps["editor"]["state"], "ok", steps["editor"])
                self.assertNotIn("launched_pid", steps["editor"])
                self.assertIn("already open", steps["editor"]["detail"])
                self.stop_editor(report)


if __name__ == "__main__":
    unittest.main()
