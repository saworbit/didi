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
        self.write("CMakeLists.txt", "project(didi VERSION 1.3.0 LANGUAGES C CXX)\n")
        self.write(
            "include/didi/mcp/mcp_protocol.hpp",
            'inline const char* kServerVersion = "1.3.0";\n',
        )
        self.write(
            "src/standalone/main.cpp",
            'std::cout << "didi (godot-mcp-native) v1.3.0";\n',
        )
        self.write("addons/didi/plugin.cfg", 'version="1.3.0"\n')

        self.write(
            "README.md",
            """# Didi

Current documented release: **1.3.0**.

Didi exposes 68 canonical tools plus 10 legacy names (78 total).

[Guide](docs/GUIDE.md#details-1) | [Security](SECURITY.md) | [Overview](#didi)
""",
        )
        self.write(
            "CHANGELOG.md",
            """# Changelog

## [Unreleased]

## [1.3.0] - 2026-08-27

Version 1.3.0 exposes 68 canonical tools, 10 legacy registrations, and 78 total registrations. Fifty canonical tools are implemented and 18 remain unimplemented.
""",
        )
        self.write(
            "SECURITY.md",
            """# Security Policy

Current release: 1.3.0.

| Version | Supported |
| --- | --- |
| 1.3.x | :white_check_mark: |
| 1.2.x | :x: |
| <=1.1.x | :x: |
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

Didi v1.3.0 registers 68 canonical tool names. Fifty are implemented in at least one mode; 18 remain reserved. Ten legacy names are registered separately, for exactly 78 tools/list entries.
""",
        )
        self.write(
            "docs/TOOL_REFERENCE.md",
            """# Tool Reference

Didi exposes 68 canonical tool names plus 10 legacy names (78 registrations).
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
            any("addons/didi/plugin.cfg" in error and "1.3.0" in error for error in errors),
            errors,
        )

    def test_rejects_stale_supported_minor(self):
        root = self.make_valid_repository()
        security = (root / "SECURITY.md").read_text(encoding="utf-8")
        self.write(
            "SECURITY.md",
            security.replace("| 1.2.x | :x: |", "| 1.2.x | :white_check_mark: |"),
        )

        errors = VALIDATOR.validate_repository(root)

        self.assertTrue(any("unsupported release 1.2.x" in error for error in errors), errors)

    def test_reports_missing_current_release_fact(self):
        root = self.make_valid_repository()
        readme = (root / "README.md").read_text(encoding="utf-8")
        self.write("README.md", readme.replace("68 canonical", "67 canonical"))

        errors = VALIDATOR.validate_repository(root)

        self.assertTrue(any("README.md" in error and "68 canonical" in error for error in errors), errors)

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


if __name__ == "__main__":
    unittest.main()
