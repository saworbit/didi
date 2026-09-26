"""Which build the end to end tests drive.

This repository is built into build/ by CI and into build-ninja/ by hand, so a
developer machine usually has both and one of them is old. The resolver used to
try build/ first by name, which meant a local run could drive a binary from days
ago and report failures that read as real regressions. These pin the rule that
replaced it, because the failure mode is silent: a stale binary answers every
request perfectly well, it just answers as the wrong version.
"""

from __future__ import annotations

import os
import tempfile
import unittest
from pathlib import Path

try:
    import didi_binary
except ImportError:
    from tests import didi_binary


class BinaryResolution(unittest.TestCase):
    def setUp(self):
        self._real_root = didi_binary.REPOSITORY_ROOT
        self._real_announced = didi_binary._announced
        self._temporary = tempfile.TemporaryDirectory()
        self.root = Path(self._temporary.name)
        didi_binary.REPOSITORY_ROOT = self.root
        # Quiet here, and put back afterwards: leaving it set would suppress the
        # announcement for whatever module runs next in this process.
        didi_binary._announced = True
        for name in ("DIDI_TEST_BINARY", "DIDI_EXECUTABLE"):
            self.addCleanup(self._restore_environment, name, os.environ.get(name))
            os.environ.pop(name, None)

    def tearDown(self):
        didi_binary.REPOSITORY_ROOT = self._real_root
        didi_binary._announced = self._real_announced
        self._temporary.cleanup()

    @staticmethod
    def _restore_environment(name, value):
        if value is None:
            os.environ.pop(name, None)
        else:
            os.environ[name] = value

    def _build(self, relative, mtime):
        """A candidate binary with an exact modification time.

        Set rather than inferred from creation order: two files written back to
        back can land on the same timestamp, and a test that depends on which
        one the clock happened to favour is a test that fails on someone else's
        machine for no reason.
        """
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(b"not really a binary")
        os.utime(path, (mtime, mtime))
        return path

    def _resolve_without_skipping(self):
        # resolve() skips when it finds nothing, and a skip inside a test
        # reports the test as skipped. For the two tests below that would turn
        # the regression they guard into the silent pass they are about.
        try:
            return didi_binary.resolve()
        except unittest.SkipTest as skipped:
            self.fail(f"resolve() skipped: {skipped}")

    def test_the_newest_build_wins_over_directory_order(self):
        # build/ comes first in the candidate list and is the older tree, which
        # is exactly the arrangement that used to drive a stale binary.
        self._build("build/Release/didi.exe", 1_000_000)
        fresh = self._build("build-ninja/didi.exe", 2_000_000)
        self.assertEqual(didi_binary.resolve(), fresh)

    def test_the_newest_build_wins_the_other_way_round_too(self):
        # And the rule is the timestamp, not a preference for build-ninja.
        fresh = self._build("build/Release/didi.exe", 2_000_000)
        self._build("build-ninja/didi.exe", 1_000_000)
        self.assertEqual(didi_binary.resolve(), fresh)

    def test_a_single_build_is_returned_unchanged(self):
        only = self._build("build/didi", 1_000_000)
        self.assertEqual(didi_binary.resolve(), only)

    def test_the_override_wins_even_over_a_newer_build(self):
        # CI sets this to the binary it just built and depends on being obeyed.
        stale = self._build("build/Release/didi.exe", 1_000_000)
        self._build("build-ninja/didi.exe", 2_000_000)
        os.environ["DIDI_TEST_BINARY"] = str(stale)
        self.assertEqual(didi_binary.resolve(), stale)

    def test_the_command_line_spelling_of_the_override_still_works(self):
        chosen = self._build("build/didi", 1_000_000)
        os.environ["DIDI_EXECUTABLE"] = str(chosen)
        self.assertEqual(didi_binary.resolve(), chosen)

    def test_an_override_naming_nothing_is_an_error_not_a_search(self):
        # Falling back to the search would drive a different binary than the one
        # that was asked for, which is the confusion this whole thing is about.
        self._build("build/didi", 1_000_000)
        os.environ["DIDI_TEST_BINARY"] = str(self.root / "build" / "absent" / "didi")
        with self.assertRaises(RuntimeError) as raised:
            didi_binary.resolve()
        self.assertIn("not a file", str(raised.exception))

    def test_no_build_at_all_still_skips(self):
        with self.assertRaises(unittest.SkipTest):
            didi_binary.resolve()

    def test_the_layout_ci_builds_on_windows_is_found(self):
        # Ninja puts didi.exe in the build root. This path was missing, so every
        # suite that searched skipped on the Windows leg and its step passed.
        built = self._build("build/didi.exe", 1_000_000)
        self.assertEqual(self._resolve_without_skipping(), built)

    def test_a_configured_tree_with_no_binary_is_an_error_not_a_skip(self):
        # A skip reads as green. A tree that was configured and holds no didi
        # anywhere this looks is a broken build or an out of date list, and
        # either way the suite did not run.
        cache = self.root / "build" / "CMakeCache.txt"
        cache.parent.mkdir(parents=True)
        cache.write_text("", encoding="utf-8")
        with self.assertRaises(RuntimeError) as raised:
            self._resolve_without_skipping()
        self.assertIn("build", str(raised.exception))
        self.assertIn("DIDI_TEST_BINARY", str(raised.exception))

    def test_the_test_binary_is_refused_by_name(self):
        # The variable names the test binary for tools/test_inventory.py, so it
        # gets set to it. The suites used to start it and fail with a JSON
        # decode error per test that named neither (#846).
        for name in ("didi_tests", "didi_tests.exe"):
            with self.subTest(name=name):
                os.environ["DIDI_TEST_BINARY"] = str(self._build(f"build/{name}", 1_000_000))
                with self.assertRaises(RuntimeError) as raised:
                    didi_binary.resolve()
                message = str(raised.exception)
                self.assertIn("DIDI_TEST_BINARY", message)
                self.assertIn("test_inventory", message)


if __name__ == "__main__":
    unittest.main()
