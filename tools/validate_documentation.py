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
    "docs/FUTURE_PHASES_DESIGN.md",
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

FORBIDDEN_ARTIFACT_PATHS = (
    ".superpowers",
    "docs/superpowers",
)

FUTURE_PHASE_RANGE = range(7, 13)
VALID_PHASE_STATUSES = {"PLANNED", "IN PROGRESS", "COMPLETE"}
FUTURE_PHASE_GOVERNANCE_TERMS = (
    "scope",
    "explicit exclusions",
    "security",
    "mutation classification",
    "exit evidence",
    "completion date",
    "pull request",
)

FACT_PATTERNS = {
    "README.md": (
        (r"\b78[- ]canonical|\b78 canonical", "78 canonical"),
        (r"\b10 (?:additional )?legacy", "10 legacy"),
        (r"\b88 total|\(88 total\)|88 registrations", "88 total"),
        (r"--project[\s\S]*DIDI_PROJECT_ROOT|DIDI_PROJECT_ROOT[\s\S]*--project", "explicit project root"),
        (r"\bdry_run\b", "mutation dry-run"),
        (r"\bconfirmation_token\b", "mutation confirmation"),
    ),
    "docs/CAPABILITIES.md": (
        (r"\b78 canonical", "78 canonical"),
        (r"\b10 legacy|Ten legacy", "10 legacy"),
        (r"\b88 [`\w/-]* ?entries|exactly 88", "88 total"),
        (r"\b60 (?:canonical )?tools? (?:are )?implemented|Sixty (?:canonical )?tools? are implemented|Sixty are implemented", "60 implemented"),
        (r"\b18 (?:canonical )?(?:tools? )?(?:remain )?(?:reserved|unimplemented)", "18 unimplemented"),
        (r"--project[\s\S]*DIDI_PROJECT_ROOT|DIDI_PROJECT_ROOT[\s\S]*--project", "explicit project root"),
        (r"\b423\b", "one-client session lock"),
        (r"\bdry_run\b", "mutation dry-run"),
        (r"\bconfirmation_token\b", "mutation confirmation"),
    ),
    "docs/TOOL_REFERENCE.md": (
        (r"\b78 canonical", "78 canonical"),
        (r"\b10 legacy", "10 legacy"),
        (r"\b88 registrations", "88 total"),
        (r"\b423\b", "one-client session lock"),
        (r"\bdry_run\b", "mutation dry-run"),
        (r"\bconfirmation_token\b", "mutation confirmation"),
    ),
    "CHANGELOG.md": (
        (r"\b78 canonical", "78 canonical"),
        (r"\b10 legacy", "10 legacy"),
        (r"\b88 total|88-registration", "88 total"),
        (r"\b60 (?:canonical )?tools? (?:are )?implemented|Sixty (?:canonical )?tools? are implemented", "60 implemented"),
        (r"\b18 (?:canonical )?(?:tools? )?(?:remain )?(?:reserved|honestly unimplemented|unimplemented)", "18 unimplemented"),
    ),
    "docs/API_SPECIFICATION.md": (
        (r"\b423\b", "one-client session lock"),
        (r"\bdry_run\b", "mutation dry-run"),
        (r"\bconfirmation_token\b", "mutation confirmation"),
        (r"ui\.hitTest", "live UI hit-test IPC method"),
    ),
    "docs/LLM_INSTRUCTIONS.md": (
        (r"--project[\s\S]*DIDI_PROJECT_ROOT|DIDI_PROJECT_ROOT[\s\S]*--project", "explicit project root"),
        (r"\bdry_run\b", "mutation dry-run"),
        (r"\bconfirmation_token\b", "mutation confirmation"),
    ),
    "docs/QUICKSTART.md": (
        (r"--project", "explicit project argument"),
        (r"\bdry_run\b", "mutation dry-run"),
        (r"\bconfirmation_token\b", "mutation confirmation"),
    ),
    "docs/ROADMAP.md": (
        (r"Phase 6[^\n]*COMPLETE", "completed Phase 6"),
        (r"Phase 5 Deep Domains \(6\)", "six-tool Phase 5 canonical row"),
    ),
}

