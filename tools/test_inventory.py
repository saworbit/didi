#!/usr/bin/env python3
"""Derive the published test counts from the suites themselves.

The number of tests in this project used to be a sentence somebody typed into
a guide. It went stale, stayed stale long enough that nobody trusted it, and
`tools/validate_documentation.py` ended up carrying a rule whose entire job is
to forbid one specific out-of-date sentence about it. A count that has to be
policed by a denylist is a count nobody is maintaining.

This tool derives every number instead:

* **Native C++**: asks the built ``didi_tests`` binary for its own registry
  with ``--list``. The registry is what the runner iterates, so the count is
  the count, not an estimate of it.
* **Python**: walks ``tests/test_*.py`` with :mod:`ast` and counts the
  ``test_*`` methods on every ``unittest.TestCase`` subclass. Parsing rather
  than importing, so the count does not depend on ``jsonschema`` being
  installed or on a test module having import side effects.
* **PowerShell**: counts the ``Assert-True`` calls in the live Godot harness
  and its helper scripts. That harness is one long scenario rather than a set
  of named cases, so it is reported as assertions and labelled as assertions.
  Overstating it as "tests" would be the same failure in a new place.

Usage::

    python tools/test_inventory.py                 # regenerate the inventory
    python tools/test_inventory.py --check         # fail if it is out of date
    python tools/test_inventory.py --json          # machine-readable counts

``--check`` is what CI runs, after the build, with ``DIDI_TEST_BINARY``
pointing at the binary it just produced.
"""

from __future__ import annotations

import argparse
import ast
import json
import os
import platform
import re
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent

# The native suite is platform-conditional, and not by a little: crash capture
# is Windows-only, and the IPC tests differ because named pipes and Unix
# sockets are not the same transport. A single committed native total is
# therefore false on two platforms out of three, and the first version of this
# tool published one anyway -- Windows numbers, which the macOS runner
# immediately and correctly rejected.
#
# So the page names its platform. Windows is the reference because it is the
# only platform with the live Godot integration harness, and it runs the
# largest native suite, so the published figure understates nothing.
REFERENCE_PLATFORM = "Windows"
INVENTORY_PATH = REPO_ROOT / "docs" / "TEST_INVENTORY.md"
README_PATH = REPO_ROOT / "README.md"

# The tests badge is located by its own shields.io URL. Comment markers around
# it were tried first and had to go: see apply_badge for what they did to the
# rendered README.
BADGE_PATTERN = re.compile(
    r"(?P<prefix>https://img\.shields\.io/badge/tests-)(?P<count>\d+)(?P<suffix>-)"
)

# Where a build can plausibly be. Ordered by how likely it is to be the one the
# caller means; every candidate is still checked so a wrong guess reports the
# whole list rather than a bare "not found".
BINARY_CANDIDATES = (
    "build/didi_tests",
    "build/Release/didi_tests.exe",
    "build/Debug/didi_tests.exe",
    "build/didi_tests.exe",
    "build-ninja/didi_tests",
    "build-ninja/didi_tests.exe",
    "build-ninja/Release/didi_tests.exe",
)

# The helper scripts are dot-sourced by the harness rather than run on their
# own, so their assertions count towards the same total.
POWERSHELL_GLOB = "*.ps1"


@dataclass
class Group:
    """One named bucket of tests within a suite."""

    name: str
    count: int
    note: str = ""


@dataclass
class Suite:
    """One test suite, its total, and how that total breaks down."""

    name: str
    unit: str
    total: int
    how: str
    groups: list[Group] = field(default_factory=list)


class InventoryError(RuntimeError):
    """Raised when a count cannot be derived, rather than guessed at."""


