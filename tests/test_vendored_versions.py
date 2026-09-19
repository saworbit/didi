"""Tests for the watcher behind the vendored sources in THIRD_PARTY.md.

The watcher exists because three copied headers have no manifest and so no
bot. That only helps if the watcher itself is right, so every part of it that
is text in, verdict out is tested directly. Nothing here touches the network:
the upstream lookup is a callable, and these substitute their own.

The last case is the one that guards the repository rather than the tool -- it
runs the real offline check over the real tree and requires it to be clean.
"""

from __future__ import annotations

import importlib.util
import sys
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent


def _load_checker():
    """Import tools/check_vendored_versions.py by path.

    `tools/` is not a package, and adding one would put an `__init__.py` in a
    directory of standalone scripts purely to satisfy an import here.
    """

    module_path = REPO_ROOT / "tools" / "check_vendored_versions.py"
    spec = importlib.util.spec_from_file_location("didi_vendored_check", module_path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


checker = _load_checker()

BANNER = "/* stb_image_write - v1.16 - public domain */\n"

TABLE = """\
# Third Party Code

Some prose that sits above the table and must not shift the parse.

| File | Upstream | Version in tree | License |
| :--- | :--- | :--- | :--- |
| `include/didi/common/json.hpp` | [nlohmann/json](x) | 3.11.3 | MIT |
| `include/didi/common/stb_image_write.h` | [nothings/stb](x) | v1.16 | Public domain |
"""


def _source(**overrides):
    """A tracked source pointing at the stb banner, with fields overridable."""
    defaults = dict(
        path="include/didi/common/stb_image_write.h",
        file_pattern=r"stb_image_write - v(?P<version>\d+(?:\.\d+)*)",
        upstream=lambda: "1.16",
    )
    defaults.update(overrides)
    return checker.VendoredSource(**defaults)


class TableParsingTests(unittest.TestCase):
    def test_reads_the_version_column_for_each_backticked_path(self):
        table = checker.parse_third_party_table(TABLE)
        self.assertEqual(table["include/didi/common/json.hpp"], "3.11.3")
        self.assertEqual(table["include/didi/common/stb_image_write.h"], "v1.16")

    def test_ignores_the_header_and_separator_rows(self):
        table = checker.parse_third_party_table(TABLE)
        self.assertEqual(len(table), 2)
        self.assertNotIn("File", table)

    def test_ignores_prose_and_rows_without_a_backticked_path(self):
        table = checker.parse_third_party_table("| plain | text | 1.0 | MIT |\nnot a row\n")
        self.assertEqual(table, {})


class VersionNormalisationTests(unittest.TestCase):
    def test_strips_a_leading_v(self):
        self.assertEqual(checker.normalise("v1.16"), "1.16")

    def test_leaves_a_bare_version_alone(self):
        self.assertEqual(checker.normalise("3.11.3"), "3.11.3")

    def test_returns_prose_unchanged_so_a_comparison_fails_loudly(self):
        self.assertEqual(checker.normalise("Godot 4.7 era"), "Godot 4.7 era")

    def test_version_tuple_orders_numerically_not_lexically(self):
        # The bug this guards: "3.9.0" > "3.11.0" as strings.
        self.assertLess(checker.version_tuple("3.9.0"), checker.version_tuple("3.11.0"))

    def test_version_tuple_rejects_prose(self):
        self.assertIsNone(checker.version_tuple("Godot 4.7 era"))


class FileVersionTests(unittest.TestCase):
    def test_reads_the_banner_out_of_the_file(self):
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            path = root / "include" / "didi" / "common" / "stb_image_write.h"
            path.parent.mkdir(parents=True)
            path.write_text(BANNER, encoding="utf-8")
            self.assertEqual(checker.read_file_version(_source(), root), "1.16")

    def test_returns_none_when_no_banner_matches(self):
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            path = root / "include" / "didi" / "common" / "stb_image_write.h"
            path.parent.mkdir(parents=True)
            path.write_text("no banner here\n", encoding="utf-8")
            self.assertIsNone(checker.read_file_version(_source(), root))


class CheckSourceTests(unittest.TestCase):
    def setUp(self):
        self._temp = tempfile.TemporaryDirectory()
        self.root = Path(self._temp.name)
        path = self.root / "include" / "didi" / "common" / "stb_image_write.h"
        path.parent.mkdir(parents=True)
        path.write_text(BANNER, encoding="utf-8")
        self.table = checker.parse_third_party_table(TABLE)
        self.addCleanup(self._temp.cleanup)

    def _kinds(self, report):
        return [finding.kind for finding in report.findings]

    def test_matching_file_table_and_upstream_produce_no_findings(self):
        report = checker.check_source(_source(), self.table, self.root, offline=False)
        self.assertEqual(report.findings, [])
        self.assertEqual(report.file_version, "1.16")

    def test_table_disagreeing_with_the_file_is_a_finding(self):
        table = dict(self.table)
        table["include/didi/common/stb_image_write.h"] = "v1.15"
        report = checker.check_source(_source(), table, self.root, offline=True)
        self.assertEqual(self._kinds(report), ["table-drift"])

    def test_v_prefix_alone_is_not_drift(self):
        table = dict(self.table)
        table["include/didi/common/stb_image_write.h"] = "1.16"
        report = checker.check_source(_source(), table, self.root, offline=True)
        self.assertEqual(report.findings, [])

    def test_a_newer_upstream_is_a_finding(self):
        source = _source(upstream=lambda: "v1.17")
        report = checker.check_source(source, self.table, self.root, offline=False)
        self.assertEqual(self._kinds(report), ["behind"])
        self.assertIn("1.17", report.findings[0].detail)

    def test_offline_never_consults_upstream(self):
        def explode():
            raise AssertionError("upstream must not be called with --offline")

        report = checker.check_source(
            _source(upstream=explode), self.table, self.root, offline=True
        )
        self.assertEqual(report.findings, [])
        self.assertIsNone(report.upstream_version)

    def test_an_unreachable_upstream_reports_rather_than_raises(self):
        def fail():
            raise checker.VersionLookupError("api.github.com: rate limited")

        report = checker.check_source(
            _source(upstream=fail), self.table, self.root, offline=False
        )
        self.assertEqual(self._kinds(report), ["lookup-failed"])

    def test_a_file_missing_from_the_tree_is_a_finding(self):
        source = _source(path="include/didi/common/json.hpp")
        report = checker.check_source(source, self.table, self.root, offline=True)
        self.assertEqual(self._kinds(report), ["missing"])

    def test_a_file_with_no_table_row_is_a_finding(self):
        report = checker.check_source(_source(), {}, self.root, offline=True)
        self.assertEqual(self._kinds(report), ["untabled"])

    def test_an_unreadable_banner_is_a_finding(self):
        source = _source(file_pattern=r"nothing matches (?P<version>\d+)")
        report = checker.check_source(source, self.table, self.root, offline=True)
        self.assertEqual(self._kinds(report), ["unreadable"])

    def test_an_untracked_source_is_reported_not_checked(self):
        source = checker.VendoredSource(
            path="include/didi/common/json.hpp",
            file_pattern="",
            untracked_reason="a compatibility contract",
        )
        report = checker.check_source(source, self.table, self.root, offline=True)
        self.assertFalse(report.tracked)
        self.assertEqual(report.findings, [])
        self.assertEqual(report.note, "a compatibility contract")


class RepositoryContractTests(unittest.TestCase):
    """The tool is only useful if the tree it watches actually passes."""

    def test_every_vendored_file_in_the_table_exists(self):
        for source in checker.VENDORED_SOURCES:
            with self.subTest(source=source.path):
                self.assertTrue((REPO_ROOT / source.path).is_file())

    def test_third_party_table_matches_the_files_on_disk(self):
        reports = checker.run(root=REPO_ROOT, offline=True)
        findings = [
            f"[{finding.kind}] {finding.source}: {finding.detail}"
            for report in reports
            for finding in report.findings
        ]
        self.assertEqual(findings, [], "\n".join(findings))

    def test_every_untracked_source_says_why(self):
        for source in checker.VENDORED_SOURCES:
            if not source.file_pattern:
                with self.subTest(source=source.path):
                    self.assertTrue(source.untracked_reason.strip())


if __name__ == "__main__":
    unittest.main()
