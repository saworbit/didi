"""Tests for tools/check_release_archive.py.

The editor half of the tool needs a real Godot and is exercised by running it
against a release. Everything that is files in, verdict out is tested here,
without an engine and without the network: the layout rules an archive is held
to, unpacking, the version checks, and which lines of engine output count as
findings.
"""

from __future__ import annotations

import importlib.util
import io
import sys
import tarfile
import tempfile
import unittest
import zipfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent


def _load_checker():
    """Import tools/check_release_archive.py by path; `tools/` is not a package."""

    module_path = REPO_ROOT / "tools" / "check_release_archive.py"
    spec = importlib.util.spec_from_file_location("didi_release_archive_check", module_path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


checker = _load_checker()

ADDON = ["addons/didi/didi.gdextension", "addons/didi/plugin.cfg", "addons/didi/didi_plugin.gd"]


def build_tree(root: Path, platform: str = "windows", version: str = "2.0.1") -> Path:
    """A complete unpacked archive for *platform*, as the release workflow stages it."""

    server, library = checker.PLATFORMS[platform]
    files = {
        "README.md": f"# Didi {version} for Somewhere\n",
        "LICENSE": "MIT License\n",
        "THIRD_PARTY_NOTICES.txt": "notices\n",
        f"bin/{server}": "binary",
        "bin/didi_class_reference.json": "{}",
        f"addons/didi/bin/{library}": "library",
        "addons/didi/didi.gdextension": "[configuration]\n",
        "addons/didi/plugin.cfg": f'[plugin]\nversion="{version}"\n',
        "addons/didi/didi_plugin.gd": "extends EditorPlugin\n",
    }
    for relative, content in files.items():
        path = root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content, encoding="utf-8")
    return root


class LayoutTests(unittest.TestCase):
    def setUp(self) -> None:
        self.scratch = tempfile.TemporaryDirectory()
        self.root = build_tree(Path(self.scratch.name) / "didi-windows-x64")

    def tearDown(self) -> None:
        self.scratch.cleanup()

    def test_accepts_exactly_the_promised_files(self):
        self.assertEqual(checker.check_layout(self.root, "windows", ADDON), [])

    def test_names_a_file_that_is_missing(self):
        (self.root / "THIRD_PARTY_NOTICES.txt").unlink()
        self.assertEqual(checker.check_layout(self.root, "windows", ADDON),
                         ["missing: THIRD_PARTY_NOTICES.txt"])

    def test_names_a_file_that_should_not_be_there(self):
        (self.root / "addons" / "didi" / "didi_plugin.gd.uid").write_text("uid://x", encoding="utf-8")
        self.assertEqual(checker.check_layout(self.root, "windows", ADDON),
                         ["unexpected: addons/didi/didi_plugin.gd.uid"])

    def test_names_a_file_that_is_empty(self):
        (self.root / "bin" / "didi_class_reference.json").write_text("", encoding="utf-8")
        self.assertEqual(checker.check_layout(self.root, "windows", ADDON),
                         ["empty: bin/didi_class_reference.json"])

    def test_detects_each_platform_from_the_library_it_carries(self):
        for platform in checker.PLATFORMS:
            with self.subTest(platform=platform), tempfile.TemporaryDirectory() as scratch:
                root = build_tree(Path(scratch) / "archive", platform)
                self.assertEqual(checker.detect_platform(root), platform)

    def test_the_tracked_addon_includes_its_manifest(self):
        tracked = checker.tracked_addon_files()
        self.assertIn("addons/didi/plugin.cfg", tracked)
        self.assertIn("addons/didi/didi.gdextension", tracked)