def resolve_test_binary(explicit: str | None = None) -> Path:
    """Return the ``didi_tests`` binary to interrogate.

    ``DIDI_TEST_BINARY`` wins, then ``--test-binary``, then the build
    directories. CI sets the environment variable to the binary it has just
    built, which is the only way to be certain the count describes the code
    under review rather than whatever was lying around.
    """

    from_env = os.environ.get("DIDI_TEST_BINARY")
    for candidate in (from_env, explicit):
        if not candidate:
            continue
        path = Path(candidate)
        if not path.is_absolute():
            path = (REPO_ROOT / path).resolve()
        if path.is_file():
            return path
        raise InventoryError(f"Test binary does not exist: {path}")

    for relative in BINARY_CANDIDATES:
        path = REPO_ROOT / relative
        if path.is_file():
            return path

    searched = "\n  ".join(BINARY_CANDIDATES)
    raise InventoryError(
        "No didi_tests binary found. Build one, or point DIDI_TEST_BINARY at it.\n"
        "  cmake -B build -S . && cmake --build build --config Release\n"
        f"Searched:\n  {searched}"
    )


def native_test_names(binary: Path) -> list[str]:
    """Return the registered native test names, from the binary's own registry."""

    try:
        completed = subprocess.run(
            [str(binary), "--list"],
            capture_output=True,
            text=True,
            timeout=120,
            cwd=REPO_ROOT,
        )
    except OSError as error:
        raise InventoryError(f"Could not run {binary}: {error}") from error

    if completed.returncode != 0:
        raise InventoryError(
            f"{binary} --list exited {completed.returncode}: "
            f"{completed.stderr.strip() or '(no stderr)'}"
        )

    names = [line.strip() for line in completed.stdout.splitlines() if line.strip()]
    if not names:
        raise InventoryError(f"{binary} --list printed no test names")
    return names


def native_suite_prefix(name: str) -> str:
    """Return the suite a native test name belongs to.

    Most names are ``Suite.CaseName``. A handful were registered as a sentence
    ("Phase7Signals partial delivery contract"), so the first word stands in
    for the suite rather than dropping them into an ``(ungrouped)`` bucket that
    tells a reader nothing.
    """

    head = name.split(".", 1)[0]
    return head.split()[0] if head.split() else name


def native_suite(binary: Path) -> Suite:
    """Group the native tests by the suite prefix each name already carries."""

    names = native_test_names(binary)
    counts: dict[str, int] = {}
    for name in names:
        prefix = native_suite_prefix(name)
        counts[prefix] = counts.get(prefix, 0) + 1

    return Suite(
        name="Native C++ suite (`didi_tests`)",
        unit="tests",
        total=len(names),
        how="`didi_tests --list`, which prints the registry the runner iterates.",
        groups=[Group(name, count) for name, count in sorted(counts.items())],
    )


def _base_names(node: ast.ClassDef) -> list[str]:
    """Return the final name segment of each base of *node*.

    ``live.ManagedRecoveryLive`` reduces to ``ManagedRecoveryLive`` and
    ``unittest.TestCase`` to ``TestCase``, so a base can be recognised without
    resolving the import that produced it.
    """

    names: list[str] = []
    for base in node.bases:
        if isinstance(base, ast.Attribute):
            names.append(base.attr)
        elif isinstance(base, ast.Name):
            names.append(base.id)
    return names


def class_shapes(source: str) -> dict[str, tuple[list[str], int]]:
    """Return ``{class name: (base names, count of test methods)}`` for *source*."""

    shapes: dict[str, tuple[list[str], int]] = {}
    for node in ast.walk(ast.parse(source)):
        if not isinstance(node, ast.ClassDef):
            continue
        methods = sum(
            1
            for member in node.body
            if isinstance(member, (ast.FunctionDef, ast.AsyncFunctionDef))
            and member.name.startswith("test")
        )
        shapes[node.name] = (_base_names(node), methods)
    return shapes


def count_python_tests(source: str, known_test_cases: frozenset[str] = frozenset()) -> int:
    """Count ``test_*`` methods on ``unittest.TestCase`` subclasses in *source*.

    Parsing, not importing. A test module that needs ``jsonschema`` or sets up
    a temporary Godot project on import would otherwise make the count depend
    on the machine doing the counting.

    Inheritance is resolved to a fixed point, within the module and against
    *known_test_cases* from the rest of the suite. This is not a nicety:
    ``tests/test_managed_recovery_adversarial.py`` derives from a case defined
    in a sibling module, and a first version of this tool counted it as zero
    -- publishing a total that quietly omitted a whole file of tests.
    """

    shapes = class_shapes(source)
    known = set(known_test_cases)
    for name, (bases, _methods) in shapes.items():
        if any(base.endswith("TestCase") for base in bases):
            known.add(name)

    changed = True
    while changed:
        changed = False
        for name, (bases, _methods) in shapes.items():
            if name not in known and any(base in known for base in bases):
                known.add(name)
                changed = True

    return sum(methods for name, (_bases, methods) in shapes.items() if name in known)