LINK_PATTERN = re.compile(r"!?\[[^\]]*\]\(([^)]+)\)")
HEADING_PATTERN = re.compile(r"^\s{0,3}#{1,6}\s+(.+?)\s*#*\s*$")
PHASE_HEADING_PATTERN = re.compile(
    r"^## Phase (?P<number>\d+):.*?\(`(?P<status>[^`]+)`\)\s*$",
    re.MULTILINE,
)
FENCED_CODE_PATTERN = re.compile(
    r"^\s{0,3}(`{3,}|~{3,}).*?^\s{0,3}\1\s*$", re.MULTILINE | re.DOTALL
)
INLINE_CODE_PATTERN = re.compile(r"(?<!`)`[^`\n]*`(?!`)")

MINIMUM_NODE24_ACTION_VERSIONS = {
    "actions/checkout": (5,),
    "actions/upload-artifact": (6,),
    "actions/download-artifact": (7,),
    "softprops/action-gh-release": (3,),
    "hendrikmuhs/ccache-action": (1, 2, 21),
}


def _workflow_job_blocks(text: str) -> list[str]:
    """Return job-local YAML blocks without requiring a YAML dependency."""
    lines = text.splitlines()
    jobs_start: int | None = None
    jobs_indent = 0
    for index, line in enumerate(lines):
        match = re.match(r"^(\s*)jobs:\s*(?:#.*)?$", line)
        if match:
            jobs_start = index
            jobs_indent = len(match.group(1))
            break

    if jobs_start is None:
        return []

    starts: list[tuple[int, int]] = []
    job_indent: int | None = None
    for index in range(jobs_start + 1, len(lines)):
        line = lines[index]
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        indent = len(line) - len(line.lstrip())
        if indent <= jobs_indent:
            break
        match = re.match(
            r"^(\s*)(?:[A-Za-z_][A-Za-z0-9_-]*|"
            r"'[A-Za-z_][A-Za-z0-9_-]*'|"
            r"\"[A-Za-z_][A-Za-z0-9_-]*\"):\s*(?:#.*)?$",
            line,
        )
        if not match:
            continue
        if job_indent is None:
            job_indent = len(match.group(1))
        if len(match.group(1)) == job_indent:
            starts.append((index, job_indent))

    blocks: list[str] = []
    for position, (start, _indent) in enumerate(starts):
        end = starts[position + 1][0] if position + 1 < len(starts) else len(lines)
        for index in range(start + 1, end):
            line = lines[index]
            if (
                line.strip()
                and not line.lstrip().startswith("#")
                and len(line) - len(line.lstrip()) <= jobs_indent
            ):
                end = index
                break
        blocks.append("\n".join(lines[start:end]))
    return blocks


def _workflow_steps(text: str) -> list[str]:
    """Return YAML list items from a job's steps block."""
    lines = text.splitlines()
    steps_start: int | None = None
    steps_indent = 0
    for index, line in enumerate(lines):
        match = re.match(r"^(\s*)steps:\s*(?:#.*)?$", line)
        if match:
            steps_start = index
            steps_indent = len(match.group(1))
            break

    if steps_start is None:
        return []

    starts: list[tuple[int, int]] = []
    item_indent: int | None = None
    section_end = len(lines)
    for index in range(steps_start + 1, len(lines)):
        line = lines[index]
        if line.strip() and not line.lstrip().startswith("#"):
            indent = len(line) - len(line.lstrip())
            if indent <= steps_indent:
                section_end = index
                break
            match = re.match(r"^(\s*)-(?:\s+.*)?$", line)
            if match:
                if item_indent is None:
                    item_indent = len(match.group(1))
                if len(match.group(1)) == item_indent:
                    starts.append((index, item_indent))

    blocks: list[str] = []
    for position, (start, _indent) in enumerate(starts):
        end = starts[position + 1][0] if position + 1 < len(starts) else section_end
        blocks.append("\n".join(lines[start:end]))
    return blocks


def _dedent_yaml_block(lines: list[str]) -> str:
    indents = [len(line) - len(line.lstrip()) for line in lines if line.strip()]
    indent = min(indents) if indents else 0
    return "\n".join(line[indent:] if line.strip() else "" for line in lines)


