"""Tests for the generator behind docs/TEST_INVENTORY.md.

The generator exists so a published test count cannot go stale. That only
holds if the generator itself is right, and every function it depends on is
pure text in, number out -- so they are tested directly rather than through a
build. These run in the lint workflow, with no compiler and no test binary.
"""

from __future__ import annotations

import contextlib
import importlib.util
import io
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

REPO_ROOT = Path(__file__).resolve().parent.parent


def _load_generator():
    """Import tools/test_inventory.py by path.

    `tools/` is not a package, and adding one would put an `__init__.py` in a
    directory of standalone scripts purely to satisfy an import here.
    """

    module_path = REPO_ROOT / "tools" / "test_inventory.py"
    spec = importlib.util.spec_from_file_location("didi_test_inventory", module_path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


inventory = _load_generator()


class PythonTestCountingTests(unittest.TestCase):
    def test_counts_test_methods_on_a_test_case(self):
        source = (
            "import unittest\n"
            "class Example(unittest.TestCase):\n"
            "    def test_one(self):\n"
            "        pass\n"
            "    def test_two(self):\n"
            "        pass\n"
        )
        self.assertEqual(inventory.count_python_tests(source), 2)

    def test_ignores_helpers_and_non_test_classes(self):
        # A helper named `build_fixture` is not a test, and a plain class that
        # happens to define `test_x` is not a test case. Counting either would
        # inflate the published number, which is the exact failure this tool
        # exists to prevent.
        source = (
            "import unittest\n"
            "class Helper:\n"
            "    def test_not_collected(self):\n"
            "        pass\n"
            "class Example(unittest.TestCase):\n"
            "    def build_fixture(self):\n"
            "        pass\n"
            "    def test_real(self):\n"
            "        pass\n"
        )
        self.assertEqual(inventory.count_python_tests(source), 1)

    def test_counts_async_tests_and_bare_testcase_bases(self):
        source = (
            "from unittest import IsolatedAsyncioTestCase, TestCase\n"
            "class Bare(TestCase):\n"
            "    def test_one(self):\n"
            "        pass\n"
            "class Async(IsolatedAsyncioTestCase):\n"
            "    async def test_two(self):\n"
            "        pass\n"
        )
        self.assertEqual(inventory.count_python_tests(source), 2)

    def test_resolves_a_base_class_defined_in_another_module(self):
        # tests/test_managed_recovery_adversarial.py does exactly this, and the
        # first version of the generator counted it as zero -- a whole file of
        # tests missing from a published total, silently.
        source = (
            "import test_managed_recovery_live as live\n"
            "class Adversarial(live.ManagedRecoveryLive):\n"
            "    def test_one(self):\n"
            "        pass\n"
        )
        self.assertEqual(inventory.count_python_tests(source), 0)
        self.assertEqual(
            inventory.count_python_tests(source, frozenset({"ManagedRecoveryLive"})),
            1,
        )

    def test_resolves_inheritance_within_a_module_to_a_fixed_point(self):
        source = (
            "import unittest\n"
            "class Base(unittest.TestCase):\n"
            "    def test_base(self):\n"
            "        pass\n"
            "class Middle(Base):\n"
            "    def test_middle(self):\n"
            "        pass\n"
            "class Leaf(Middle):\n"
            "    def test_leaf(self):\n"
            "        pass\n"
        )
        self.assertEqual(inventory.count_python_tests(source), 3)

    def test_every_committed_python_test_module_is_counted(self):
        # A module whose tests all fail to be recognised would silently drop
        # out of the total. The suite as committed must contribute.
        suite = inventory.python_suite(REPO_ROOT)
        self.assertGreater(suite.total, 0)
        counted = {group.name for group in suite.groups}
        on_disk = {path.name for path in (REPO_ROOT / "tests").glob("test_*.py")}
        self.assertEqual(
            on_disk - counted,
            set(),
            "these test modules contribute nothing to the published count",
        )


class PowerShellAssertionCountingTests(unittest.TestCase):
    def test_counts_call_sites_but_not_the_definition(self):
        source = (
            "function Assert-True([bool]$Condition, [string]$Message) {\n"
            "    if (-not $Condition) { throw $Message }\n"
            "}\n"
            "Assert-True ($a -eq $b) 'first'\n"
            "Assert-True ($c -eq $d) 'second'\n"
        )
        self.assertEqual(inventory.count_powershell_assertions(source), 2)

    def test_ignores_commented_out_assertions(self):
        source = "# Assert-True $false 'disabled'\nAssert-True $true 'live'\n"
        self.assertEqual(inventory.count_powershell_assertions(source), 1)

    def test_live_harness_contributes_assertions(self):
        suite = inventory.powershell_suite(REPO_ROOT)
        self.assertEqual(suite.unit, "assertions")
        self.assertGreater(suite.total, 0)


class NativeGroupingTests(unittest.TestCase):
    def test_dotted_names_group_by_their_suite(self):
        self.assertEqual(inventory.native_suite_prefix("Tools.DefaultRegistration"), "Tools")

    def test_sentence_names_group_by_their_first_word(self):
        # A few native cases were registered as prose rather than
        # `Suite.Case`. They still belong somewhere legible.
        self.assertEqual(
            inventory.native_suite_prefix("Phase7Signals partial delivery contract"),
            "Phase7Signals",
        )
        self.assertEqual(inventory.native_suite_prefix("phase7 generated schemas"), "phase7")


BADGE_LINE = (
    "[![Tests](https://img.shields.io/badge/tests-804-2ea043"
    "?logo=pytest&logoColor=white)](docs/TEST_INVENTORY.md)"
)


class BadgeTests(unittest.TestCase):
    def test_badge_rewrites_only_the_count(self):
        text = f"# Title\n{BADGE_LINE}\ntrailing prose\n"
        updated = inventory.apply_badge(text, 1234)
        self.assertIn("badge/tests-1234-2ea043", updated)
        self.assertNotIn("tests-804", updated)
        # The surrounding markup, including the link target and the query
        # string, is left exactly as it was.
        self.assertIn("?logo=pytest&logoColor=white)](docs/TEST_INVENTORY.md)", updated)
        self.assertIn("trailing prose", updated)

    def test_badge_is_idempotent(self):
        once = inventory.apply_badge(BADGE_LINE, 42)
        self.assertEqual(inventory.apply_badge(once, 42), once)

    def test_a_missing_badge_is_an_error_not_a_silent_skip(self):
        with self.assertRaises(inventory.InventoryError):
            inventory.apply_badge("# README with no badge\n", 7)

    def test_two_badges_are_refused_rather_than_guessed_between(self):
        # One would go stale with nothing to notice.
        with self.assertRaises(inventory.InventoryError):
            inventory.apply_badge(f"{BADGE_LINE}\n{BADGE_LINE}\n", 7)

    def test_readme_carries_exactly_one_tests_badge(self):
        readme = (REPO_ROOT / "README.md").read_text(encoding="utf-8")
        self.assertEqual(len(inventory.BADGE_PATTERN.findall(readme)), 1)

    def test_no_badge_line_starts_with_an_html_comment(self):
        """The regression that shipped: markers stopped the badge rendering.

        A line beginning with `<!--` opens a raw HTML block in CommonMark, so
        everything on it is emitted literally instead of parsed as Markdown.
        The tests badge was wrapped in `<!-- test-count:start -->` markers and
        GitHub rendered the whole line as visible text, splitting the badge row
        into two paragraphs around it.
        """

        readme = (REPO_ROOT / "README.md").read_text(encoding="utf-8")
        offenders = [
            line
            for line in readme.splitlines()
            if line.lstrip().startswith("<!--") and "img.shields.io" in line
        ]
        self.assertEqual(
            offenders,
            [],
            "a badge on a line starting with an HTML comment renders as literal text",
        )


class ReferencePlatformTests(unittest.TestCase):
    """The page publishes one platform's native figures and must say so.

    The first version of this tool did not, and the macOS runner rejected the
    committed Windows count within one CI run: crash capture is Windows-only,
    and the IPC cases differ because a named pipe and a Unix socket are not the
    same transport.
    """

    def test_the_page_names_its_platform(self):
        page = (REPO_ROOT / "docs" / "TEST_INVENTORY.md").read_text(encoding="utf-8")
        self.assertIn(f"**These are the {inventory.REFERENCE_PLATFORM} figures.**", page)
        self.assertIn("platform-conditional", page)

    def test_rendering_always_names_the_platform(self):
        rendered = inventory.render_inventory(
            [inventory.Suite(name="Native", unit="tests", total=1, how="a registry.")]
        )
        self.assertIn(inventory.REFERENCE_PLATFORM, rendered)

    def _run(self, argv, system):
        """Run main() as though on *system*, returning (code, stdout, stderr).

        The environment is stripped of DIDI_TEST_BINARY and the working
        directory moved away from any build tree, because the point of these
        tests is that the platform is decided *before* anything looks for a
        binary. An earlier version checked only the exit code, and passed on a
        machine with no build for entirely the wrong reason -- the missing
        binary also returns 2 -- then failed on the lint runner, which has no
        build and needs the skip.
        """

        out, err = io.StringIO(), io.StringIO()
        environment = {k: v for k, v in os.environ.items() if k != "DIDI_TEST_BINARY"}
        with tempfile.TemporaryDirectory() as empty:
            previous = os.getcwd()
            os.chdir(empty)
            try:
                # BINARY_CANDIDATES resolves against REPO_ROOT, which is
                # absolute, so changing directory alone would not hide a local
                # build. Empty it, and the "no binary anywhere" condition the
                # lint runner is actually in gets reproduced on any machine.
                with mock.patch.object(inventory.platform, "system", return_value=system), \
                        mock.patch.object(inventory, "BINARY_CANDIDATES", ()), \
                        mock.patch.dict(os.environ, environment, clear=True), \
                        contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
                    code = inventory.main(argv)
            finally:
                os.chdir(previous)
        return code, out.getvalue(), err.getvalue()

    def test_check_is_a_skip_not_a_failure_off_the_reference_platform(self):
        for system in ("Linux", "Darwin"):
            with self.subTest(system=system):
                code, out, _ = self._run(["--check"], system)
                self.assertEqual(code, 0)
                self.assertIn(f"skipped on {system}", out)
                self.assertIn(inventory.REFERENCE_PLATFORM, out)

    def test_regeneration_refuses_off_the_reference_platform(self):
        # The dangerous direction: writing another platform's numbers into a
        # page that claims Windows figures would be silent and wrong. Assert
        # the reason, not just the exit code -- a missing build also returns 2.
        code, _, err = self._run([], "Darwin")
        self.assertEqual(code, 2)
        self.assertIn("refusing to regenerate on Darwin", err)

    def test_the_platform_decision_precedes_looking_for_a_binary(self):
        # The regression that reached CI: the gate sat after binary
        # resolution, so on a machine with no build the answer was "no
        # didi_tests binary found" rather than "not this platform".
        _, out, err = self._run(["--check"], "Linux")
        self.assertNotIn("No didi_tests binary found", out + err)


class RenderingTests(unittest.TestCase):
    def _suites(self):
        return [
            inventory.Suite(
                name="Native",
                unit="tests",
                total=3,
                how="a registry.",
                groups=[inventory.Group("Alpha", 2), inventory.Group("Beta", 1)],
            ),
            inventory.Suite(
                name="Harness",
                unit="assertions",
                total=9,
                how="call sites.",
                groups=[inventory.Group("run.ps1", 9)],
            ),
        ]

    def test_totals_separate_tests_from_assertions(self):
        rendered = inventory.render_inventory(self._suites())
        self.assertIn("| Automated tests | **3** |", rendered)
        self.assertIn("| Live-harness assertions | 9 |", rendered)

    def test_rendering_is_deterministic(self):
        first = inventory.render_inventory(self._suites())
        second = inventory.render_inventory(self._suites())
        self.assertEqual(first, second)

    def test_committed_inventory_is_generated_output(self):
        # Not a count check -- that needs a build, and CI does it after one.
        # This catches the page being hand-edited into something the generator
        # would never produce, which is how a generated file stops being one.
        page = (REPO_ROOT / "docs" / "TEST_INVENTORY.md").read_text(encoding="utf-8")
        self.assertTrue(page.startswith("# Test Inventory\n"))
        self.assertIn("**Generated file. Do not edit by hand.**", page)
        self.assertIn("| Automated tests | **", page)
        self.assertTrue(page.endswith("\n"))


if __name__ == "__main__":
    unittest.main()