def python_suite(root: Path = REPO_ROOT) -> Suite:
    """Count the Python contract tests, grouped by module."""

    paths = sorted((root / "tests").glob("test_*.py"))
    sources = {path: path.read_text(encoding="utf-8") for path in paths}

    # Names of every class that is a test case anywhere in the suite, so a
    # module that subclasses across file boundaries still resolves.
    known: set[str] = set()
    changed = True
    while changed:
        changed = False
        for source in sources.values():
            for name, (bases, _methods) in class_shapes(source).items():
                if name in known:
                    continue
                if any(base.endswith("TestCase") or base in known for base in bases):
                    known.add(name)
                    changed = True

    groups: list[Group] = []
    total = 0
    for path in paths:
        count = count_python_tests(sources[path], frozenset(known))
        if count == 0:
            continue
        groups.append(Group(path.name, count))
        total += count

    return Suite(
        name="Python contract suites (`tests/test_*.py`)",
        unit="tests",
        total=total,
        how="`test_*` methods on every `unittest.TestCase` subclass, read with `ast`.",
        groups=groups,
    )


def count_powershell_assertions(source: str) -> int:
    """Count ``Assert-True`` call sites, ignoring the function's own definition."""

    total = 0
    for line in source.splitlines():
        stripped = line.strip()
        if stripped.startswith("#"):
            continue
        if re.match(r"^function\s+Assert-True\b", stripped, flags=re.IGNORECASE):
            continue
        total += len(re.findall(r"\bAssert-True\b", line))
    return total


def powershell_suite(root: Path = REPO_ROOT) -> Suite:
    """Count the assertions in the live Godot harness and its helpers."""

    groups: list[Group] = []
    total = 0
    for path in sorted((root / "tests").glob(POWERSHELL_GLOB)):
        count = count_powershell_assertions(path.read_text(encoding="utf-8"))
        if count == 0:
            continue
        groups.append(Group(path.name, count))
        total += count

    return Suite(
        name="Live Godot harness (`tests/*.ps1`)",
        unit="assertions",
        total=total,
        how=(
            "`Assert-True` call sites. The harness is one long live scenario "
            "rather than a set of named cases, so this counts assertions and "
            "says so."
        ),
        groups=groups,
    )


def collect(binary: Path | None) -> list[Suite]:
    """Gather every suite. ``binary`` of ``None`` omits the native suite."""

    suites: list[Suite] = []
    if binary is not None:
        suites.append(native_suite(binary))
    suites.append(python_suite())
    suites.append(powershell_suite())
    return suites


def apply_badge(readme_text: str, test_total: int) -> str:
    """Rewrite the count inside the tests badge URL, or raise if it is absent.

    The badge is found by its own URL. An earlier version wrapped it in
    ``<!-- test-count:start -->`` markers, which broke the rendered page: a
    line beginning with ``<!--`` is a raw HTML block in CommonMark, so GitHub
    emitted the entire badge line as literal text and split the badge row into
    two paragraphs around it. Nothing about the number was wrong; the marker
    itself was the defect.
    """

    updated, replacements = BADGE_PATTERN.subn(
        lambda match: f"{match.group('prefix')}{test_total}{match.group('suffix')}",
        readme_text,
    )
    if replacements == 0:
        raise InventoryError(
            "README.md has no tests badge to update. Expected a shields.io URL "
            "of the form https://img.shields.io/badge/tests-<count>-<colour>."
        )
    if replacements > 1:
        raise InventoryError(
            f"README.md has {replacements} tests badges. One of them would go "
            "stale without anything noticing, so this refuses to guess."
        )
    return updated