def _step_mapping(step: str) -> dict[str, str]:
    """Extract scalar values for keys on a workflow step itself."""
    lines = step.splitlines()
    if not lines:
        return {}
    item = re.match(r"^(\s*)-\s*(.*)$", lines[0])
    if not item:
        return {}
    mapping_indent = len(item.group(1)) + 2

    keys: list[tuple[int, str, str]] = []
    first_key = re.match(r"^([A-Za-z_][A-Za-z0-9_-]*):\s*(.*)$", item.group(2))
    if first_key:
        keys.append((0, first_key.group(1), first_key.group(2)))

    for index, line in enumerate(lines[1:], start=1):
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        if len(line) - len(line.lstrip()) != mapping_indent:
            continue
        match = re.match(r"^\s*([A-Za-z_][A-Za-z0-9_-]*):\s*(.*)$", line)
        if match:
            keys.append((index, match.group(1), match.group(2)))

    values: dict[str, str] = {}
    for position, (line_index, key, raw_value) in enumerate(keys):
        if re.fullmatch(r"[|>][-+]?\d*", raw_value):
            end = keys[position + 1][0] if position + 1 < len(keys) else len(lines)
            values[key] = _dedent_yaml_block(lines[line_index + 1 : end])
        else:
            values[key] = raw_value.strip()
    return values


def _has_macos_aws_tap_cleanup(step: str) -> bool:
    """Return whether a workflow step safely removes the unused runner tap."""
    values = _step_mapping(step)
    macos_only = re.fullmatch(
        r"(?:runner\.os\s*==\s*['\"]macOS['\"]|"
        r"\$\{\{\s*runner\.os\s*==\s*['\"]macOS['\"]\s*\}\})",
        values.get("if", ""),
    )
    guarded_removal = re.search(
        r"^[ \t]*if[ \t]+brew[ \t]+tap[ \t]*\|[ \t]*grep[ \t]+-qx[ \t]+"
        r"(['\"])aws/tap\1;[ \t]+then[ \t]*\n"
        r"[ \t]*brew[ \t]+untap[ \t]+aws/tap[ \t]*\n"
        r"[ \t]*fi[ \t]*$",
        values.get("run", ""),
        flags=re.MULTILINE,
    )
    return bool(macos_only and guarded_removal)


def _job_uses_macos_runner(job: str) -> bool:
    runner_label = re.compile(
        r"\bmacos-(?:latest|\d+)(?:-[A-Za-z0-9]+)*\b",
        flags=re.IGNORECASE,
    )
    lines = job.splitlines()
    steps_index = next(
        (index for index, line in enumerate(lines) if re.match(r"^\s*steps:\s*$", line)),
        len(lines),
    )
    configuration = lines[:steps_index]
    runs_on = next(
        (
            match.group(1).strip()
            for line in configuration
            if (match := re.match(r"^\s*runs-on:\s*(.+)$", line))
        ),
        "",
    )
    if runner_label.search(runs_on):
        return True
    if "matrix." not in runs_on:
        return False

    for index, line in enumerate(configuration):
        matrix = re.match(r"^(\s*)matrix:\s*(.*)$", line)
        if not matrix:
            continue
        matrix_indent = len(matrix.group(1))
        block = [matrix.group(2)]
        for candidate in configuration[index + 1 :]:
            if (
                candidate.strip()
                and not candidate.lstrip().startswith("#")
                and len(candidate) - len(candidate.lstrip()) <= matrix_indent
            ):
                break
            block.append(candidate)
        if runner_label.search("\n".join(block)):
            return True
    return False


def _step_uses_ccache_action(step: str) -> bool:
    uses = _step_mapping(step).get("uses", "").strip().strip("'\"")
    return bool(re.match(r"^hendrikmuhs/ccache-action@", uses))


