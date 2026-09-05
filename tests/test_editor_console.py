"""The editor console's addon, checked without needing Godot.

The console is GDScript, and CI has no engine to run it in. What CI can still
prove is the set of things that break the addon without any script executing at
all: a file that exists but is not packaged, a preload pointing at nothing, a
brand asset that drifted from the brand, and a session token reaching a panel
that has no business holding one. Each of those has a single place it would go
wrong, and each is checked here.

The engine-side behaviour -- that the plugin loads, the mark rasterises, and the
bridge closes when asked -- was verified against Godot 4.5.1, 4.6.2 and 4.7.2 by
running the addon in each. That is not something a Python test can stand in for,
and this file does not pretend to.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ADDON = ROOT / "addons" / "didi"
DEMO_ADDON = ROOT / "demo" / "addons" / "didi"

# Generated into whichever project a tool is run against, so it is not part of
# what the addon ships. tools/validate_documentation.py exempts the same name.
GENERATED_IN_PLACE = {"test_lab_sandbox.tscn"}


def addon_files() -> list[str]:
    return sorted(
        entry.name
        for entry in ADDON.iterdir()
        if entry.is_file() and entry.name not in GENERATED_IN_PLACE
    )


class AddonPackagingTest(unittest.TestCase):
    """Three lists name the addon's files. They have to agree."""

    def test_cmake_manifest_names_every_addon_file(self) -> None:
        cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
        block = re.search(
            r"set\(DIDI_ADDON_MANIFEST_FILES(.*?)\n\)", cmake, re.DOTALL
        )
        self.assertIsNotNone(block, "DIDI_ADDON_MANIFEST_FILES is gone from CMakeLists.txt")
        listed = set(re.findall(r"addons/didi/([^\"\s]+)", block.group(1)))
        self.assertEqual(
            set(addon_files()),
            listed,
            "addons/didi and DIDI_ADDON_MANIFEST_FILES disagree. A file missing from "
            "the manifest is not staged, not installed, and not in the release "
            "archive, so the addon ships without it and its console cannot open.",
        )

    def test_ci_expects_every_addon_file_in_the_staged_directory(self) -> None:
        workflow = (ROOT / ".github" / "workflows" / "ci.yml").read_text(encoding="utf-8")
        line = re.search(r'expected="\$library ([^"]*)"', workflow)
        self.assertIsNotNone(line, "The staged addon check no longer builds an expected list")
        expected = set(line.group(1).split())
        self.assertEqual(
            set(addon_files()),
            expected,
            "The staged-addon check in ci.yml lists different files from addons/didi. "
            "It compares against a sorted directory listing, so a stale list fails "
            "the build for the wrong reason or passes an incomplete addon.",
        )

    def test_ci_expected_list_is_sorted_the_way_it_is_compared(self) -> None:
        workflow = (ROOT / ".github" / "workflows" / "ci.yml").read_text(encoding="utf-8")
        line = re.search(r'expected="\$library ([^"]*)"', workflow)
        assert line is not None
        names = line.group(1).split()
        self.assertEqual(
            names,
            sorted(names),
            "The check compares against `LC_ALL=C sort` output, so the expected list "
            "has to be in that order to ever match.",
        )

    def test_demo_project_carries_the_same_addon(self) -> None:
        for name in addon_files():
            with self.subTest(name=name):
                demo = DEMO_ADDON / name
                self.assertTrue(demo.is_file(), f"demo/addons/didi/{name} is missing")
                self.assertEqual(
                    (ADDON / name).read_bytes().replace(b"\r\n", b"\n"),
                    demo.read_bytes().replace(b"\r\n", b"\n"),
                    f"demo/addons/didi/{name} has drifted from the canonical addon",
                )


class AddonSourceTest(unittest.TestCase):
    def test_every_preloaded_path_exists(self) -> None:
        pattern = re.compile(r'preload\("(res://[^"]+)"\)')
        for script in sorted(ADDON.glob("*.gd")):
            for target in pattern.findall(script.read_text(encoding="utf-8")):
                with self.subTest(script=script.name, target=target):
                    self.assertTrue(
                        (ROOT / target.removeprefix("res://")).is_file()
                        or (ADDON / Path(target).name).is_file(),
                        f"{script.name} preloads {target}, which is not in the addon",
                    )

    def test_the_console_never_reads_a_session_token(self) -> None:
        """A descriptor on disk carries the secret that authenticates a client.

        The console reads those descriptors to report bridge state. It has no
        use for the token: not to show, not to copy, not to put in a report. The
        reader copies the fields it names and drops the rest, and this is what
        keeps someone from adding the token to that list later without noticing
        what it means.
        """
        for script in sorted(ADDON.glob("*.gd")):
            text = script.read_text(encoding="utf-8")
            # The word appears in prose explaining the rule; a quoted "token"
            # would be a field being read out of a descriptor.
            self.assertNotIn(
                '"token"',
                text,
                f"{script.name} names the descriptor token field. The console must "
                "never carry a session secret.",
            )

    def test_scripts_are_tool_scripts(self) -> None:
        """Editor scripts that are not @tool do nothing in the editor."""
        for script in sorted(ADDON.glob("*.gd")):
            with self.subTest(script=script.name):
                self.assertTrue(
                    script.read_text(encoding="utf-8").startswith("@tool\n"),
                    f"{script.name} is loaded by an EditorPlugin and must be @tool",
                )


class BrandAssetTest(unittest.TestCase):
    """The addon ships the brand's own files, not lookalikes."""

    SOURCES = {
        "didi_mark.svg": "didi-mark.svg",
        "didi_mark_compact.svg": "didi-mark-compact.svg",
        "didi_signature.svg": "didi-signature.svg",
    }

    def test_addon_marks_match_the_brand_sources(self) -> None:
        brand = ROOT / "docs" / "brand" / "svg"
        for shipped, source in self.SOURCES.items():
            with self.subTest(asset=shipped):
                self.assertEqual(
                    (ADDON / shipped).read_bytes().replace(b"\r\n", b"\n"),
                    (brand / source).read_bytes().replace(b"\r\n", b"\n"),
                    f"addons/didi/{shipped} has drifted from docs/brand/svg/{source}. "
                    "Brand geometry is generated from one place; a hand-edited copy "
                    "in the addon is a second mark.",
                )

    def test_shipped_marks_are_single_colour_sources(self) -> None:
        """The console recolours the mark for the editor's theme.

        It does that by substituting `currentColor`, so an asset without one is
        an asset that renders in whatever colour it was authored in and
        disappears against half the editor themes.
        """
        for shipped in self.SOURCES:
            with self.subTest(asset=shipped):
                self.assertIn("currentColor", (ADDON / shipped).read_text(encoding="utf-8"))


if __name__ == "__main__":
    unittest.main()
