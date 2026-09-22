"""Tests that every list naming the fuzz targets says the same thing.

The set of fuzz targets is written down five times and nothing joined them up:

* the `.cc` files in `fuzz/`, which is what a target physically is
* `DIDI_FUZZ_TARGETS` in `CMakeLists.txt`, which decides what gets built
* the `target:` matrix in `.github/workflows/fuzz.yml`, which decides what
  gets run
* the table in `fuzz/README.md`, which tells a reader what is covered
* the seed corpus directories under `fuzz/corpus/`, which the workflow finds
  by stripping `fuzz_` off the target name

Every disagreement between them is quiet, and one of them is silent by
construction. `fuzz.yml` copies the committed seeds in with

    seed_dir="fuzz/corpus/${TARGET#fuzz_}"
    cp -n "$seed_dir"/* ".fuzz-corpus/${TARGET}/" 2>/dev/null || true

so a corpus directory that is missing or misnamed costs the run its seeds and
says nothing. The README's claim that the eight bytes which used to segfault
the frame decoder are re-executed on every run would quietly stop being true.

A target in `CMakeLists.txt` and not in the matrix is the same shape: it
compiles on every pull request, which is what the build list is for, and is
never fuzzed, which is what the matrix is for. `fuzz.yml` names that exact
failure in its own header, and #862 was it happening to the Phase 7 harness.

The lists are deliberately explicit rather than globbed, which `fuzz/README.md`
says and this test does not argue with. Explicit keeps them readable. It does
nothing about drift, which is what this is for.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

REPOSITORY_ROOT = Path(__file__).resolve().parents[1]
FUZZ_DIRECTORY = REPOSITORY_ROOT / "fuzz"
CMAKELISTS = REPOSITORY_ROOT / "CMakeLists.txt"
WORKFLOW = REPOSITORY_ROOT / ".github" / "workflows" / "fuzz.yml"
FUZZ_README = FUZZ_DIRECTORY / "README.md"
SECURITY = REPOSITORY_ROOT / "SECURITY.md"

# Only as far as anyone is going to add targets. A sixth wants a word here.
COUNT_WORDS = {1: "one", 2: "two", 3: "three", 4: "four", 5: "five"}


def _source_files() -> set[str]:
    """Target names taken from the files that define them."""

    return {path.stem for path in FUZZ_DIRECTORY.glob("fuzz_*.cc")}


def _cmake_targets() -> set[str]:
    text = CMAKELISTS.read_text(encoding="utf-8")
    match = re.search(r"set\(DIDI_FUZZ_TARGETS\s*(.*?)\)", text, re.DOTALL)
    if match is None:
        raise AssertionError(
            "CMakeLists.txt no longer sets DIDI_FUZZ_TARGETS. If the build "
            "list moved, move this test with it rather than deleting it."
        )
    return set(match.group(1).split())


def _workflow_matrix() -> set[str]:
    text = WORKFLOW.read_text(encoding="utf-8")
    match = re.search(r"^\s*target:\s*\[(.*?)\]\s*$", text, re.MULTILINE)
    if match is None:
        raise AssertionError(
            f"{WORKFLOW.name} no longer carries a literal target matrix. If it "
            "now derives one, this test should compare against whatever it "
            "derives it from rather than be deleted."
        )
    return {entry.strip() for entry in match.group(1).split(",") if entry.strip()}


def _readme_table() -> set[str]:
    text = FUZZ_README.read_text(encoding="utf-8")
    return set(re.findall(r"^\|\s*`(fuzz_\w+)`\s*\|", text, re.MULTILINE))


def _corpus_directories() -> set[str]:
    corpus_root = FUZZ_DIRECTORY / "corpus"
    return {path.name for path in corpus_root.iterdir() if path.is_dir()}


class FuzzTargetListsAgree(unittest.TestCase):
    def setUp(self) -> None:
        self.files = _source_files()
        self.assertTrue(self.files, "fuzz/ has no fuzz_*.cc files at all")

    def test_cmake_builds_every_target_that_exists(self) -> None:
        self.assertEqual(
            _cmake_targets(),
            self.files,
            "DIDI_FUZZ_TARGETS in CMakeLists.txt and the fuzz_*.cc files in "
            "fuzz/ disagree. A file missing from the list is never compiled, "
            "and a list entry with no file fails the build.",
        )

    def test_the_workflow_runs_every_target_that_is_built(self) -> None:
        self.assertEqual(
            _workflow_matrix(),
            self.files,
            "The target matrix in .github/workflows/fuzz.yml and the "
            "fuzz_*.cc files in fuzz/ disagree. A target missing from the "
            "matrix still compiles on every pull request and is fuzzed by "
            "nothing, which is the case fuzz.yml's own header calls worse "
            "than having no target at all.",
        )

    def test_every_target_has_a_seed_corpus(self) -> None:
        # The workflow strips the prefix to find the directory, so the names
        # are not equal and cannot simply be compared.
        expected = {name[len("fuzz_"):] for name in self.files}
        self.assertEqual(
            _corpus_directories(),
            expected,
            "fuzz/corpus/ and the fuzz targets disagree. The workflow builds "
            "the seed path as fuzz/corpus/${TARGET#fuzz_} and copies it with "
            "errors discarded, so a directory that is missing or misnamed "
            "costs the run its committed seeds without failing it.",
        )

    def test_no_seed_corpus_is_empty(self) -> None:
        corpus_root = FUZZ_DIRECTORY / "corpus"
        for name in sorted(_corpus_directories()):
            with self.subTest(corpus=name):
                seeds = [path for path in (corpus_root / name).iterdir() if path.is_file()]
                self.assertTrue(
                    seeds,
                    f"fuzz/corpus/{name} holds no seeds. The copy into the "
                    "run discards its errors, so an empty directory is the "
                    "same as a missing one and reads as coverage either way.",
                )

    def test_the_readme_table_lists_every_target(self) -> None:
        self.assertEqual(
            _readme_table(),
            self.files,
            "The table in fuzz/README.md and the fuzz targets disagree. That "
            "table is what a reader checks to find out what is covered.",
        )

    def test_security_md_states_the_right_number_of_decoders(self) -> None:
        row = next(
            (line for line in SECURITY.read_text(encoding="utf-8").splitlines()
             if line.startswith("| [Fuzzing]")),
            None,
        )
        self.assertIsNotNone(
            row,
            "SECURITY.md no longer has a Fuzzing row. It is the page that "
            "tells a reader what covers the untrusted input path, so if the "
            "row moved, move this with it.",
        )
        expected = COUNT_WORDS.get(len(self.files))
        self.assertIsNotNone(
            expected, f"no count word for {len(self.files)} targets; add one above"
        )
        self.assertIn(
            f"the {expected} decoders",
            row,
            f"SECURITY.md's Fuzzing row does not say 'the {expected} decoders' "
            f"and there are {len(self.files)} fuzz targets. #882 was this "
            "sentence naming a decoder nothing called; the count goes stale "
            "the same way.",
        )


if __name__ == "__main__":
    unittest.main()
