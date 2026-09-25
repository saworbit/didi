"""The weekly read of the code scanning tab (#807).

Offline: the API is replaced with the alerts it answered on the two days that
mattered, so these pin what the report says and when the job goes red, not
what GitHub happens to hold today.
"""

from __future__ import annotations

import contextlib
import importlib.util
import io
import os
import sys
import unittest
from pathlib import Path
from unittest import mock

REPO_ROOT = Path(__file__).resolve().parent.parent


def _load_watch():
    # By path, the way tests/test_vendored_versions.py loads its tool: tools/
    # is a directory of standalone scripts, not a package.
    module_path = REPO_ROOT / "tools" / "check_code_scanning.py"
    spec = importlib.util.spec_from_file_location("didi_code_scanning_check", module_path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


watch = _load_watch()


def alert(number, rule, path, line, *, tool="CodeQL", severity="high", created="2026-09-18",
          dismissed_at=None, reason=None):
    return {
        "number": number,
        "tool": {"name": tool},
        "rule": {"id": rule, "security_severity_level": severity},
        "most_recent_instance": {"location": {"path": path, "start_line": line}},
        "created_at": f"{created}T00:00:00Z",
        "dismissed_at": dismissed_at and f"{dismissed_at}T00:00:00Z",
        "dismissed_reason": reason,
        "html_url": f"https://github.com/o/r/security/code-scanning/{number}",
    }


PROCESS = "cpp/uncontrolled-process-operation"
DISMISSED = [
    alert(18, PROCESS, "src/offline/process_runner.cpp", 239,
          dismissed_at="2026-09-09", reason="won't fix"),
    alert(19, PROCESS, "src/offline/test_runner.cpp", 662,
          dismissed_at="2026-09-09", reason="won't fix"),
    alert(15, "cpp/path-injection", "src/runtime/session_client.cpp", 866,
          dismissed_at="2026-09-09", reason="won't fix"),
]
# What was open on 2026-09-20: both process operations back under new numbers
# after the execvp lines moved, and a Scorecard finding nobody had triaged.
OPEN = [
    alert(29, PROCESS, "src/offline/process_runner.cpp", 265, created="2026-09-19"),
    alert(28, PROCESS, "src/offline/test_runner.cpp", 856),
    alert(21, "PinnedDependenciesID", "tools/localci/Dockerfile", 51,
          tool="Scorecard", severity="medium", created="2026-09-15"),
]


class Findings(unittest.TestCase):
    def test_a_re_raise_names_the_dismissal_it_repeats(self):
        entries = {e["number"]: e for e in watch.findings(OPEN, DISMISSED)}
        self.assertEqual(entries[28]["repeats"]["number"], 19)
        self.assertEqual(entries[29]["repeats"]["number"], 18)
        self.assertEqual(entries[28]["repeats"]["dismissed_reason"], "won't fix")
        self.assertNotIn("repeats", entries[21])

    def test_the_line_is_not_compared_because_it_moving_is_the_cause(self):
        # 662 against 856: a different line, the same finding.
        entries = watch.findings([OPEN[1]], DISMISSED)
        self.assertEqual(entries[0]["repeats"]["number"], 19)

    def test_another_rule_or_file_is_not_a_repeat(self):
        other_file = alert(30, PROCESS, "src/offline/gdscript_diagnostics.cpp", 649)
        other_rule = alert(31, "cpp/path-injection", "src/offline/test_runner.cpp", 856)
        entries = watch.findings([other_file, other_rule], DISMISSED)
        self.assertTrue(all("repeats" not in e for e in entries))

    def test_the_newest_dismissal_is_the_one_named(self):
        older = alert(19, PROCESS, "src/offline/test_runner.cpp", 662,
                      dismissed_at="2026-09-09", reason="won't fix")
        newer = alert(28, PROCESS, "src/offline/test_runner.cpp", 856,
                      dismissed_at="2026-09-20", reason="won't fix")
        entries = watch.findings([alert(40, PROCESS, "src/offline/test_runner.cpp", 860)],
                                 [older, newer])
        self.assertEqual(entries[0]["repeats"]["number"], 28)


class Report(unittest.TestCase):
    def test_repeats_are_listed_apart_and_say_what_to_do(self):
        text = watch.render(watch.findings(OPEN, DISMISSED))
        self.assertIn("Open code scanning alerts: 3", text)
        fresh, repeated = text.split("Raised again after a dismissal")
        self.assertIn("#21 Scorecard PinnedDependenciesID", fresh)
        self.assertNotIn("#28", fresh)
        self.assertIn("repeats #19, dismissed 2026-09-09 as won't fix", repeated)
        self.assertIn("SECURITY.md", repeated)

    def test_a_clean_tab_says_so(self):
        self.assertEqual(watch.render([]), "No code scanning alerts are open.\n")


class ExitStatus(unittest.TestCase):
    def run_main(self, open_alerts, *argv, env=None, error=None):
        def fetch(repo, state, token):
            if error:
                raise watch.ReadError(error)
            return open_alerts if state == "open" else DISMISSED

        environment = {"GITHUB_TOKEN": "t", "GITHUB_REPOSITORY": "o/r"} if env is None else env
        out, err = io.StringIO(), io.StringIO()
        with mock.patch.object(watch, "fetch_alerts", fetch), \
                mock.patch.dict(os.environ, environment, clear=True), \
                contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            status = watch.main(list(argv))
        return status, out.getvalue(), err.getvalue()

    def test_check_is_red_while_anything_is_open(self):
        self.assertEqual(self.run_main(OPEN, "--check")[0], 1)
        self.assertEqual(self.run_main([], "--check")[0], 0)

    def test_without_check_a_report_is_not_a_failure(self):
        self.assertEqual(self.run_main(OPEN)[0], 0)

    def test_a_tab_that_cannot_be_read_is_never_reported_clean(self):
        status, out, err = self.run_main(OPEN, "--check", error="answered 403.")
        self.assertEqual(status, 2)
        self.assertEqual(out, "")
        self.assertIn("403", err)

    def test_no_token_is_a_refusal_that_names_what_it_needs(self):
        status, _, err = self.run_main(OPEN, "--check", env={"GITHUB_REPOSITORY": "o/r"})
        self.assertEqual(status, 2)
        self.assertIn("GITHUB_TOKEN", err)
        self.assertIn("security-events", err)


if __name__ == "__main__":
    unittest.main()