def render_inventory(suites: list[Suite]) -> str:
    """Render the Markdown page. Deterministic: same suites, same bytes."""

    test_total = sum(suite.total for suite in suites if suite.unit == "tests")
    assertion_total = sum(suite.total for suite in suites if suite.unit == "assertions")

    lines: list[str] = []
    lines.append("# Test Inventory")
    lines.append("")
    lines.append(
        "**Generated file. Do not edit by hand.** Regenerate with "
        "`python tools/test_inventory.py`; CI runs `--check` after the build and "
        "fails when this page and the suites disagree."
    )
    lines.append("")
    lines.append(
        "Every number here is derived from the suites themselves rather than "
        "written down beside them. The native total comes from the test "
        "binary's own registry, the Python totals from parsing the test "
        "modules, and the harness total from counting its assertions. A test "
        "added without a line appearing here means the generator is wrong, "
        "which is a defect worth knowing about."
    )
    lines.append("")
    lines.append(
        f"**These are the {REFERENCE_PLATFORM} figures.** The native suite is "
        "platform-conditional and the difference is not small: crash capture "
        "is Windows-only, and the IPC cases differ because a named pipe and a "
        "Unix socket are not the same transport, so the POSIX runners register "
        "roughly a dozen fewer native tests. Every platform runs the whole "
        "Python suite. Windows is the reference here because it is the only "
        "platform with the live Godot integration harness, and because it runs "
        "the largest native suite, so nothing below is an overstatement of "
        "what another platform does."
    )
    lines.append("")
    lines.append("## Totals")
    lines.append("")
    lines.append("| Measure | Count |")
    lines.append("| --- | ---: |")
    lines.append(f"| Automated tests | **{test_total}** |")
    lines.append(f"| Live-harness assertions | {assertion_total} |")
    lines.append("")
    lines.append(
        "The badge in the [README](../README.md) shows the automated test "
        "total. Harness assertions are counted separately because they are "
        "assertions inside one live scenario, not independently runnable "
        "cases; adding them together would flatter the number."
    )
    lines.append("")

    for suite in suites:
        lines.append(f"## {suite.name}")
        lines.append("")
        lines.append(f"**{suite.total} {suite.unit}.** Derived from {suite.how}")
        lines.append("")
        heading = "Suite" if suite.unit == "tests" else "Script"
        if suite.groups and suite.groups[0].name.endswith((".py", ".ps1")):
            heading = "File"
        lines.append(f"| {heading} | {suite.unit.capitalize()} |")
        lines.append("| --- | ---: |")
        for group in suite.groups:
            lines.append(f"| `{group.name}` | {group.count} |")
        lines.append("")

    lines.append("## What is not counted here")
    lines.append("")
    lines.append(
        "- The end-to-end MCP conversation in `.github/workflows/ci.yml`, "
        "which drives the built binary over stdio and asserts the wire "
        "surface against the manifest that same binary emits."
    )
    lines.append(
        "- `tools/validate_documentation.py`, which checks the documentation "
        "contract rather than the code, and reports its own findings."
    )
    lines.append(
        "- Compiler and sanitizer diagnostics. ASan and UBSan run the native "
        "suite again under instrumentation; they add coverage, not cases."
    )
    lines.append(
        "- The libFuzzer targets in `fuzz/`. They generate their own inputs "
        "rather than asserting a fixed set, so counting them as tests would "
        "be counting the wrong thing: three targets is not three cases, and "
        "the number that matters is the corpus, which grows on its own."
    )
    lines.append("")

    return "\n".join(lines) + "\n"


