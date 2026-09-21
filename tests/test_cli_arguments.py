"""Command line parsing: a typo must never look like a successful start.

The parser used to be an if/else chain with no final else. An unknown option was
ignored, an out-of-range log level was ignored, and a value-taking option would
happily swallow the following flag. That last one is the dangerous case:
`--log-level --yolo` consumed the flag, so a launch that asked for YOLO mode
started without it and without the warning that says confirmations are off.

Every form below is checked against the real executable, because this is
behaviour of the process at startup and nothing smaller can prove it.

The test runner had the same shape and it was found later: `didi_tests
--filter Tools.Rename` matched neither branch, left the filter empty, ran all
of them and reported the whole suite's exit code, which reads as one isolated
test passing (#803). Those cases are at the bottom of this file.
"""

import json
import os
import shutil
import subprocess
import tempfile
import threading
import unittest
from pathlib import Path

REPOSITORY_ROOT = Path(__file__).resolve().parents[1]
FIXTURE_PROJECT = str(REPOSITORY_ROOT / "tests" / "godot_smoke")


# One resolver for every test that drives the binary. tests/ is imported two
# ways -- as the top level directory by unittest discover, and as tests.<module>
# by the explicit invocations in CI -- and only one of these resolves at a time.
try:
    import didi_binary as _binary
except ImportError:
    from tests import didi_binary as _binary

_executable = _binary.resolve


def _run(arguments, environment=None):
    return subprocess.run(
        [_executable(), *arguments],
        stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=60,
        env=environment,
    )


# Names that a real user directory can hold. The first three are representable
# in the Windows ANSI codepage and the last three are not, and before #611 the
# two halves failed differently: cp1252 names crashed the process with
# STATUS_STACK_BUFFER_OVERRUN and no output at all, and the rest were mangled
# into question marks and refused as an inaccessible directory.
NON_ASCII_PROJECT_NAMES = [
    "pr\u00f3jekt", "gr\u00fcn", "a\u00f1o",
    "\u043f\u0440\u043e\u0435\u043a\u0442", "\u65e5\u672c", "pro\U0001f600ject",
]


# (name, arguments) that must still be accepted exactly as before.
ACCEPTED = [
    ("version long", ["--version"]),
    ("version short", ["-v"]),
    ("help long", ["--help"]),
    ("help short", ["-h"]),
    ("manifest without a project", ["--dump-tool-manifest"]),
    ("project long", ["--project", FIXTURE_PROJECT]),
    ("project short", ["-p", FIXTURE_PROJECT]),
    ("yolo", ["--project", FIXTURE_PROJECT, "--yolo"]),
    ("pipe name", ["--project", FIXTURE_PROJECT, "--pipe-name", "didi-cli-test"]),
    ("log level DEBUG", ["--project", FIXTURE_PROJECT, "--log-level", "DEBUG"]),
    ("log level INFO", ["--project", FIXTURE_PROJECT, "--log-level", "INFO"]),
    ("log level WARN", ["--project", FIXTURE_PROJECT, "--log-level", "WARN"]),
    ("log level ERROR", ["--project", FIXTURE_PROJECT, "--log-level", "ERROR"]),
    ("log level NONE", ["--project", FIXTURE_PROJECT, "--log-level", "NONE"]),
    ("every option at once",
     ["--project", FIXTURE_PROJECT, "--yolo", "--log-level", "WARN",
      "--pipe-name", "didi-cli-test"]),
]

# (name, arguments, fragment the refusal must name) that must exit 2.
REFUSED = [
    ("misspelled option", ["--project", FIXTURE_PROJECT, "--wat"],
     "unknown option --wat"),
    ("misspelled short option", ["-x"], "unknown option -x"),
    ("option that is only a prefix", ["--proj", FIXTURE_PROJECT],
     "unknown option --proj"),
    ("equals form is not supported", ["--project=" + FIXTURE_PROJECT],
     "unknown option --project="),
    ("log level outside the enum", ["--project", FIXTURE_PROJECT, "--log-level", "VERBOSE"],
     "--log-level expects DEBUG, INFO, WARN, ERROR, or NONE"),
    ("log level in the wrong case", ["--project", FIXTURE_PROJECT, "--log-level", "debug"],
     "--log-level expects DEBUG, INFO, WARN, ERROR, or NONE"),
    ("log level swallows the next flag", ["--log-level", "--yolo", "--project", FIXTURE_PROJECT],
     "--log-level expects a value, but the next argument is the option --yolo"),
    ("project swallows the next flag", ["--project", "--yolo"],
     "--project expects a value, but the next argument is the option --yolo"),
    ("pipe name swallows the next flag", ["--pipe-name", "--yolo"],
     "--pipe-name expects a value, but the next argument is the option --yolo"),
    ("project without a value", ["--project"], "--project expects a value"),
    ("project short without a value", ["-p"], "-p expects a value"),
    ("pipe name without a value", ["--project", FIXTURE_PROJECT, "--pipe-name"],
     "--pipe-name expects a value"),
    ("log level without a value", ["--log-level"], "--log-level expects a value"),
    ("empty project", ["--project", ""], "--project expects a value and was given an empty one"),
    ("empty pipe name", ["--project", FIXTURE_PROJECT, "--pipe-name", ""],
     "--pipe-name expects a value and was given an empty one"),
    ("empty log level", ["--log-level", ""],
     "--log-level expects a value and was given an empty one"),
    ("empty argument", [""], "an empty argument is neither an option nor a value"),
    ("stray argument", ["oops"], "unexpected argument oops"),
    ("stray argument after valid options", ["--project", FIXTURE_PROJECT, "oops"],
     "unexpected argument oops"),
]


