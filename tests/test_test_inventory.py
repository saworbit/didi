"""Tests for the generator behind docs/TEST_INVENTORY.md.

The generator exists so a published test count cannot go stale. That only
holds if the generator itself is right, and every function it depends on is
pure text in, number out -- so they are tested directly rather than through a
build. These run in the lint workflow, with no compiler and no test binary.
"""

from __future__ import annotations

import importlib.util
import sys
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


class BadgeTests(unittest.TestCase):
    def test_badge_rewrites_only_the_marked_region(self):
        text = (
            "# Title\n"
            f"{inventory.BADGE_START}old badge{inventory.BADGE_END}\n"
            "trailing prose\n"
        )
        updated = inventory.apply_badge(text, 1234)
        self.assertIn("badge/tests-1234-", updated)
        self.assertIn("trailing prose", updated)
        self.assertNotIn("old badge", updated)

    def test_badge_is_idempotent(self):
        text = f"{inventory.BADGE_START}x{inventory.BADGE_END}"
        once = inventory.apply_badge(text, 42)
        self.assertEqual(inventory.apply_badge(once, 42), once)

    def test_missing_markers_are_an_error_not_a_silent_skip(self):
        with self.assertRaises(inventory.InventoryError):
            inventory.apply_badge("# README with no markers\n", 7)

    def test_readme_carries_the_markers(self):
        readme = (REPO_ROOT / "README.md").read_text(encoding="utf-8")
        self.assertIn(inventory.BADGE_START, readme)
        self.assertIn(inventory.BADGE_END, readme)


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

    def test_check_is_a_skip_not_a_failure_off_the_reference_platform(self):
        with mock.patch.object(inventory.platform, "system", return_value="Linux"):
            self.assertEqual(inventory.main(["--check"]), 0)

    def test_regeneration_refuses_off_the_reference_platform(self):
        # The dangerous direction: writing another platform's numbers into a
        # page that claims Windows figures would be silent and wrong.
        with mock.patch.object(inventory.platform, "system", return_value="Darwin"):
            self.assertEqual(inventory.main([]), 2)


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
