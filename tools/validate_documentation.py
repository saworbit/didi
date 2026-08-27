#!/usr/bin/env python3
"""Validate Didi's release documentation contract with no third-party dependencies."""

from __future__ import annotations

import argparse
from pathlib import Path
import re
import sys
from typing import Sequence
from urllib.parse import unquote, urlparse


REQUIRED_DOCUMENTS = (
    "LICENSE",
    "README.md",
    "CHANGELOG.md",
    "CONTRIBUTING.md",
    "SECURITY.md",
    "docs/ADMIN_GUIDE.md",
    "docs/API_SPECIFICATION.md",
    "docs/ARCHITECTURE.md",
    "docs/CAPABILITIES.md",
    "docs/DEVELOPER_GUIDE.md",
    "docs/INTEGRATION_GUIDE.md",
    "docs/LLM_INSTRUCTIONS.md",
    "docs/QUICKSTART.md",
    "docs/RESOURCES_AND_PROMPTS.md",
    "docs/ROADMAP.md",
    "docs/TOOL_REFERENCE.md",
)

VERSION_SOURCES = (
    "CMakeLists.txt",
    "include/didi/mcp/mcp_protocol.hpp",
    "src/standalone/main.cpp",
    "addons/didi/plugin.cfg",
    "README.md",
    "docs/CAPABILITIES.md",
    "CHANGELOG.md",
    "SECURITY.md",
)

FACT_PATTERNS = {
    "README.md": (
        (r"\b68[- ]canonical|\b68 canonical", "68 canonical"),
        (r"\b10 (?:additional )?legacy", "10 legacy"),
        (r"\b78 total|\(78 total\)|78 registrations", "78 total"),
    ),
    "docs/CAPABILITIES.md": (
        (r"\b68 canonical", "68 canonical"),
        (r"\b10 legacy|Ten legacy", "10 legacy"),
        (r"\b78 [`\w/-]* ?entries|exactly 78", "78 total"),
        (r"\b50 (?:canonical )?tools? (?:are )?implemented|Fifty (?:canonical )?tools? are implemented|Fifty are implemented", "50 implemented"),
        (r"\b18 (?:canonical )?(?:tools? )?(?:remain )?(?:reserved|unimplemented)", "18 unimplemented"),
    ),
    "docs/TOOL_REFERENCE.md": (
        (r"\b68 canonical", "68 canonical"),
        (r"\b10 legacy", "10 legacy"),
        (r"\b78 registrations", "78 total"),
    ),
    "CHANGELOG.md": (
        (r"\b68 canonical", "68 canonical"),
        (r"\b10 legacy", "10 legacy"),
        (r"\b78 total|78-registration", "78 total"),
        (r"\b50 (?:canonical )?tools? (?:are )?implemented|Fifty (?:canonical )?tools? are implemented", "50 implemented"),
        (r"\b18 (?:canonical )?(?:tools? )?(?:remain )?(?:reserved|honestly unimplemented|unimplemented)", "18 unimplemented"),
    ),
}

LINK_PATTERN = re.compile(r"!?\[[^\]]*\]\(([^)]+)\)")
HEADING_PATTERN = re.compile(r"^\s{0,3}#{1,6}\s+(.+?)\s*#*\s*$")
FENCED_CODE_PATTERN = re.compile(
    r"^\s{0,3}(`{3,}|~{3,}).*?^\s{0,3}\1\s*$", re.MULTILINE | re.DOTALL
)
INLINE_CODE_PATTERN = re.compile(r"(?<!`)`[^`\n]*`(?!`)")


def extract_project_version(cmake_text: str) -> str:
    match = re.search(
        r"project\s*\(\s*didi\s+VERSION\s+(\d+\.\d+\.\d+)\b",
        cmake_text,
        flags=re.IGNORECASE | re.DOTALL,
    )
    if not match:
        raise ValueError("CMakeLists.txt: cannot derive project(didi VERSION x.y.z)")
    return match.group(1)


def _without_code(text: str) -> str:
    return INLINE_CODE_PATTERN.sub("", FENCED_CODE_PATTERN.sub("", text))


def markdown_anchors(text: str) -> set[str]:
    anchors: set[str] = set()
    seen: dict[str, int] = {}
    clean_text = FENCED_CODE_PATTERN.sub("", text)
    for line in clean_text.splitlines():
        match = HEADING_PATTERN.match(line)
        if not match:
            continue
        heading = re.sub(r"<[^>]+>", "", match.group(1))
        heading = re.sub(r"!?\[([^\]]*)\]\([^)]*\)", r"\1", heading)
        heading = heading.replace("`", "").lower()
        slug = re.sub(r"[^\w\- ]", "", heading, flags=re.UNICODE)
        slug = re.sub(r"\s", "-", slug).strip("-")
        duplicate = seen.get(slug, 0)
        seen[slug] = duplicate + 1
        anchors.add(slug if duplicate == 0 else f"{slug}-{duplicate}")
    anchors.update(
        match.lower()
        for match in re.findall(
            r"<a\s+(?:name|id)=[\"']([^\"']+)[\"']", clean_text, flags=re.IGNORECASE
        )
    )
    return anchors