# Reading with a deadline, because the failure this guards against is a read
# that never returns. A bare readline() on a hung server hangs the suite too.
def _read_line_within(stream, seconds):
    captured = {}

    def read():
        captured["line"] = stream.readline()

    reader = threading.Thread(target=read, daemon=True)
    reader.start()
    reader.join(seconds)
    return captured.get("line")


class CommandLineTests(unittest.TestCase):
    def test_accepted_forms_start_and_exit_cleanly(self):
        for name, arguments in ACCEPTED:
            with self.subTest(name):
                result = _run(arguments)
                self.assertEqual(result.returncode, 0, result.stderr)

    def test_malformed_forms_are_refused_before_startup(self):
        for name, arguments, fragment in REFUSED:
            with self.subTest(name):
                result = _run(arguments)
                self.assertEqual(result.returncode, 2, result.stdout)
                self.assertIn(fragment, result.stderr)
                # A refusal is not a start. Nothing may have run far enough to
                # announce the server or to print a JSON-RPC line on stdout.
                self.assertNotIn("Starting Didi MCP Native Server", result.stderr)
                self.assertEqual(result.stdout.strip(), "")

    def test_a_refusal_shows_the_help_line_for_the_option(self):
        result = _run(["--project", FIXTURE_PROJECT, "--log-level", "VERBOSE"])
        self.assertIn("--log-level <level>   Set log level (DEBUG, INFO, WARN, ERROR, NONE)",
                      result.stderr)

    def test_debug_logging_answers_a_client_that_ignores_stderr(self):
        # The MCP stdio transport says a server MAY write logs to standard
        # error and a client MAY ignore them. At DEBUG the startup log is one
        # line per registered tool, which is past the pipe buffer, and writing
        # it inline blocked the thread that would have answered initialize: a
        # host that does not drain stderr saw no error, no exit and no output
        # (#689). The log is now written off-thread and lines are dropped
        # rather than the server.
        process = subprocess.Popen(
            [_executable(), "--log-level", "DEBUG", "--project", FIXTURE_PROJECT],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        )
        try:
            request = {
                "jsonrpc": "2.0", "id": 0, "method": "initialize",
                "params": {"protocolVersion": "2024-11-05", "capabilities": {},
                           "clientInfo": {"name": "stderr-ignoring-host", "version": "1"}},
            }
            process.stdin.write(json.dumps(request).encode() + b"\n")
            process.stdin.flush()
            answer = _read_line_within(process.stdout, seconds=30)
            self.assertIsNotNone(answer, "initialize was never answered")
            self.assertEqual(json.loads(answer)["id"], 0)
        finally:
            process.kill()
            process.communicate()

    def test_yolo_survives_a_preceding_value_option(self):
        # The failure that started this: --log-level ate --yolo and the server
        # came up without the mode the operator asked for.
        result = _run(["--project", FIXTURE_PROJECT, "--log-level", "WARN", "--yolo"])
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("YOLO mode", result.stderr)

    def test_help_lists_every_option_the_parser_accepts(self):
        help_text = _run(["--help"]).stdout
        for option in ("--version", "--help", "--project", "--pipe-name",
                       "--log-level", "--dump-tool-manifest", "--yolo"):
            self.assertIn(option, help_text)

    def test_version_prints_the_build_it_came_from_as_well_as_the_release(self):
        # The version cannot tell two builds apart, and the GDExtension is a
        # separate file a user copies around on its own. Before a session is
        # attached this is the only way to record which build a run was handed,
        # which is what field trial 03 needed and did not have.
        result = _run(["--version"])
        self.assertEqual(result.returncode, 0, result.stderr)
        lines = [line.strip() for line in result.stdout.splitlines() if line.strip()]
        self.assertEqual(len(lines), 2, result.stdout)
        self.assertTrue(lines[0].startswith("didi (godot-mcp-native) v"), result.stdout)
        self.assertTrue(lines[1].startswith("build "), result.stdout)
        # Version, commit and configure stamp, so two binaries match only when
        # they were configured together.
        self.assertRegex(lines[1], r"^build \d+\.\d+\.\d+\+\S+\.\d{8}T\d{6}$")

    def test_a_non_ascii_project_root_starts(self):
        # A Windows username with an accent in it is ordinary, and a project
        # under C:/Users/<name>/ inherits it. Both routes to the project root
        # are checked, because both read the same mangled bytes.
        for name in NON_ASCII_PROJECT_NAMES:
            with self.subTest(name.encode("unicode_escape").decode("ascii")):
                base = tempfile.mkdtemp(prefix="didi-cli-")
                try:
                    root = os.path.join(base, name)
                    os.mkdir(root)
                    shutil.copy(os.path.join(FIXTURE_PROJECT, "project.godot"), root)

                    argument = _run(["--project", root])
                    self.assertEqual(argument.returncode, 0, argument.stderr)

                    environment = dict(os.environ)
                    environment["DIDI_PROJECT_ROOT"] = root
                    inherited = _run([], environment=environment)
                    self.assertEqual(inherited.returncode, 0, inherited.stderr)
                finally:
                    shutil.rmtree(base, ignore_errors=True)

    def test_manifest_still_prints_json_without_a_project(self):
        result = _run(["--dump-tool-manifest"])
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIsInstance(json.loads(result.stdout), dict)