def _report_difference(label: str, expected: str, actual: str) -> None:
    import difflib

    diff = difflib.unified_diff(
        actual.splitlines(keepends=True),
        expected.splitlines(keepends=True),
        fromfile=f"{label} (committed)",
        tofile=f"{label} (derived)",
        n=2,
    )
    sys.stderr.write("".join(diff))


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--check",
        action="store_true",
        help="Fail if the committed inventory or README badge is out of date.",
    )
    parser.add_argument(
        "--json",
        action="store_true",
        help="Print the derived counts as JSON and write nothing.",
    )
    parser.add_argument(
        "--test-binary",
        help="Path to didi_tests. Defaults to DIDI_TEST_BINARY, then the build directories.",
    )
    parser.add_argument(
        "--no-native",
        action="store_true",
        help="Skip the native suite. Only for environments with no build at all.",
    )
    args = parser.parse_args(argv)

    # The platform gate comes first, before anything looks for a binary. On a
    # platform this page does not describe there is nothing to check and
    # nothing safe to write, and that is true whether or not a build exists --
    # so demanding one first would report a missing build as the problem when
    # the real answer is "not here". It did exactly that on the lint runner,
    # which has no build at all.
    current = platform.system()
    if current != REFERENCE_PLATFORM and not args.json:
        if args.check:
            print(
                f"test_inventory: skipped on {current}. The published counts are "
                f"the {REFERENCE_PLATFORM} figures, and the native suite is "
                "platform-conditional, so there is nothing here to compare "
                f"against. The {REFERENCE_PLATFORM} job checks them."
            )
            return 0
        print(
            f"test_inventory: refusing to regenerate on {current}. The page "
            f"publishes the {REFERENCE_PLATFORM} figures and this platform "
            "registers a different set of native tests, so writing here would "
            f"replace them with numbers the page does not claim. Regenerate on "
            f"{REFERENCE_PLATFORM}, or use --json to inspect this platform.",
            file=sys.stderr,
        )
        return 2

    try:
        binary = None if args.no_native else resolve_test_binary(args.test_binary)
        suites = collect(binary)
    except InventoryError as error:
        print(f"test_inventory: {error}", file=sys.stderr)
        return 2

    if args.json:
        payload = {
            "tests": sum(s.total for s in suites if s.unit == "tests"),
            "assertions": sum(s.total for s in suites if s.unit == "assertions"),
            "suites": [
                {
                    "name": suite.name,
                    "unit": suite.unit,
                    "total": suite.total,
                    "groups": {group.name: group.count for group in suite.groups},
                }
                for suite in suites
            ],
        }
        print(json.dumps(payload, indent=2))
        return 0

    if args.no_native:
        print(
            "test_inventory: refusing to write an inventory with no native suite; "
            "--no-native is for --json only.",
            file=sys.stderr,
        )
        return 2

    inventory = render_inventory(suites)
    test_total = sum(suite.total for suite in suites if suite.unit == "tests")
    readme = README_PATH.read_text(encoding="utf-8")

    try:
        updated_readme = apply_badge(readme, test_total)
    except InventoryError as error:
        print(f"test_inventory: {error}", file=sys.stderr)
        return 2

    if args.check:
        problems = 0
        committed = INVENTORY_PATH.read_text(encoding="utf-8") if INVENTORY_PATH.exists() else ""
        if committed != inventory:
            print(
                f"test_inventory: {INVENTORY_PATH.relative_to(REPO_ROOT).as_posix()} "
                "is out of date. Run `python tools/test_inventory.py`.",
                file=sys.stderr,
            )
            _report_difference("docs/TEST_INVENTORY.md", inventory, committed)
            problems += 1
        if readme != updated_readme:
            print(
                f"test_inventory: the README tests badge says something other than "
                f"{test_total}. Run `python tools/test_inventory.py`.",
                file=sys.stderr,
            )
            problems += 1
        if problems:
            return 1
        print(f"test_inventory: {test_total} tests, inventory and badge agree.")
        return 0

    # `newline=""` writes the "\n" already in the rendered text verbatim. The
    # default would translate it to "\r\n" on Windows and rewrite every line of
    # the README on the way past, which .gitattributes then normalises back --
    # a whole-file diff in the working tree for no change at all.
    INVENTORY_PATH.write_text(inventory, encoding="utf-8", newline="")
    if readme != updated_readme:
        README_PATH.write_text(updated_readme, encoding="utf-8", newline="")
    print(
        f"test_inventory: wrote {INVENTORY_PATH.relative_to(REPO_ROOT).as_posix()} "
        f"({test_total} tests)."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