def validate_markdown_links(root: Path, markdown: list[Path]) -> list[str]:
    errors: list[str] = []
    anchor_cache: dict[Path, set[str]] = {}
    repository = root.resolve()

    for source in markdown:
        relative_source = source.relative_to(root).as_posix()
        try:
            text = source.read_text(encoding="utf-8", errors="strict")
        except (OSError, UnicodeError) as error:
            errors.append(f"{relative_source}: cannot read UTF-8 Markdown: {error}")
            continue

        for raw_target in LINK_PATTERN.findall(_without_code(text)):
            target = raw_target.strip().split()[0].strip("<>")
            if not target:
                continue
            parsed = urlparse(target)
            if parsed.scheme or target.startswith("//"):
                continue

            file_part, separator, fragment = target.partition("#")
            decoded_file = unquote(file_part).split("?", 1)[0]
            candidate = source if not decoded_file else source.parent / decoded_file
            resolved = candidate.resolve()
            try:
                resolved.relative_to(repository)
            except ValueError:
                errors.append(f"{relative_source}: target escapes repository: {target}")
                continue
            if not resolved.exists():
                errors.append(f"{relative_source}: missing target: {target}")
                continue
            if separator and fragment and resolved.suffix.lower() == ".md":
                try:
                    anchors = anchor_cache.setdefault(
                        resolved,
                        markdown_anchors(resolved.read_text(encoding="utf-8", errors="strict")),
                    )
                except (OSError, UnicodeError) as error:
                    errors.append(f"{relative_source}: cannot read anchor target {target}: {error}")
                    continue
                decoded_fragment = unquote(fragment).lower()
                if decoded_fragment not in anchors:
                    errors.append(f"{relative_source}: missing anchor: {target}")
    return errors


def _read_required(root: Path, relative_path: str, errors: list[str]) -> str | None:
    path = root / relative_path
    if not path.is_file():
        errors.append(f"{relative_path}: required file is missing")
        return None
    try:
        return path.read_text(encoding="utf-8", errors="strict")
    except (OSError, UnicodeError) as error:
        errors.append(f"{relative_path}: cannot read UTF-8 text: {error}")
        return None


def _include_markdown(root: Path, path: Path) -> bool:
    relative_parts = path.relative_to(root).parts
    return not any(part in {".git", ".worktrees", ".superpowers"} for part in relative_parts) and not any(
        part.startswith("build") for part in relative_parts
    )


def validate_repository(root: Path) -> list[str]:
    root = root.resolve()
    errors: list[str] = []
    texts: dict[str, str] = {}

    for relative_path in sorted(set(REQUIRED_DOCUMENTS + VERSION_SOURCES)):
        text = _read_required(root, relative_path, errors)
        if text is not None:
            texts[relative_path] = text

    cmake_text = texts.get("CMakeLists.txt")
    version: str | None = None
    if cmake_text is not None:
        try:
            version = extract_project_version(cmake_text)
        except ValueError as error:
            errors.append(str(error))

    if version is not None:
        version_expectations = {
            "include/didi/mcp/mcp_protocol.hpp": f'kServerVersion = "{version}"',
            "src/standalone/main.cpp": f"v{version}",
            "addons/didi/plugin.cfg": f'version="{version}"',
            "README.md": version,
            "docs/CAPABILITIES.md": version,
            "CHANGELOG.md": f"[{version}]",
            "SECURITY.md": version,
        }
        for relative_path, expected in version_expectations.items():
            text = texts.get(relative_path)
            if text is not None and expected not in text:
                errors.append(
                    f"{relative_path}: expected current project version {version} ({expected!r})"
                )

        major, minor, _patch = version.split(".")
        supported_minor = f"{major}.{minor}.x"
        security = texts.get("SECURITY.md")
        if security is not None:
            supported_rows: list[str] = []
            for line in security.splitlines():
                row = re.match(r"^\|\s*([^|]+?)\s*\|\s*([^|]+?)\s*\|\s*$", line)
                if not row:
                    continue
                release, status = (part.strip() for part in row.groups())
                if ":white_check_mark:" in status:
                    supported_rows.append(release)
            if supported_minor not in supported_rows:
                errors.append(f"SECURITY.md: current release line {supported_minor} is not supported")
            for release in supported_rows:
                if release != supported_minor:
                    errors.append(f"SECURITY.md: unsupported release {release} is marked supported")

    for relative_path, requirements in FACT_PATTERNS.items():
        text = texts.get(relative_path)
        if text is None:
            continue
        for pattern, label in requirements:
            if not re.search(pattern, text, flags=re.IGNORECASE):
                errors.append(f"{relative_path}: missing current release fact: {label}")

    markdown = sorted(
        path
        for path in root.rglob("*.md")
        if _include_markdown(root, path)
    )
    errors.extend(validate_markdown_links(root, markdown))
    return errors


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--root",
        type=Path,
        default=Path(__file__).resolve().parents[1],
        help="repository root (defaults to the validator's parent repository)",
    )
    arguments = parser.parse_args(argv)
    errors = validate_repository(arguments.root)
    if errors:
        print("\n".join(errors), file=sys.stderr)
        return 1
    markdown_count = sum(
        1
        for path in arguments.root.rglob("*.md")
        if _include_markdown(arguments.root, path)
    )
    print(f"Documentation contract valid ({markdown_count} Markdown files, version sources aligned)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