def validate_workflow_contract(relative_path: str, text: str) -> list[str]:
    """Validate release-runner assumptions that have caused packaging failures."""
    errors: list[str] = []
    for match in re.finditer(
        r"uses:\s*([A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+)@v(\d+(?:\.\d+){0,2})\b", text
    ):
        action, version_text = match.groups()
        minimum = MINIMUM_NODE24_ACTION_VERSIONS.get(action)
        observed = tuple(int(part) for part in version_text.split("."))
        padded_observed = observed + (0,) * (3 - len(observed))
        padded_minimum = minimum + (0,) * (3 - len(minimum)) if minimum else None
        if padded_minimum is not None and padded_observed < padded_minimum:
            recommendation = ".".join(str(part) for part in minimum)
            errors.append(
                f"{relative_path}: {action}@v{version_text} still targets deprecated Node 20; "
                f"use v{recommendation} or later"
            )

    for job in _workflow_job_blocks(text):
        if not _job_uses_macos_runner(job):
            continue
        steps = _workflow_steps(job)
        for index, step in enumerate(steps):
            if not _step_uses_ccache_action(step):
                continue
            if index == 0 or not _has_macos_aws_tap_cleanup(steps[index - 1]):
                errors.append(
                    f"{relative_path}: remove aws/tap on macOS before "
                    "hendrikmuhs/ccache-action"
                )

    if "windows-latest" not in text:
        return errors

    for job in _workflow_job_blocks(text):
        for step in _workflow_steps(job):
            if "jwlawson/actions-setup-cmake@v2" not in step:
                continue
            if not re.search(r"cmake-version:\s*['\"]?3\.28\.x['\"]?", step):
                continue
            excludes_windows = re.search(
                r"^\s*if:\s*runner\.os\s*!=\s*['\"]Windows['\"]\s*$",
                step,
                flags=re.MULTILINE,
            )
            if not excludes_windows:
                errors.append(
                    f"{relative_path}: pinned CMake 3.28 must not replace the Windows runner CMake"
                )
    return errors


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


def validate_future_phase_roadmap(roadmap_text: str) -> list[str]:
    errors: list[str] = []
    phases = {
        int(match.group("number")): match.group("status")
        for match in PHASE_HEADING_PATTERN.finditer(roadmap_text)
    }
    for phase in FUTURE_PHASE_RANGE:
        if phase not in phases:
            errors.append(f"docs/ROADMAP.md must declare Phase {phase}")
            continue
        status = phases[phase]
        if status not in VALID_PHASE_STATUSES:
            errors.append(
                f"docs/ROADMAP.md Phase {phase} has invalid status '{status}'"
            )
    return errors


def validate_future_phase_governance(design_text: str) -> list[str]:
    normalized = design_text.lower()
    missing = [
        term for term in FUTURE_PHASE_GOVERNANCE_TERMS
        if term not in normalized
    ]
    if missing:
        return [
            "docs/FUTURE_PHASES_DESIGN.md must define future-phase governance"
        ]
    return []


def _include_markdown(root: Path, path: Path) -> bool:
    relative_parts = path.relative_to(root).parts
    return not any(part in {".git", ".worktrees", ".superpowers"} for part in relative_parts) and not any(
        part.startswith("build") for part in relative_parts
    )


def validate_repository(root: Path) -> list[str]:
    root = root.resolve()
    errors: list[str] = []
    texts: dict[str, str] = {}

    for relative_path in FORBIDDEN_ARTIFACT_PATHS:
        if (root / relative_path).exists():
            errors.append(
                f"{relative_path}: agent workflow artifacts must not be committed to the project"
            )

    workflow_root = root / ".github" / "workflows"
    if workflow_root.is_dir():
        for workflow in sorted(workflow_root.glob("*.y*ml")):
            try:
                workflow_text = workflow.read_text(encoding="utf-8", errors="strict")
            except (OSError, UnicodeError) as error:
                errors.append(
                    f"{workflow.relative_to(root).as_posix()}: cannot read UTF-8 workflow: {error}"
                )
                continue
            if ".superpowers/" in workflow_text or "docs/superpowers/" in workflow_text:
                errors.append(
                    f"{workflow.relative_to(root).as_posix()}: Superpowers artifacts must not be CI dependencies"
                )
            errors.extend(
                validate_workflow_contract(
                    workflow.relative_to(root).as_posix(), workflow_text
                )
            )

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

    roadmap_text = texts.get("docs/ROADMAP.md")
    if roadmap_text is not None:
        errors.extend(validate_future_phase_roadmap(roadmap_text))

    design_text = texts.get("docs/FUTURE_PHASES_DESIGN.md")
    if design_text is not None:
        errors.extend(validate_future_phase_governance(design_text))

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
