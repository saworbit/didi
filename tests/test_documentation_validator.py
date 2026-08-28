import importlib.util
from pathlib import Path
import tempfile
import unittest


REPOSITORY_ROOT = Path(__file__).resolve().parents[1]
VALIDATOR_PATH = REPOSITORY_ROOT / "tools" / "validate_documentation.py"
SPEC = importlib.util.spec_from_file_location("validate_documentation", VALIDATOR_PATH)
if SPEC is None or SPEC.loader is None:
    raise ImportError(f"Cannot load documentation validator from {VALIDATOR_PATH}")
VALIDATOR = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(VALIDATOR)


class DocumentationValidatorTests(unittest.TestCase):
    def setUp(self):
        self.temporary_directory = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary_directory.name)

    def tearDown(self):
        self.temporary_directory.cleanup()

    def write(self, relative_path: str, text: str) -> None:
        path = self.root / relative_path
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")

    def make_valid_repository(self) -> Path:
        self.write("LICENSE", "MIT License\n")
        self.write("CMakeLists.txt", "project(didi VERSION 1.4.0 LANGUAGES C CXX)\n")
        self.write(
            "include/didi/mcp/mcp_protocol.hpp",
            'inline const char* kServerVersion = "1.4.0";\n',
        )
        self.write(
            "src/standalone/main.cpp",
            'std::cout << "didi (godot-mcp-native) v1.4.0";\n',
        )
        self.write("addons/didi/plugin.cfg", 'version="1.4.0"\n')

        self.write(
            "README.md",
            """# Didi

Current documented release: **1.4.0**.

Didi exposes 72 canonical tools plus 10 legacy names (82 total).

[Guide](docs/GUIDE.md#details-1) | [Security](SECURITY.md) | [Overview](#didi)
""",
        )
        self.write(
            "CHANGELOG.md",
            """# Changelog

## [Unreleased]

## [1.4.0] - 2026-08-28

Version 1.4.0 exposes 72 canonical tools, 10 legacy registrations, and 82 total registrations. Fifty-four canonical tools are implemented and 18 remain unimplemented.
""",
        )
        self.write(
            "SECURITY.md",
            """# Security Policy

Current release: 1.4.0.

| Version | Supported |
| --- | --- |
| 1.4.x | :white_check_mark: |
| 1.3.x | :x: |
| <=1.2.x | :x: |
""",
        )
        self.write("CONTRIBUTING.md", "# Contributing\n")

        generic_docs = (
            "ADMIN_GUIDE.md",
            "API_SPECIFICATION.md",
            "ARCHITECTURE.md",
            "DEVELOPER_GUIDE.md",
            "INTEGRATION_GUIDE.md",
            "LLM_INSTRUCTIONS.md",
            "QUICKSTART.md",
            "RESOURCES_AND_PROMPTS.md",
            "ROADMAP.md",
        )
        for name in generic_docs:
            self.write(f"docs/{name}", f"# {name.removesuffix('.md').replace('_', ' ').title()}\n")

        self.write(
            "docs/CAPABILITIES.md",
            """# Current Capability Matrix

Didi v1.4.0 registers 72 canonical tool names. Fifty-four are implemented in at least one mode; 18 remain reserved. Ten legacy names are registered separately, for exactly 82 tools/list entries.
""",
        )
        self.write(
            "docs/TOOL_REFERENCE.md",
            """# Tool Reference

Didi exposes 72 canonical tool names plus 10 legacy names (82 registrations).
""",
        )
        self.write(
            "docs/GUIDE.md",
            """# Guide

## Details

First section.

## Details

Second section.

```markdown
[ignored](missing.md#also-missing)
```

`[also ignored](missing-inline.md)`

[External](https://example.com/docs#anchor)
""",
        )
        return self.root

    def test_valid_repository_has_no_errors(self):
        self.assertEqual(VALIDATOR.validate_repository(self.make_valid_repository()), [])

    def test_reports_mismatched_version_source(self):
        root = self.make_valid_repository()
        self.write("addons/didi/plugin.cfg", 'version="1.2.0"\n')

        errors = VALIDATOR.validate_repository(root)

        self.assertTrue(
            any("addons/didi/plugin.cfg" in error and "1.4.0" in error for error in errors),
            errors,
        )

    def test_reports_missing_license(self):
        root = self.make_valid_repository()
        (root / "LICENSE").unlink()

        errors = VALIDATOR.validate_repository(root)

        self.assertTrue(any("LICENSE: required file is missing" in error for error in errors), errors)

    def test_rejects_superpowers_artifacts(self):
        root = self.make_valid_repository()
        self.write(".superpowers/internal-report.md", "# Internal report\n")
        self.write("docs/superpowers/internal-plan.md", "# Internal plan\n")

        errors = VALIDATOR.validate_repository(root)

        self.assertTrue(any(".superpowers" in error for error in errors), errors)
        self.assertTrue(any("docs/superpowers" in error for error in errors), errors)

    def test_rejects_workflow_reference_to_superpowers(self):
        root = self.make_valid_repository()
        self.write(
            ".github/workflows/ci.yml",
            "run: cat docs/superpowers/internal-plan.md\n",
        )

        errors = VALIDATOR.validate_repository(root)

        self.assertTrue(
            any(".github/workflows/ci.yml" in error and "Superpowers" in error for error in errors),
            errors,
        )

    def test_rejects_pinned_cmake_setup_on_windows_release_runner(self):
        root = self.make_valid_repository()
        self.write(
            ".github/workflows/release.yml",
            """jobs:
  package:
    strategy:
      matrix:
        os: [windows-latest, ubuntu-latest]
    runs-on: ${{ matrix.os }}
    steps:
      - name: Setup CMake
        uses: jwlawson/actions-setup-cmake@v2
        with:
          cmake-version: '3.28.x'
""",
        )

        errors = VALIDATOR.validate_repository(root)

        self.assertTrue(
            any("release.yml" in error and "Windows runner CMake" in error for error in errors),
            errors,
        )

    def test_rejects_actions_that_still_target_deprecated_node_20(self):
        root = self.make_valid_repository()
        self.write(
            ".github/workflows/ci.yml",
            """steps:
  - uses: actions/checkout@v4
  - uses: actions/upload-artifact@v4
  - uses: actions/download-artifact@v4
  - uses: softprops/action-gh-release@v2
  - uses: hendrikmuhs/ccache-action@v1.2
""",
        )

        errors = VALIDATOR.validate_repository(root)

        for action in (
            "actions/checkout",
            "actions/upload-artifact",
            "actions/download-artifact",
            "softprops/action-gh-release",
            "hendrikmuhs/ccache-action",
        ):
            self.assertTrue(
                any(action in error and "Node 20" in error for error in errors),
                (action, errors),
            )

    def test_requires_macos_aws_tap_cleanup_before_ccache(self):
        root = self.make_valid_repository()
        workflows = {
            "missing cleanup": """jobs:
  build:
    strategy:
      matrix:
        os: [macos-latest]
    runs-on: ${{ matrix.os }}
    steps:
      - uses: hendrikmuhs/ccache-action@v1.2.23
""",
            "cleanup after ccache": """jobs:
  build:
    strategy:
      matrix:
        os: [macos-latest]
    runs-on: ${{ matrix.os }}
    steps:
      - uses: hendrikmuhs/ccache-action@v1.2.23
      - name: Remove unused Homebrew tap
        if: runner.os == 'macOS'
        run: |
          if brew tap | grep -qx 'aws/tap'; then
            brew untap aws/tap
          fi
""",
            "cleanup without presence check": """jobs:
  build:
    strategy:
      matrix:
        os: [macos-latest]
    runs-on: ${{ matrix.os }}
    steps:
      - name: Remove unused Homebrew tap
        if: runner.os == 'macOS'
        run: brew untap aws/tap
      - uses: hendrikmuhs/ccache-action@v1.2.23
""",
            "cleanup without macOS guard": """jobs:
  build:
    strategy:
      matrix:
        os: [macos-latest]
    runs-on: ${{ matrix.os }}
    steps:
      - name: Remove unused Homebrew tap
        run: |
          if brew tap | grep -qx 'aws/tap'; then
            brew untap aws/tap
          fi
      - uses: hendrikmuhs/ccache-action@v1.2.23
""",
            "cleanup in a different job": """jobs:
  cleanup:
    runs-on: macos-latest
    steps:
      - name: Remove unused Homebrew tap
        if: runner.os == 'macOS'
        run: |
          if brew tap | grep -qx 'aws/tap'; then
            brew untap aws/tap
          fi
  build:
    runs-on: macos-latest
    steps:
      - uses: hendrikmuhs/ccache-action@v1.2.23
""",
            "only first ccache is guarded": """jobs:
  build:
    runs-on: macos-latest
    steps:
      - name: Remove unused Homebrew tap
        if: runner.os == 'macOS'
        run: |
          if brew tap | grep -qx 'aws/tap'; then
            brew untap aws/tap
          fi
      - uses: hendrikmuhs/ccache-action@v1.2.23
      - name: Separate cache setup
        run: echo separate
      - uses: hendrikmuhs/ccache-action@v1.2.23
""",
            "id-first ccache step": """jobs:
  build:
    runs-on: macos-latest
    steps:
      - id: compiler-cache
        uses: hendrikmuhs/ccache-action@v1.2.23
""",
            "disconnected presence check": """jobs:
  build:
    runs-on: macos-latest
    steps:
      - name: Remove unused Homebrew tap
        if: runner.os == 'macOS'
        run: |
          brew tap | grep -qx 'aws/tap' || true
          brew untap aws/tap
      - uses: hendrikmuhs/ccache-action@v1.2.23
""",
            "concrete macOS runner label": """jobs:
  build:
    runs-on: macos-26
    steps:
      - uses: hendrikmuhs/ccache-action@v1.2.23
""",
            "quoted job id": """jobs:
  'build':
    runs-on: macos-latest
    steps:
      - uses: hendrikmuhs/ccache-action@v1.2.23
""",
            "dedented comment before step": """jobs:
  build:
    runs-on: macos-latest
    steps:
# The runner image carries an unused tap.
      - uses: hendrikmuhs/ccache-action@v1.2.23
""",
            "bare dash step item": """jobs:
  build:
    runs-on: macos-latest
    steps:
      -
        id: compiler-cache
        uses: hendrikmuhs/ccache-action@v1.2.23
""",
            "cleanup text only in env": """jobs:
  build:
    runs-on: macos-latest
    steps:
      - name: Pretend to remove unused Homebrew tap
        if: runner.os == 'macOS'
        env:
          UNUSED_SCRIPT: |
            if brew tap | grep -qx 'aws/tap'; then
              brew untap aws/tap
            fi
        run: echo no-cleanup
      - uses: hendrikmuhs/ccache-action@v1.2.23
""",
        }

        for scenario, workflow in workflows.items():
            with self.subTest(scenario=scenario):
                self.write(".github/workflows/ci.yml", workflow)
                errors = VALIDATOR.validate_repository(root)
                self.assertTrue(
                    any("aws/tap" in error and "ccache" in error for error in errors),
                    errors,
                )

    def test_accepts_macos_aws_tap_cleanup_before_ccache(self):
        root = self.make_valid_repository()
        workflows = {
            "shorthand guard": """jobs:
  build:
    strategy:
      matrix:
        os: [macos-latest]
    runs-on: ${{ matrix.os }}
    steps:
      - name: Remove unused Homebrew tap
        if: runner.os == 'macOS'
        run: |
          if brew tap | grep -qx 'aws/tap'; then
            brew untap aws/tap
          fi
      - uses: hendrikmuhs/ccache-action@v1.2.23
""",
            "expression guard and id-first ccache": """jobs:
  build:
    runs-on: macos-26
    steps:
      - name: Remove unused Homebrew tap
        if: ${{ runner.os == 'macOS' }}
        run: |
          if brew tap | grep -qx 'aws/tap'; then
            brew untap aws/tap
          fi
      - id: compiler-cache
        uses: hendrikmuhs/ccache-action@v1.2.23
""",
            "Ubuntu step only mentions macOS": """jobs:
  build:
    runs-on: ubuntu-latest
    steps:
      - name: Cache compiler; macos-latest uses another job
        uses: hendrikmuhs/ccache-action@v1.2.23
""",
            "macOS step only echoes action name": """jobs:
  build:
    runs-on: macos-latest
    steps:
      - name: Explain cache action
        run: echo hendrikmuhs/ccache-action
""",
        }

        for scenario, workflow in workflows.items():
            with self.subTest(scenario=scenario):
                self.write(".github/workflows/ci.yml", workflow)
                self.assertEqual(VALIDATOR.validate_repository(root), [])

    def test_rejects_stale_supported_minor(self):
        root = self.make_valid_repository()
        security = (root / "SECURITY.md").read_text(encoding="utf-8")
        self.write(
            "SECURITY.md",
            security.replace("| 1.3.x | :x: |", "| 1.3.x | :white_check_mark: |"),
        )

        errors = VALIDATOR.validate_repository(root)

        self.assertTrue(any("unsupported release 1.3.x" in error for error in errors), errors)

    def test_reports_missing_current_release_fact(self):
        root = self.make_valid_repository()
        readme = (root / "README.md").read_text(encoding="utf-8")
        self.write("README.md", readme.replace("72 canonical", "71 canonical"))

        errors = VALIDATOR.validate_repository(root)

        self.assertTrue(any("README.md" in error and "72 canonical" in error for error in errors), errors)

    def test_reports_missing_markdown_target(self):
        root = self.make_valid_repository()
        self.write("README.md", "# Didi\n\n[Missing](docs/NOPE.md)\n")

        errors = VALIDATOR.validate_repository(root)

        self.assertTrue(any("missing target" in error for error in errors), errors)

    def test_reports_missing_markdown_anchor(self):
        root = self.make_valid_repository()
        readme = (root / "README.md").read_text(encoding="utf-8")
        self.write(
            "README.md",
            readme.replace("docs/GUIDE.md#details-1", "docs/GUIDE.md#missing"),
        )

        errors = VALIDATOR.validate_repository(root)

        self.assertTrue(any("missing anchor" in error for error in errors), errors)

    def test_validates_links_when_repository_parent_is_a_hidden_worktree(self):
        self.root = self.root / ".worktrees" / "documentation-branch"
        root = self.make_valid_repository()
        readme = (root / "README.md").read_text(encoding="utf-8")
        self.write("README.md", readme + "\n[Missing](docs/NOPE.md)\n")

        errors = VALIDATOR.validate_repository(root)

        self.assertTrue(any("missing target" in error for error in errors), errors)


if __name__ == "__main__":
    unittest.main()