class UnpackTests(unittest.TestCase):
    def test_unpacks_a_zip_and_a_tarball_to_their_one_folder(self):
        with tempfile.TemporaryDirectory() as scratch:
            base = Path(scratch)
            tree = build_tree(base / "staged" / "didi-linux-x64", "linux")
            zipped = base / "didi.zip"
            with zipfile.ZipFile(zipped, "w") as bundle:
                for path in tree.rglob("*"):
                    bundle.write(path, path.relative_to(tree.parent).as_posix())
            tarred = base / "didi.tar.gz"
            with tarfile.open(tarred, "w:gz") as bundle:
                bundle.add(tree, arcname=tree.name)
            for archive in (zipped, tarred):
                with self.subTest(archive=archive.name):
                    destination = base / f"out-{archive.stem}"
                    destination.mkdir()
                    root = checker.unpack(archive, destination)
                    self.assertEqual(root.name, "didi-linux-x64")
                    self.assertEqual(checker.check_layout(root, "linux", ADDON), [])

    def test_refuses_an_archive_that_would_scatter_files(self):
        with tempfile.TemporaryDirectory() as scratch:
            base = Path(scratch)
            archive = base / "loose.zip"
            with zipfile.ZipFile(archive, "w") as bundle:
                bundle.writestr("README.md", "readme")
                bundle.writestr("bin/didi.exe", "binary")
            destination = base / "out"
            destination.mkdir()
            with self.assertRaisesRegex(ValueError, "one top-level folder"):
                checker.unpack(archive, destination)


    def test_refuses_a_tarball_that_reaches_outside_the_destination(self):
        with tempfile.TemporaryDirectory() as scratch:
            base = Path(scratch)
            archive = base / "escape.tar.gz"
            with tarfile.open(archive, "w:gz") as bundle:
                for name in ("didi-linux-x64/README.md", "didi-linux-x64/../../escaped.txt"):
                    data = b"x"
                    info = tarfile.TarInfo(name)
                    info.size = len(data)
                    bundle.addfile(info, io.BytesIO(data))
            destination = base / "deep" / "out"
            destination.mkdir(parents=True)
            with self.assertRaisesRegex(ValueError, "outside the destination"):
                checker.unpack(archive, destination)
            self.assertFalse((base / "escaped.txt").exists())
            self.assertFalse((base / "deep" / "escaped.txt").exists())

    def test_refuses_a_link_inside_a_tarball(self):
        with tempfile.TemporaryDirectory() as scratch:
            base = Path(scratch)
            archive = base / "link.tar.gz"
            with tarfile.open(archive, "w:gz") as bundle:
                info = tarfile.TarInfo("didi-linux-x64/bin/didi")
                info.type = tarfile.SYMTYPE
                info.linkname = "/usr/bin/env"
                bundle.addfile(info)
            destination = base / "out"
            destination.mkdir()
            with self.assertRaisesRegex(ValueError, "not a regular file or folder"):
                checker.unpack(archive, destination)


class VersionTests(unittest.TestCase):
    def test_reads_the_version_the_server_prints(self):
        output = "didi (godot-mcp-native) v2.0.1\nbuild 2.0.1+8244df5006f9.20260923T084156\n"
        self.assertEqual(checker.parse_version(output), "2.0.1")
        self.assertEqual(checker.parse_version("something else\n"), "")
        self.assertEqual(checker.parse_version(""), "")

    def test_requires_the_addon_and_readme_to_agree_with_the_server(self):
        with tempfile.TemporaryDirectory() as scratch:
            root = build_tree(Path(scratch) / "archive", version="2.0.0")
            self.assertEqual(checker.check_version(root, "2.0.0", None), [])
            findings = checker.check_version(root, "2.0.1", "2.0.1")
            self.assertEqual(len(findings), 2)
            self.assertIn("plugin.cfg", findings[0])
            self.assertIn("README.md", findings[1])
            self.assertEqual(checker.check_version(root, "2.0.0", "2.0.1"),
                             ["the server reports 2.0.0, expected 2.0.1"])


class EngineOutputTests(unittest.TestCase):
    def test_flags_what_the_engine_complains_about_and_nothing_else(self):
        output = io.StringIO(
            "Godot Engine v4.5.1.stable.official - https://godotengine.org\n"
            "ERROR: Attempt to get non-existent interface function: 'classdb_register_extension_class6'.\n"
            "[Didi] Didi Native MCP Editor Plugin active.\n"
            "[  50% ] _update_scan_actions | didi_signature.svg\n"
            "WARNING: res://addons/didi/didi_console.gd:12 - unused variable\n"
            "SCRIPT ERROR: Parse Error: Identifier not declared.\n"
            "Can't open dynamic library: res://addons/didi/bin/libdidi_extension.dylib\n"
            "No GDExtension library found for current OS and architecture (macos.x86_64)\n"
        )
        problems = checker.problem_lines(output)
        self.assertEqual(len(problems), 5)
        self.assertTrue(problems[0].startswith("ERROR: Attempt to get non-existent"))
        self.assertFalse(any("Plugin active" in line for line in problems))


if __name__ == "__main__":
    unittest.main()
