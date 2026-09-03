"""Command line parsing: a typo must never look like a successful start.

The parser used to be an if/else chain with no final else. An unknown option was
ignored, an out-of-range log level was ignored, and a value-taking option would
happily swallow the following flag. That last one is the dangerous case:
`--log-level --yolo` consumed the flag, so a launch that asked for YOLO mode
started without it and without the warning that says confirmations are off.

Every form below is checked against the real executable, because this is
behaviour of the process at startup and nothing smaller can prove it.
"""

import json
import os
import subprocess
import unittest
from pathlib import Path

REPOSITORY_ROOT = Path(__file__).resolve().parents[1]
FIXTURE_PROJECT = str(REPOSITORY_ROOT / "tests" / "godot_smoke")


def _executable() -> str:
    override = os.environ.get("DIDI_EXECUTABLE")
    if override:
        return override
    # The repository is built into build/ by CI and into build-ninja/ by hand,
    # so both can exist at once. Take the newest rather than a fixed order, or a
    # stale binary from the other directory answers for the one just built.
    candidates = [
        REPOSITORY_ROOT / name
        for name in (
            "build/Release/didi.exe", "build/Debug/didi.exe", "build/didi",
            "build-ninja/didi.exe", "build-ninja/didi",
        )
    ]
    built = [path for path in candidates if path.is_file()]
    if not built:
        raise unittest.SkipTest("didi executable not built")
    return str(max(built, key=lambda path: path.stat().st_mtime))


def _run(arguments):
    return subprocess.run(
        [_executable(), *arguments],
        stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=60,
    )


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

    def test_manifest_still_prints_json_without_a_project(self):
        result = _run(["--dump-tool-manifest"])
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIsInstance(json.loads(result.stdout), dict)


if __name__ == "__main__":
    unittest.main()
