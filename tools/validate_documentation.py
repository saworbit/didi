#!/usr/bin/env python3
"""Validate Didi's release documentation contract with no third-party dependencies."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import subprocess
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
    "docs/FUTURE_PHASES_IMPLEMENTATION_PLAN.md",
    "docs/PHASE_7_API_FEASIBILITY.md",
    "docs/PHASE_7_IMPLEMENTATION_PLAN.md",
    "docs/TOOL_REFERENCE.md",
)

# The C++ side is not on this list any more. It reads the version from a header
# generated out of project(VERSION ...), so it cannot drift and there is nothing
# to compare. GENERATED_VERSION_FILES below keeps it that way.
# Files that must keep reading the generated version rather than repeating it.
# Removing them from VERSION_SOURCES only stops the comparison; this is what
# stops someone quietly typing the number back in and reintroducing the drift.
GENERATED_VERSION_FILES = (
    "include/didi/mcp/mcp_protocol.hpp",
    "src/standalone/main.cpp",
)

VERSION_SOURCES = (
    "CMakeLists.txt",
    "addons/didi/plugin.cfg",
    # The demo project ships its own copy of the addon, because a Godot project
    # cannot reference an addon outside its own res://. It drifted two minor
    # versions behind unnoticed precisely because nothing checked it.
    "demo/addons/didi/plugin.cfg",
    "README.md",
    "docs/CAPABILITIES.md",
    "CHANGELOG.md",
    "SECURITY.md",
)

TRACKED_ONLY_ARTIFACT_PATHS = (".superpowers",)
FILESYSTEM_FORBIDDEN_ARTIFACT_PATHS = ("docs/superpowers",)

FUTURE_PHASE_RANGE = range(7, 13)
PHASE7_STATUS = "PARTIAL_DELIVERY"
VALID_PHASE_STATUSES = {"PLANNED", "IN PROGRESS", "COMPLETE", PHASE7_STATUS}
FUTURE_PHASE_GOVERNANCE_FIELDS = (
    "scope",
    "explicit exclusions",
    "security classification",
    "mutation classification",
    "exit evidence",
)
FUTURE_PHASE_COMPLETION_FIELDS = (
    "completion date",
    "pull request",
    "verification evidence",
)
# Fallback used when no tool manifest is supplied: local runs without a build,
# and the docs-only CI job, which deliberately does not build the binary. When a
# manifest IS supplied the built binary is authoritative and this constant is
# checked against it, so a stale fallback fails the build jobs rather than
# silently disagreeing with the software it stands in for.
CANONICAL_IMPLEMENTATION_COUNTS = (100, 97, 3)

# How many names Phase 7 reserved in total. Distinct from the unimplemented
# count above: they were equal only while no Phase 7 tool had been delivered,
# and conflating them silently breaks the moment one is.
PHASE7_NAME_COUNT = 18

TOOL_MANIFEST_COUNT_KEYS = ("canonical", "legacy", "implemented", "unimplemented", "total")

# Which published counts each document must state, keyed by manifest count.
COUNT_FACTS = {
    "README.md": (
        ("canonical", r"\b{n}[- ]canonical", "canonical tools"),
        ("legacy", r"\b{n} (?:additional )?legacy", "legacy names"),
        ("total", r"\b{n} total|\({n} total\)|{n} registrations", "total registrations"),
    ),
    "docs/CAPABILITIES.md": (
        ("canonical", r"\b{n} canonical", "canonical tools"),
        ("legacy", r"\b{n} legacy|{w} legacy", "legacy names"),
        ("total", r"\b{n} [`\w/-]* ?entries|exactly {n}", "total registrations"),
        ("implemented", r"\b{n} (?:canonical )?tools? (?:are )?implemented|{w} (?:canonical tools? )?are implemented", "implemented tools"),
        ("unimplemented", r"\b{n} (?:canonical )?(?:tools? )?(?:remain )?(?:reserved|unimplemented)", "unimplemented tools"),
    ),
    "docs/TOOL_REFERENCE.md": (
        ("canonical", r"\b{n} canonical", "canonical tools"),
        ("legacy", r"\b{n} legacy", "legacy names"),
        ("total", r"\b{n} registrations", "total registrations"),
    ),
    "CHANGELOG.md": (
        ("canonical", r"\b{n} canonical", "canonical tools"),
        ("legacy", r"\b{n} legacy", "legacy names"),
        ("total", r"\b{n} total|{n}-registration", "total registrations"),
        ("implemented", r"\b{n} (?:canonical )?tools? (?:are )?implemented|{w} (?:canonical )?tools? are implemented", "implemented tools"),
        ("unimplemented", r"\b{n} (?:canonical )?(?:tools? )?(?:remain )?(?:reserved|honestly unimplemented|unimplemented)", "unimplemented tools"),
    ),
}

# Documentation may spell a count in words; the number still comes from the manifest.
NUMBER_WORDS = {
    10: "ten",
    18: "eighteen",
    60: "sixty",
    14: "fourteen",
    61: "sixty-one",
    65: "sixty-five",
    66: "sixty-six",
    67: "sixty-seven",
    68: "sixty-eight",
    69: "sixty-nine",
    78: "seventy-eight",
    79: "seventy-nine",
    80: "eighty",
    81: "eighty-one",
    82: "eighty-two",
    83: "eighty-three",
    88: "eighty-eight",
    89: "eighty-nine",
    90: "ninety",
    91: "ninety-one",
    92: "ninety-two",
    93: "ninety-three",
    94: "ninety-four",
    95: "ninety-five",
    96: "ninety-six",
    97: "ninety-seven",
    100: "one hundred",
}
# Reverse lookup, so a document may spell a count in words without the
# validator needing a branch for each individual number.
WORD_NUMBERS = {word: number for number, word in NUMBER_WORDS.items()}
# Longest first, so a hyphenated word is not truncated to its prefix.
NUMBER_WORD_PATTERN = "|".join(
    sorted((re.escape(word) for word in WORD_NUMBERS), key=len, reverse=True)
)
PHASE7_CANONICAL_TOOLS = (
    "signal_list_connections",
    "signal_connect",
    "signal_disconnect",
    "signal_emit",
    "viewport_set_camera_transform",
    "viewport_toggle_debug_draw",
    "tilemap_set_cells",
    "tilemap_get_used_rect",
    "gridmap_set_cells",
    "physics_raycast_query",
    "physics_simulate_step",
    "nav_bake_mesh",
    "nav_query_path",
    "anim_list_tracks",
    "anim_play_track",
    "runtime_inject_input",
    "runtime_get_call_stack",
    "runtime_read_profiler",
)
PHASE7_BLOCKED_TOOLS = {
    "physics_simulate_step",
    "nav_bake_mesh",
    "runtime_get_call_stack",
}
PHASE7_STATUS_DOCUMENTS = (
    "README.md",
    "CHANGELOG.md",
    "docs/CAPABILITIES.md",
    "docs/DEVELOPER_GUIDE.md",
    "docs/FUTURE_PHASES_DESIGN.md",
    "docs/FUTURE_PHASES_IMPLEMENTATION_PLAN.md",
    "docs/LLM_INSTRUCTIONS.md",
    "docs/PHASE_7_API_FEASIBILITY.md",
    "docs/PHASE_7_IMPLEMENTATION_PLAN.md",
    "docs/ROADMAP.md",
    "docs/TOOL_REFERENCE.md",
)
PHASE7_CURRENT_STATE_DOCUMENTS = (
    "README.md",
    "CHANGELOG.md",
    "docs/CAPABILITIES.md",
    "docs/DEVELOPER_GUIDE.md",
    "docs/FUTURE_PHASES_DESIGN.md",
    "docs/LLM_INSTRUCTIONS.md",
    "docs/ROADMAP.md",
    "docs/TOOL_REFERENCE.md",
)
PHASE7_STALE_DELIVERY_CLAIMS = {
    "README.md": (
        (
            re.compile(r"79 canonical tools plus 10 legacy names", re.IGNORECASE),
            "79 canonical tools plus 10 legacy names",
        ),
        (
            re.compile(r"Registry \(79 canonical tools \+ 10 legacy names\)", re.IGNORECASE),
            "Registry (79 canonical tools + 10 legacy names)",
        ),
    ),
    "docs/QUICKSTART.md": (
        (
            re.compile(
                r"TileMap/GridMap editing(?: and call-stack inspection)? "
                r"remain registered but unimplemented",
                re.IGNORECASE,
            ),
            "TileMap/GridMap editing remains registered but unimplemented",
        ),
    ),
    "docs/FUTURE_PHASES_DESIGN.md": (
        (
            re.compile(r"existing\s+79-tool\s+canonical\s+surface", re.IGNORECASE),
            "existing 79-tool canonical surface",
        ),
        (
            re.compile(
                r"(?:The\s+)?eleven\s+feasible-but-unbuilt\s+names\s+have\s+no\s+"
                r"implementation\s+at\s+all\s+yet",
                re.IGNORECASE,
            ),
            "Eleven feasible-but-unbuilt names have no implementation at all yet",
        ),
        (
            re.compile(
                r"Phase\s+7A-7C\s+define\s+the\s+approved\s+contracts[\s\S]{0,160}"
                r"Their\s+implementation\s+did\s+not\s+start\s+because",
                re.IGNORECASE,
            ),
            "Phase 7A-7C contracts: their implementation did not start",
        ),
        (
            re.compile(
                r"The\s+detailed\s+7A-7C\s+requirements\s+below\s+remain\s+"
                r"approved\s+contract\s+design,\s+not\s+delivered\s+behavior",
                re.IGNORECASE,
            ),
            "The detailed 7A-7C requirements are not delivered behavior",
        ),
        (
            re.compile(r"All\s+79\s+canonical\s+registrations\s+report", re.IGNORECASE),
            "All 79 canonical registrations report implemented",
        ),
    ),
    "docs/ROADMAP.md": (
        (
            re.compile(
                r"The\s+implementation\s+on\s+`main`\s+remains\s+61/79",
                re.IGNORECASE,
            ),
            "The implementation on `main` remains 61/79",
        ),
        (
            re.compile(
                r"TileMap/GridMap\s+(?:editing\s+)?(?:remains?|is)\s+unimplemented",
                re.IGNORECASE,
            ),
            "TileMap/GridMap editing remains unimplemented",
        ),
    ),
    "docs/LLM_INSTRUCTIONS.md": (
        (
            re.compile(r"exposes\s+79\s+canonical\s+tools", re.IGNORECASE),
            "exposes 79 canonical tools",
        ),
    ),
    "docs/DEVELOPER_GUIDE.md": (
        (
            re.compile(
                r"implemented:\s*false`?\s+for\s+`runtime_inject_input`[\s\S]{0,100}"
                r"`runtime_read_profiler`",
                re.IGNORECASE,
            ),
            "implemented: false for runtime_inject_input and runtime_read_profiler",
        ),
        (
            re.compile(r"79\s+canonical/10\s+legacy/89\s+total", re.IGNORECASE),
            "79 canonical/10 legacy/89 total",
        ),
        (
            re.compile(r"60\s+implemented/18\s+unimplemented", re.IGNORECASE),
            "60 implemented/18 unimplemented",
        ),
    ),
    "docs/RESOURCES_AND_PROMPTS.md": (
        (
            re.compile(
                r"Runtime input injection, call stacks, profiler telemetry[\s\S]{0,80}"
                r"remain unsupported",
                re.IGNORECASE,
            ),
            "Runtime input injection, call stacks, profiler telemetry remain unsupported",
        ),
    ),
    "docs/PHASE_7_IMPLEMENTATION_PLAN.md": (
        (
            re.compile(r"The\s+other\s+8\s+names\s+remain\s+registered\s+but\s+unimplemented", re.IGNORECASE),
            "The other 8 names remain registered but unimplemented",
        ),
        (
            re.compile(r"Work\s+can\s+proceed\s+only\s+after\s+governance\s+chooses", re.IGNORECASE),
            "Work can proceed only after governance chooses",
        ),
    ),
}
PHASE7_CURRENT_STATUS_START = "<!-- phase7-current-status:start -->"
PHASE7_CURRENT_STATUS_END = "<!-- phase7-current-status:end -->"
PHASE7_FEASIBLE_NAMES_START = "<!-- phase7-feasible-names:start -->"
PHASE7_FEASIBLE_NAMES_END = "<!-- phase7-feasible-names:end -->"

FACT_PATTERNS = {
    "README.md": (
        (r"--project[\s\S]*DIDI_PROJECT_ROOT|DIDI_PROJECT_ROOT[\s\S]*--project", "explicit project root"),
        (r"\bdry_run\b", "mutation dry-run"),
        (r"\bconfirmation_token\b", "mutation confirmation"),
    ),
    "docs/CAPABILITIES.md": (
        (r"--project[\s\S]*DIDI_PROJECT_ROOT|DIDI_PROJECT_ROOT[\s\S]*--project", "explicit project root"),
        (r"\b423\b", "one-client session lock"),
        (r"\bdry_run\b", "mutation dry-run"),
        (r"\bconfirmation_token\b", "mutation confirmation"),
    ),
    "docs/TOOL_REFERENCE.md": (
        (r"\b423\b", "one-client session lock"),
        (r"\bdry_run\b", "mutation dry-run"),
        (r"\bconfirmation_token\b", "mutation confirmation"),
    ),
    "CHANGELOG.md": (
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
ROADMAP_PHASE_HEADING_PATTERN = re.compile(
    r"^##\s+.*?\bPhase\s+(?P<number>\d+):(?P<title>[^\n]*)$",
    re.MULTILINE,
)
ROADMAP_SEQUENCE_STATUS_PATTERN = re.compile(
    r"^\|\s*\*\*Phase\s+(?P<number>\d+)\s+\((?P<status>[^)]+)\)\*\*\s*\|",
    re.MULTILINE,
)
DESIGN_PHASE_HEADING_PATTERN = re.compile(
    r"^## Phase (?P<number>\d+):[^\n]*$",
    re.MULTILINE,
)
FENCED_CODE_PATTERN = re.compile(
    r"^\s{0,3}(`{3,}|~{3,}).*?^\s{0,3}\1\s*$", re.MULTILINE | re.DOTALL
)
INLINE_CODE_PATTERN = re.compile(r"(?<!`)`[^`\n]*`(?!`)")
PHASE7_MATRIX_ROW_PATTERN = re.compile(
    r"^\|\s*`(?P<tool>[a-z0-9_]+)`\s*\|"
    r"(?:[^|\n]*\|)*\s*\*\*(?P<decision>GO|BLOCKED)\*\*\s*\|\s*$",
    re.MULTILINE,
)
PHASE7_CURRENT_STATUS_PATTERN = re.compile(
    r"^\*\*Status:\*\*\s+`(?P<status>[^`\n]+)`\s*\n"
    r"\*\*Canonical implementation:\*\*\s+`(?P<implemented>\d+)/(?P<canonical>\d+)`\s*\n"
    r"\*\*Phase 7 registrations:\*\*\s+`(?P<registered>\d+)/(?P<phase7_total>\d+)`\s+unimplemented\s*\n"
    r"\*\*Feasibility:\*\*\s+`(?P<feasible>\d+)/(?P<feasibility_total>\d+)`\s+"
    r"implementation-feasible;\s+`(?P<blocked>\d+)/(?P=feasibility_total)`\s+API-blocked$"
)

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


def _normalized_phase_status(title: str) -> str | None:
    match = re.search(r"\((?P<status>[^()]*)\)\s*$", title)
    if match is None:
        return None
    status = match.group("status").strip().strip("`").strip()
    return re.split(r"\s+(?:—|-)\s+", status, maxsplit=1)[0].strip()


def _roadmap_phase_declarations(roadmap_text: str) -> dict[int, list[str | None]]:
    declarations: dict[int, list[str | None]] = {}
    for match in ROADMAP_PHASE_HEADING_PATTERN.finditer(roadmap_text):
        phase = int(match.group("number"))
        declarations.setdefault(phase, []).append(
            _normalized_phase_status(match.group("title"))
        )
    return declarations


def validate_future_phase_roadmap(roadmap_text: str) -> list[str]:
    errors: list[str] = []
    declarations = _roadmap_phase_declarations(roadmap_text)

    for phase, statuses in sorted(declarations.items()):
        if len(statuses) > 1:
            errors.append(f"docs/ROADMAP.md declares Phase {phase} more than once")
        for status in statuses:
            if status is None:
                errors.append(f"docs/ROADMAP.md Phase {phase} must declare a status")
            elif status not in VALID_PHASE_STATUSES:
                message = f"docs/ROADMAP.md Phase {phase} has invalid status '{status}'"
                if message not in errors:
                    errors.append(message)

    for match in ROADMAP_SEQUENCE_STATUS_PATTERN.finditer(roadmap_text):
        phase = int(match.group("number"))
        status = match.group("status").strip().strip("`").strip()
        if status not in VALID_PHASE_STATUSES:
            message = f"docs/ROADMAP.md Phase {phase} has invalid status '{status}'"
            if message not in errors:
                errors.append(message)

    for phase in FUTURE_PHASE_RANGE:
        if phase not in declarations:
            errors.append(f"docs/ROADMAP.md must declare Phase {phase}")
    if declarations.get(7) != [PHASE7_STATUS]:
        errors.append(
            "docs/ROADMAP.md Phase 7 must declare status "
            f"'{PHASE7_STATUS}'"
        )
    return errors


def _phase_sections(
    text: str,
    heading_pattern: re.Pattern[str],
) -> dict[int, list[str]]:
    matches = list(heading_pattern.finditer(text))
    sections: dict[int, list[str]] = {}
    for index, match in enumerate(matches):
        end = matches[index + 1].start() if index + 1 < len(matches) else len(text)
        sections.setdefault(int(match.group("number")), []).append(text[match.end():end])
    return sections


def _section_has_field(section: str, field: str) -> bool:
    return bool(
        re.search(
            rf"^\*\*{re.escape(field)}:\*\*(?:\s+\S|\s*$)",
            section,
            flags=re.IGNORECASE | re.MULTILINE,
        )
    )


def validate_future_phase_governance(
    design_text: str,
    roadmap_text: str | None = None,
) -> list[str]:
    errors: list[str] = []
    sections = _phase_sections(design_text, DESIGN_PHASE_HEADING_PATTERN)
    roadmap_declarations = (
        _roadmap_phase_declarations(roadmap_text) if roadmap_text is not None else {}
    )

    for phase in FUTURE_PHASE_RANGE:
        phase_sections = sections.get(phase, [])
        if not phase_sections:
            errors.append(f"docs/FUTURE_PHASES_DESIGN.md must define Phase {phase}")
            continue
        if len(phase_sections) > 1:
            errors.append(
                f"docs/FUTURE_PHASES_DESIGN.md defines Phase {phase} more than once"
            )
        section = phase_sections[0]
        for field in FUTURE_PHASE_GOVERNANCE_FIELDS:
            if not _section_has_field(section, field):
                errors.append(
                    f"docs/FUTURE_PHASES_DESIGN.md Phase {phase} is missing "
                    f"required governance field: {field}"
                )

        status_match = re.search(
            r"^\*\*Status:\*\*\s+`?([^`\n]+)`?\s*$",
            section,
            flags=re.MULTILINE,
        )
        if phase == 7 and (
            status_match is None or status_match.group(1).strip() != PHASE7_STATUS
        ):
            errors.append(
                "docs/FUTURE_PHASES_DESIGN.md Phase 7 must declare status "
                f"'{PHASE7_STATUS}'"
            )

        phase_is_complete = "COMPLETE" in roadmap_declarations.get(phase, [])
        if phase_is_complete:
            for field in FUTURE_PHASE_COMPLETION_FIELDS:
                if not _section_has_field(section, field):
                    errors.append(
                        f"docs/FUTURE_PHASES_DESIGN.md Phase {phase} is COMPLETE but "
                        f"missing completion field: {field}"
                    )
    return errors


CANONICAL_COUNT_PATTERNS = {
    "README.md": re.compile(
        r"implementation remains (?P<implemented>\d+)/(?P<canonical>\d+) canonical tools"
        r"[^.\n]*all (?P<remaining>\d+) Phase 7 names "
        r"(?:remain(?:ing)? )?registered but unimplemented",
        re.IGNORECASE,
    ),
    "docs/CAPABILITIES.md": re.compile(
        r"registers (?P<canonical>\d+) canonical tool names\.\s+"
        rf"(?P<implemented>{NUMBER_WORD_PATTERN}|\d+) are implemented[^;]*;\s+"
        r"(?P<remaining>\d+) remain",
        re.IGNORECASE,
    ),
    "docs/ROADMAP.md": re.compile(
        r"\*\*Objective:\*\*[^\n]*implementation remains "
        r"(?P<implemented>\d+)/(?P<canonical>\d+) canonical tools"
        r"[^.\n]*all (?P<remaining>\d+) Phase 7 names "
        r"(?:remain(?:ing)? )?registered but unimplemented",
        re.IGNORECASE,
    ),
    "docs/FUTURE_PHASES_DESIGN.md": re.compile(
        r"\*\*Goal:\*\*[^\n]*implementation remains "
        r"(?P<implemented>\d+)/(?P<canonical>\d+) canonical tools"
        r"[^.\n]*all (?P<remaining>\d+) Phase 7 names "
        r"(?:remain(?:ing)? )?registered but unimplemented",
        re.IGNORECASE,
    ),
    "CHANGELOG.md": re.compile(
        r"Discovery now exposes (?P<canonical>\d+) canonical tools[^\n]*\.\s+"
        rf"(?P<implemented>{NUMBER_WORD_PATTERN}|\d+) canonical tools are implemented and "
        r"(?P<remaining>\d+) remain",
        re.IGNORECASE,
    ),
}


def _current_changelog_section(text: str) -> str:
    start = re.search(r"^## \[Unreleased\]\s*$", text, flags=re.MULTILINE)
    if start is None:
        return ""
    remainder = text[start.end():]
    end = re.search(r"^## \[", remainder, flags=re.MULTILINE)
    return remainder[:end.start()] if end is not None else remainder


def _without_phase7_excluded_contexts(text: str) -> str:
    clean = FENCED_CODE_PATTERN.sub("", text)
    lines: list[str] = []
    excluded_level: int | None = None
    for line in clean.splitlines():
        heading = re.match(r"^\s{0,3}(#{1,6})\s+(.+?)\s*#*\s*$", line)
        if heading is not None:
            level = len(heading.group(1))
            if excluded_level is not None and level <= excluded_level:
                excluded_level = None
            if re.search(
                r"\b(?:glossary|historical)\b",
                heading.group(2),
                flags=re.IGNORECASE,
            ):
                excluded_level = level
                continue
        if excluded_level is None:
            lines.append(line)
    return "\n".join(lines)


def _designated_phase7_block(
    text: str,
    start_marker: str,
    end_marker: str,
    relative_path: str,
    label: str,
) -> tuple[str | None, list[str]]:
    clean = _without_phase7_excluded_contexts(text)
    if clean.count(start_marker) != 1 or clean.count(end_marker) != 1:
        return None, [
            f"{relative_path}: must contain exactly one designated Phase 7 {label} block"
        ]
    start = clean.index(start_marker) + len(start_marker)
    end = clean.index(end_marker, start)
    return clean[start:end].strip(), []


def validate_phase7_reconciliation(
    texts: dict[str, str],
    expected_counts: tuple[int, int, int] = CANONICAL_IMPLEMENTATION_COUNTS,
) -> list[str]:
    errors: list[str] = []

    for relative_path in PHASE7_STATUS_DOCUMENTS:
        text = texts.get(relative_path)
        if text is None:
            continue
        source = _current_changelog_section(text) if relative_path == "CHANGELOG.md" else text
        block, block_errors = _designated_phase7_block(
            source,
            PHASE7_CURRENT_STATUS_START,
            PHASE7_CURRENT_STATUS_END,
            relative_path,
            "current-status",
        )
        errors.extend(block_errors)
        if block is None:
            continue
        match = PHASE7_CURRENT_STATUS_PATTERN.fullmatch(block)
        if match is None:
            errors.append(
                f"{relative_path}: Phase 7 current status block has invalid field structure"
            )
            continue
        if match.group("status") != PHASE7_STATUS:
            errors.append(
                f"{relative_path}: Phase 7 current status block must report status "
                f"{PHASE7_STATUS}"
            )
        implementation = (
            int(match.group("implemented")),
            int(match.group("canonical")),
            int(match.group("registered")),
            int(match.group("phase7_total")),
        )
        expected_canonical, expected_implemented, expected_remaining = expected_counts
        if implementation != (
            expected_implemented,
            expected_canonical,
            expected_remaining,
            PHASE7_NAME_COUNT,
        ):
            errors.append(
                f"{relative_path}: Phase 7 current status block must report "
                f"{expected_implemented}/{expected_canonical} canonical implementation and "
                f"{expected_remaining}/{PHASE7_NAME_COUNT} unimplemented registrations"
            )
        feasibility = (
            int(match.group("feasible")),
            int(match.group("feasibility_total")),
            int(match.group("blocked")),
        )
        if feasibility != (15, 18, 3):
            errors.append(
                f"{relative_path}: Phase 7 current status block must report feasibility "
                "as 15/18 implementation-feasible and 3/18 API-blocked"
            )

    for relative_path in PHASE7_CURRENT_STATE_DOCUMENTS:
        text = texts.get(relative_path)
        if text is None:
            continue
        source = _current_changelog_section(text) if relative_path == "CHANGELOG.md" else text
        source = _without_phase7_excluded_contexts(source)
        if re.search(r"\bPhase\s+7\s+is\s+planned\b", source, flags=re.IGNORECASE):
            errors.append(
                f"{relative_path}: stale Phase 7 planned prose in current-state documentation"
            )

    for relative_path, claims in PHASE7_STALE_DELIVERY_CLAIMS.items():
        raw_text = texts.get(relative_path)
        if raw_text is None:
            continue
        text = _without_phase7_excluded_contexts(raw_text)
        for pattern, description in claims:
            if pattern.search(text):
                errors.append(
                    f"{relative_path}: stale Phase 7 delivery prose: {description}"
                )

    readme = texts.get("README.md")
    if readme is not None and not all(
        target in readme
        for target in (
            "(docs/PHASE_7_API_FEASIBILITY.md)",
            "(docs/PHASE_7_IMPLEMENTATION_PLAN.md)",
        )
    ):
        errors.append("README.md must link both Phase 7 records")

    evidence = texts.get("docs/PHASE_7_API_FEASIBILITY.md")
    if evidence is not None:
        if not re.search(
            r"15/18\s+(?:are\s+)?implementation-feasible"
            r"[\s\S]*?3/18\s+(?:are\s+)?API-blocked",
            evidence,
            flags=re.IGNORECASE,
        ):
            errors.append(
                "docs/PHASE_7_API_FEASIBILITY.md: Phase 7 feasibility summary "
                "must report 15 feasible and 3 blocked"
            )
        for required in (
            "2026-08-29",
            "Godot 4.5.1",
            "Godot 4.7.2",
            "no supported public API/semantics satisfying the exact approved "
            "contract was found on either tested version",
        ):
            if required.lower() not in evidence.lower():
                errors.append(
                    "docs/PHASE_7_API_FEASIBILITY.md: missing feasibility fact: "
                    f"{required}"
                )

        rows = list(PHASE7_MATRIX_ROW_PATTERN.finditer(evidence))
        row_names = [match.group("tool") for match in rows]
        if (
            len(rows) != len(PHASE7_CANONICAL_TOOLS)
            or len(set(row_names)) != len(PHASE7_CANONICAL_TOOLS)
            or set(row_names) != set(PHASE7_CANONICAL_TOOLS)
        ):
            errors.append(
                "docs/PHASE_7_API_FEASIBILITY.md: matrix must contain exactly "
                "the 18 Phase 7 canonical names once each"
            )
        decisions = {
            match.group("tool"): match.group("decision")
            for match in rows
        }
        feasible_count = sum(
            decision == "GO" for decision in decisions.values()
        )
        blocked = {
            name for name, decision in decisions.items() if decision == "BLOCKED"
        }
        if feasible_count != 15 or len(blocked) != 3:
            errors.append(
                "docs/PHASE_7_API_FEASIBILITY.md: matrix must report "
                "15 feasible and 3 blocked"
            )
        if blocked != PHASE7_BLOCKED_TOOLS:
            expected = ", ".join(sorted(PHASE7_BLOCKED_TOOLS))
            errors.append(
                f"Phase 7 blocked set must be exactly: {expected}"
            )
        feasible_block, feasible_block_errors = _designated_phase7_block(
            evidence,
            PHASE7_FEASIBLE_NAMES_START,
            PHASE7_FEASIBLE_NAMES_END,
            "docs/PHASE_7_API_FEASIBILITY.md",
            "feasible-name",
        )
        errors.extend(feasible_block_errors)
        if feasible_block is not None:
            feasible_names = re.findall(r"`([a-z0-9_]+)`", feasible_block)
            matrix_go = {
                name for name, decision in decisions.items() if decision == "GO"
            }
            if (
                len(feasible_names) != len(set(feasible_names))
                or set(feasible_names) != matrix_go
            ):
                errors.append(
                    "docs/PHASE_7_API_FEASIBILITY.md: feasible-name list must "
                    "exactly equal matrix GO set"
                )

    # The plan must say what actually happened to it. It originally had to record
    # that the all-or-nothing gate stopped Tasks 2-13; once partial delivery was
    # authorized and the signal names shipped, that sentence became false, so the
    # rule now requires the plan to name the decision that superseded the gate.
    plan = texts.get("docs/PHASE_7_IMPLEMENTATION_PLAN.md")
    if plan is not None and not re.search(
        r"Tasks 2-13[^\n]*replaced by an explicit partial-delivery decision",
        plan,
        flags=re.IGNORECASE,
    ):
        errors.append(
            "docs/PHASE_7_IMPLEMENTATION_PLAN.md must record that the all-or-nothing "
            "gate originally stopped Tasks 2-13 and was replaced by an explicit "
            "partial-delivery decision"
        )
    return errors


def _parse_count(value: str) -> int:
    word = WORD_NUMBERS.get(value.lower())
    return word if word is not None else int(value)


def validate_canonical_implementation_counts(
    texts: dict[str, str], expected: tuple[int, int, int] = CANONICAL_IMPLEMENTATION_COUNTS
) -> list[str]:
    errors: list[str] = []
    observed: dict[str, tuple[int, int, int]] = {}
    for relative_path, pattern in CANONICAL_COUNT_PATTERNS.items():
        text = texts.get(relative_path)
        if text is None:
            continue
        source = _current_changelog_section(text) if relative_path == "CHANGELOG.md" else text
        match = pattern.search(source)
        if match is None:
            errors.append(
                f"{relative_path}: cannot parse canonical implementation counts"
            )
            continue

        canonical = int(match.group("canonical"))
        implemented = _parse_count(match.group("implemented"))
        remaining = int(match.group("remaining"))
        counts = (canonical, implemented, remaining)
        observed[relative_path] = counts
        if implemented + remaining != canonical:
            errors.append(
                f"{relative_path}: canonical implementation counts do not add up: "
                f"{implemented} implemented + {remaining} remaining != {canonical} canonical"
            )
        if counts != expected:
            errors.append(
                f"{relative_path}: canonical implementation counts must be {expected[0]} canonical, "
                f"{expected[1]} implemented, and {expected[2]} remaining; "
                f"found {canonical}, {implemented}, {remaining}"
            )
        if "target" in match.groupdict() and (
            int(match.group("target")) != canonical
            or int(match.group("target_canonical")) != canonical
        ):
            errors.append(
                f"{relative_path}: canonical completion target must equal {canonical}/{canonical}"
            )

    if len(set(observed.values())) > 1:
        details = ", ".join(
            f"{path}={counts[0]}/{counts[1]}/{counts[2]}"
            for path, counts in observed.items()
        )
        errors.append(
            f"canonical implementation counts disagree across current documents: {details}"
        )
    return errors


def _include_markdown(root: Path, path: Path) -> bool:
    relative_parts = path.relative_to(root).parts
    return not any(part in {".git", ".worktrees", ".superpowers"} for part in relative_parts) and not any(
        part.startswith("build") for part in relative_parts
    )


def _tracked_paths(
    root: Path,
    relative_paths: tuple[str, ...],
) -> tuple[set[str], str | None]:
    try:
        result = subprocess.run(
            ["git", "-C", str(root), "ls-files", "-z", "--", *relative_paths],
            check=False,
            capture_output=True,
        )
    except OSError as error:
        return set(), f"git ls-files failed while checking forbidden artifacts: {error}"
    if result.returncode != 0:
        detail = result.stderr.decode("utf-8", errors="replace").strip()
        suffix = f": {detail}" if detail else f" with exit code {result.returncode}"
        return set(), f"git ls-files failed while checking forbidden artifacts{suffix}"
    paths = result.stdout.decode("utf-8", errors="surrogateescape").split("\0")
    return {path for path in paths if path}, None



def validate_capability_name_tables(
    root: Path, names: dict[str, list[str]]
) -> list[str]:
    """Diff the documented unimplemented name list against the tool manifest.

    The count checks alone let a shipped tool keep sitting in the unimplemented
    table: the four signal names went live and the totals still added up, so CI
    stayed green while CAPABILITIES.md said calls to them were rejected two
    paragraphs above saying they were delivered.
    """
    errors: list[str] = []
    unimplemented = names.get("unimplemented")
    implemented = names.get("implemented")
    if unimplemented is None or implemented is None:
        return ["tool manifest has no 'names.unimplemented'/'names.implemented' arrays"]

    expected_unimplemented = set(unimplemented)
    implemented_set = set(implemented)

    path = root / "docs" / "CAPABILITIES.md"
    if not path.is_file():
        return ["docs/CAPABILITIES.md: not found"]
    text = path.read_text(encoding="utf-8")

    row = None
    for line in text.splitlines():
        if line.startswith("| `unimplemented` |"):
            row = line
            break
    if row is None:
        return ["docs/CAPABILITIES.md: no `unimplemented` row in the execution-mode table"]

    documented = set(re.findall(r"`([a-z0-9_]+)`", row.split("|")[2]))
    shipped_but_listed = sorted(documented & implemented_set)
    if shipped_but_listed:
        errors.append(
            "docs/CAPABILITIES.md: the unimplemented row lists tools the binary "
            f"reports as implemented: {', '.join(shipped_but_listed)}"
        )
    missing = sorted(expected_unimplemented - documented)
    if missing:
        errors.append(
            "docs/CAPABILITIES.md: the unimplemented row omits reserved tools: "
            f"{', '.join(missing)}"
        )
    return errors


def load_tool_manifest(
    path: Path,
) -> tuple[dict[str, int] | None, dict[str, list[str]], list[str]]:
    """Read the counts emitted by `didi --dump-tool-manifest`.

    The documentation contract is validated against the software, so a missing
    or malformed manifest is a hard failure rather than a reason to skip the
    count checks.
    """
    if not path.is_file():
        return None, {}, [
            f"{path}: tool manifest not found. Build didi and generate it with: "
            f"didi --dump-tool-manifest > {path}"
        ]
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        return None, {}, [f"{path}: cannot read tool manifest: {error}"]

    counts = document.get("counts")
    if not isinstance(counts, dict):
        return None, {}, [f"{path}: tool manifest has no 'counts' object"]
    missing = [k for k in TOOL_MANIFEST_COUNT_KEYS if not isinstance(counts.get(k), int)]
    if missing:
        return None, {}, [f"{path}: tool manifest is missing integer counts: {', '.join(missing)}"]
    if counts["canonical"] + counts["legacy"] != counts["total"]:
        return None, {}, [f"{path}: tool manifest total does not equal canonical + legacy"]
    if counts["implemented"] + counts["unimplemented"] != counts["canonical"]:
        return None, {}, [
            f"{path}: tool manifest canonical does not equal implemented + unimplemented"
        ]
    raw_names = document.get("names")
    names: dict[str, list[str]] = {}
    if isinstance(raw_names, dict):
        for key, value in raw_names.items():
            if isinstance(value, list) and all(isinstance(item, str) for item in value):
                names[key] = value
    return counts, names, []
CURRENT_PROJECT_MANIFESTS = (
    "demo/project.godot",
    "tests/godot_smoke/project.godot",
)
CURRENT_PROJECT_FIXTURE_EXCEPTIONS = (
    "tests/phase7_contract_probe/project.godot",
    "tests/phase7_signal_bridge/project.godot",
)
HISTORICAL_PROJECT_MANIFESTS = (
    "tests/phase7_feasibility/project.godot",
)
CURRENT_PHASE7_PLANS = (
    "docs/PHASE_7_PARTIAL_IMPLEMENTATION_PLAN.md",
)


def validate_tool_surface_counts(
    texts: dict[str, str], counts: dict[str, int]
) -> list[str]:
    """Assert every document states the counts the built binary actually has."""
    errors: list[str] = []
    for relative_path, requirements in COUNT_FACTS.items():
        text = texts.get(relative_path)
        if text is None:
            continue
        for count_key, template, label in requirements:
            number = counts[count_key]
            pattern = template.format(n=number, w=NUMBER_WORDS.get(number, str(number)))
            if not re.search(pattern, text, flags=re.IGNORECASE):
                errors.append(
                    f"{relative_path}: must state {number} {label} "
                    "to match the built tool surface"
                )
    return errors


def validate_addon_copies_match(root: Path) -> list[str]:
    """The demo project's addon copy must match the canonical one.

    A Godot project cannot reference an addon outside its own `res://`, so the
    demo needs its own copy. Nothing checked that copy, and it fell two minor
    versions behind and kept pre-arch-suffix macOS library paths after the
    canonical copy gained per-architecture entries. A user opening the demo was
    running a different addon from the one Didi ships.
    """
    errors: list[str] = []
    for name in ("plugin.cfg", "didi.gdextension", "didi_plugin.gd"):
        canonical = root / "addons" / "didi" / name
        demo = root / "demo" / "addons" / "didi" / name
        if not canonical.is_file():
            errors.append(f"addons/didi/{name}: canonical addon file is missing")
            continue
        if not demo.is_file():
            errors.append(f"demo/addons/didi/{name}: demo addon copy is missing")
            continue
        # Compare content, ignoring line-ending differences between checkouts.
        canonical_text = canonical.read_text(encoding="utf-8").splitlines()
        demo_text = demo.read_text(encoding="utf-8").splitlines()
        if canonical_text != demo_text:
            errors.append(
                f"demo/addons/didi/{name} does not match addons/didi/{name}. "
                "The demo ships a copy of the addon; keep the two identical so the "
                "demo project exercises what Didi actually distributes."
            )
    return errors


def validate_python_tests_are_registered(root: Path) -> list[str]:
    """Every Python test module must be named by some workflow.

    CI runs named `unittest` modules rather than discovering the tests
    directory, so a new file under `tests/` runs nowhere until someone adds a
    step for it. That failure is silent in the worst way: the suite passes
    locally, so nothing looks wrong, and it provides none of the protection it
    appears to.

    `tests/test_phase7_signal_admission.py` sat in exactly that state -- it
    asserts the Phase 7 signal test seam never reaches a shipping build, and it
    executed in no job at all.
    """
    errors: list[str] = []
    workflows = root / ".github" / "workflows"
    if not workflows.is_dir():
        return errors
    referenced = "\n".join(
        path.read_text(encoding="utf-8", errors="replace")
        for path in sorted(workflows.glob("*.yml"))
    )
    for path in sorted((root / "tests").glob("test_*.py")):
        module = f"tests.{path.stem}"
        if module not in referenced:
            errors.append(
                f"{module} is named by no workflow, so it never runs in CI. "
                "Add a step that invokes it, or delete it -- a test that does not "
                "execute is worse than no test, because it looks like coverage."
            )
    return errors


def validate_repository(root: Path, tool_manifest: Path | None = None) -> list[str]:
    root = root.resolve()
    errors: list[str] = []
    texts: dict[str, str] = {}

    tracked_paths, tracked_paths_error = _tracked_paths(
        root,
        TRACKED_ONLY_ARTIFACT_PATHS,
    )
    if tracked_paths_error is not None:
        errors.append(f"documentation validator: {tracked_paths_error}")
    for relative_path in TRACKED_ONLY_ARTIFACT_PATHS:
        if any(
            path == relative_path or path.startswith(f"{relative_path}/")
            for path in tracked_paths
        ):
            errors.append(
                f"{relative_path}: agent workflow artifacts must not be committed to the project"
            )

    for relative_path in FILESYSTEM_FORBIDDEN_ARTIFACT_PATHS:
        if (root / relative_path).exists():
            errors.append(
                f"{relative_path}: agent workflow artifacts must not exist in the project"
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
            "addons/didi/plugin.cfg": f'version="{version}"',
            "demo/addons/didi/plugin.cfg": f'version="{version}"',
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

        for relative_path in GENERATED_VERSION_FILES:
            text = _read_required(root, relative_path, errors)
            if text is not None and version in text:
                errors.append(
                    f"{relative_path}: spells out version {version}. It reads "
                    "didi::kProjectVersion from the generated header, and a literal here "
                    "is the duplication that header exists to remove."
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

    expected_counts = CANONICAL_IMPLEMENTATION_COUNTS
    if tool_manifest is not None:
        manifest_counts, manifest_names, manifest_errors = load_tool_manifest(tool_manifest)
        errors.extend(manifest_errors)
        if manifest_counts is not None:
            expected_counts = (
                manifest_counts["canonical"],
                manifest_counts["implemented"],
                manifest_counts["unimplemented"],
            )
            manifest_triple = (
                manifest_counts["canonical"],
                manifest_counts["implemented"],
                manifest_counts["unimplemented"],
            )
            errors.extend(validate_capability_name_tables(root, manifest_names))
            if manifest_triple != CANONICAL_IMPLEMENTATION_COUNTS:
                errors.append(
                    "tools/validate_documentation.py: CANONICAL_IMPLEMENTATION_COUNTS is "
                    f"{CANONICAL_IMPLEMENTATION_COUNTS} but the built tool surface is "
                    f"{manifest_triple}. Update the fallback so the docs-only job, which "
                    "has no binary to read, checks the same numbers as the build jobs."
                )
            errors.extend(validate_tool_surface_counts(texts, manifest_counts))
    errors.extend(validate_canonical_implementation_counts(texts, expected_counts))
    errors.extend(validate_phase7_reconciliation(texts, expected_counts))
    errors.extend(validate_addon_copies_match(root))
    errors.extend(validate_python_tests_are_registered(root))

    roadmap_text = texts.get("docs/ROADMAP.md")
    if roadmap_text is not None:
        errors.extend(validate_future_phase_roadmap(roadmap_text))

    design_text = texts.get("docs/FUTURE_PHASES_DESIGN.md")
    if design_text is not None:
        errors.extend(validate_future_phase_governance(design_text, roadmap_text))

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
    parser.add_argument(
        "--tool-manifest",
        type=Path,
        default=None,
        help=(
            "JSON emitted by `didi --dump-tool-manifest`. When supplied, every "
            "published tool count is validated against the built binary instead "
            "of against other documentation."
        ),
    )
    arguments = parser.parse_args(argv)
    errors = validate_repository(arguments.root, arguments.tool_manifest)
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