# The test binary, not the server. DIDI_TEST_BINARY deliberately is not read
# here: it names the server for every Python suite in this directory and the
# test binary for tools/test_inventory.py, and honouring it here would make a
# third meaning out of the same variable.
NATIVE_RUNNER_CANDIDATES = (
    "build/Release/didi_tests.exe",
    "build/Debug/didi_tests.exe",
    "build/didi_tests",
    "build-ninja/didi_tests.exe",
    "build-ninja/didi_tests",
)


def _native_runner():
    built = [
        path
        for path in (REPOSITORY_ROOT / name for name in NATIVE_RUNNER_CANDIDATES)
        if path.is_file()
    ]
    if not built:
        raise unittest.SkipTest("didi_tests not built")
    built.sort(key=lambda path: path.stat().st_mtime, reverse=True)
    return built[0]


class NativeRunnerArguments(unittest.TestCase):
    """The same rule, one binary along.

    The runner's own comment says why a single test is run alone: the suite
    shares process-global state, so running one by itself is how you tell a
    genuine failure from a leak an earlier test left behind. The space form
    silently handed back exactly the shared-state run that reasoning avoids,
    and looked like it had done what was asked.
    """

    def _run(self, arguments, timeout=180):
        return subprocess.run(
            [str(_native_runner()), *arguments],
            stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=timeout,
        )

    def test_the_space_form_of_filter_is_refused_by_name(self):
        result = self._run(["--filter", "Tools.RenameRefusals"])

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("unrecognised argument '--filter'", result.stderr)
        # Both accepted forms, so the refusal is a remedy and not just a no.
        self.assertIn("--filter=<substring>", result.stderr)
        self.assertIn("--list", result.stderr)
        # And it refused instead of running the suite, which is the whole point.
        self.assertNotIn("Running Didi Native MCP Test Suite", result.stdout)

    def test_a_bare_test_name_is_refused(self):
        result = self._run(["Tools.RenameRefusals"])

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("unrecognised argument 'Tools.RenameRefusals'", result.stderr)

    def test_the_documented_filter_form_runs_one_test(self):
        result = self._run(["--filter=Tools.RenameRefusals"])

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("Filter: Tools.RenameRefusals", result.stdout)
        # One, not all of them. The count is what told the two runs apart.
        self.assertIn("1 total", result.stdout)

    def test_list_still_prints_every_name(self):
        result = self._run(["--list"])

        self.assertEqual(result.returncode, 0, result.stderr)
        names = [line for line in result.stdout.splitlines() if line.strip()]
        self.assertGreater(len(names), 100)
        self.assertIn("Tools.RenameRefusals", names)


if __name__ == "__main__":
    unittest.main()
