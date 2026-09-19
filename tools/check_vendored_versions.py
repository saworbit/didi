#!/usr/bin/env python3
"""Watch the vendored single-header libraries Dependabot cannot see.

`THIRD_PARTY.md` ends with an admission: the three files in `include/` are
"reviewed by hand or not at all". They are copied sources rather than package
manager entries, so nothing resolves them, nothing bumps them, and nothing
notices when upstream ships a release. That is the whole gap this tool closes.

It is deliberately not a version number typed into a table and trusted. A
recorded version drifts from the file it describes the first time somebody
updates one and forgets the other, and a table nobody checks is the same
problem as a dependency nobody checks. So two things are verified:

* **The table agrees with the file.** Each vendored header states its own
  version in its upstream banner. That string is read out of the file on disk
  and compared with the row in `THIRD_PARTY.md`. This needs no network, is
  deterministic, and is what `tests/test_vendored_versions.py` asserts on every
  run of the documentation suite.
* **The file agrees with upstream.** For the sources that publish a version
  somewhere machine-readable, the current upstream version is fetched and
  compared. This needs the network, so it runs on a schedule in
  `.github/workflows/supply-chain.yml` rather than in a required check --
  a rate-limited API is not a reason to make a pull request unmergeable.

Not every vendored file can be tracked, and the ones that cannot say so out
loud rather than being quietly omitted. `gdextension_interface.h` is a
compatibility contract, not a version to chase: `addons/didi/didi.gdextension`
declares `compatibility_minimum = "4.5"` and the live harness runs against real
4.5.1 and 4.7.2 editors, so "is there a newer Godot?" is the wrong question and
answering it weekly would train everyone to ignore the answer.

Usage::

    python tools/check_vendored_versions.py            # report, network
    python tools/check_vendored_versions.py --offline  # table vs file only
    python tools/check_vendored_versions.py --check    # exit 1 on any finding
    python tools/check_vendored_versions.py --json     # machine-readable

`--check --offline` is what the test suite runs. `--check` on its own is what
the scheduled workflow runs.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
import urllib.error
import urllib.request
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Callable, Sequence

REPO_ROOT = Path(__file__).resolve().parent.parent
THIRD_PARTY_PATH = REPO_ROOT / "THIRD_PARTY.md"

# Long enough to survive a slow response, short enough that a hung endpoint
# does not hold a scheduled job open for its whole timeout budget.
NETWORK_TIMEOUT_SECONDS = 20


class VersionLookupError(RuntimeError):
    """Upstream could not be asked. Distinct from upstream answering `behind`."""


@dataclass(frozen=True)
class Finding:
    """One thing a human needs to do something about."""

    source: str
    kind: str
    detail: str


@dataclass
class SourceReport:
    """What is known about one vendored file after a run."""

    source: str
    path: str
    file_version: str | None = None
    table_version: str | None = None
    upstream_version: str | None = None
    tracked: bool = True
    note: str = ""
    findings: list[Finding] = field(default_factory=list)


def _fetch(url: str) -> str:
    """GET *url* as text, authenticating when a token happens to be around.

    Unauthenticated GitHub API calls are rate limited per IP, which on a shared
    runner is a limit somebody else may already have spent. `GITHUB_TOKEN` is
    present in Actions by default, so using it costs nothing and moves the
    limit from 60 an hour to 5000.
    """
    request = urllib.request.Request(url, headers={"User-Agent": "didi-vendored-check"})
    token = os.environ.get("GITHUB_TOKEN", "").strip()
    if token and url.startswith("https://api.github.com/"):
        request.add_header("Authorization", f"Bearer {token}")
        request.add_header("X-GitHub-Api-Version", "2022-11-28")
    try:
        with urllib.request.urlopen(request, timeout=NETWORK_TIMEOUT_SECONDS) as response:
            return response.read().decode("utf-8", errors="replace")
    except (urllib.error.URLError, TimeoutError, OSError) as error:
        raise VersionLookupError(f"{url}: {error}") from error


def github_latest_release(repo: str) -> Callable[[], str]:
    """Upstream version is the tag of the newest published release."""

    def lookup() -> str:
        payload = _fetch(f"https://api.github.com/repos/{repo}/releases/latest")
        try:
            tag = json.loads(payload)["tag_name"]
        except (ValueError, KeyError) as error:
            raise VersionLookupError(f"{repo}: unreadable release payload") from error
        return str(tag)

    return lookup


def raw_file_banner(repo: str, ref: str, path: str, pattern: str) -> Callable[[], str]:
    """Upstream version is stated in the file itself, not in a release.

    `nothings/stb` publishes no releases and tags nothing: the version lives in
    the header's first line and is bumped in place. Asking the releases API
    about it returns nothing at all, which would read as "up to date" forever.
    """

    def lookup() -> str:
        text = _fetch(f"https://raw.githubusercontent.com/{repo}/{ref}/{path}")
        found = re.search(pattern, text)
        if not found:
            raise VersionLookupError(f"{repo}/{path}: no version banner matched")
        return found.group("version")

    return lookup


@dataclass(frozen=True)
class VendoredSource:
    """A row in THIRD_PARTY.md and the file it claims to describe."""

    path: str
    file_pattern: str
    upstream: Callable[[], str] | None = None
    untracked_reason: str = ""


# The banner each header carries. Read from the file rather than assumed, so a
# replaced file that forgot to update the table is caught by the offline pass.
VENDORED_SOURCES: tuple[VendoredSource, ...] = (
    VendoredSource(
        path="include/didi/common/json.hpp",
        # nlohmann/json prints its version inside an ASCII-art banner, twice.
        # Either occurrence is the same string; the first match is enough.
        file_pattern=r"version (?P<version>\d+\.\d+\.\d+)",
        upstream=github_latest_release("nlohmann/json"),
    ),
    VendoredSource(
        path="include/didi/common/stb_image_write.h",
        file_pattern=r"stb_image_write - v(?P<version>\d+(?:\.\d+)*)",
        upstream=raw_file_banner(
            "nothings/stb",
            "master",
            "stb_image_write.h",
            r"stb_image_write - v(?P<version>\d+(?:\.\d+)*)",
        ),
    ),
    VendoredSource(
        path="include/didi/gdextension/gdextension_interface.h",
        # No banner to read: the file is a C header of function pointers.
        file_pattern="",
        untracked_reason=(
            "a compatibility contract rather than a version to chase -- "
            "addons/didi/didi.gdextension declares compatibility_minimum 4.5 and "
            "the live harness runs against real 4.5.1 and 4.7.2 editors, which is "
            "what actually has to stay true"
        ),
    ),
)


def normalise(version: str) -> str:
    """Reduce a recorded version to something two sources can be compared on.

    `v1.16`, `1.16` and `Godot 4.7 era, ...` all appear in this repository's own
    records. The first two are the same version written differently; the third
    is prose, and is returned unchanged so a comparison against it fails loudly
    rather than silently matching something.
    """
    stripped = version.strip()
    found = re.match(r"^v?(?P<version>\d+(?:\.\d+)*)$", stripped)
    return found.group("version") if found else stripped


def version_tuple(version: str) -> tuple[int, ...] | None:
    """`3.11.3` -> `(3, 11, 3)`; anything else -> None."""
    normalised = normalise(version)
    if not re.match(r"^\d+(?:\.\d+)*$", normalised):
        return None
    return tuple(int(part) for part in normalised.split("."))


def parse_third_party_table(text: str) -> dict[str, str]:
    """Map each vendored path to the version THIRD_PARTY.md records for it.

    The table's first column is a backticked path and its third is the version.
    Parsed rather than indexed by line number, so inserting a paragraph above
    the table does not silently break the check.
    """
    versions: dict[str, str] = {}
    for line in text.splitlines():
        if not line.startswith("|"):
            continue
        cells = [cell.strip() for cell in line.strip().strip("|").split("|")]
        if len(cells) < 3:
            continue
        found = re.match(r"^`(?P<path>[^`]+)`$", cells[0])
        if found:
            versions[found.group("path")] = cells[2]
    return versions


def read_file_version(source: VendoredSource, root: Path) -> str | None:
    """Pull the version out of the vendored file itself."""
    if not source.file_pattern:
        return None
    path = root / source.path
    if not path.is_file():
        return None
    # The banner is at the top of every one of these headers, and json.hpp is
    # about a megabyte. Read the opening rather than the whole file.
    with path.open("r", encoding="utf-8", errors="replace") as handle:
        head = handle.read(8192)
    found = re.search(source.file_pattern, head)
    return found.group("version") if found else None


def check_source(
    source: VendoredSource,
    table: dict[str, str],
    root: Path,
    offline: bool,
) -> SourceReport:
    """Run both comparisons for one vendored file."""
    report = SourceReport(source=Path(source.path).name, path=source.path)
    report.table_version = table.get(source.path)

    if report.table_version is None:
        report.findings.append(
            Finding(
                source=source.path,
                kind="untabled",
                detail=(
                    "vendored file has no row in THIRD_PARTY.md. Every copied "
                    "source belongs in that table -- it is the only record there is."
                ),
            )
        )
        return report

    if not source.file_pattern:
        report.tracked = False
        report.note = source.untracked_reason
        return report

    if not (root / source.path).is_file():
        report.findings.append(
            Finding(
                source=source.path,
                kind="missing",
                detail="THIRD_PARTY.md lists this file but it is not in the tree.",
            )
        )
        return report

    report.file_version = read_file_version(source, root)
    if report.file_version is None:
        report.findings.append(
            Finding(
                source=source.path,
                kind="unreadable",
                detail=(
                    "no version banner matched in the vendored file. Upstream may "
                    "have changed its header format; update file_pattern in "
                    "tools/check_vendored_versions.py."
                ),
            )
        )
        return report

    if normalise(report.file_version) != normalise(report.table_version):
        report.findings.append(
            Finding(
                source=source.path,
                kind="table-drift",
                detail=(
                    f"THIRD_PARTY.md records {report.table_version!r} but the file "
                    f"says {report.file_version!r}. One of them was updated without "
                    "the other."
                ),
            )
        )

    if offline or source.upstream is None:
        return report

    try:
        report.upstream_version = source.upstream()
    except VersionLookupError as error:
        report.findings.append(
            Finding(source=source.path, kind="lookup-failed", detail=str(error))
        )
        return report

    local = version_tuple(report.file_version)
    upstream = version_tuple(report.upstream_version)
    if local is None or upstream is None:
        report.findings.append(
            Finding(
                source=source.path,
                kind="uncomparable",
                detail=(
                    f"cannot compare {report.file_version!r} with upstream "
                    f"{report.upstream_version!r}."
                ),
            )
        )
    elif local < upstream:
        report.findings.append(
            Finding(
                source=source.path,
                kind="behind",
                detail=(
                    f"tree has {report.file_version}, upstream has "
                    f"{report.upstream_version}. See THIRD_PARTY.md for what "
                    "updating one involves -- it is not a routine bump."
                ),
            )
        )

    return report


def run(root: Path = REPO_ROOT, offline: bool = False) -> list[SourceReport]:
    """Check every vendored source and return one report each."""
    table = parse_third_party_table((root / "THIRD_PARTY.md").read_text(encoding="utf-8"))
    return [check_source(source, table, root, offline) for source in VENDORED_SOURCES]


def render(reports: Sequence[SourceReport]) -> str:
    """Human-readable summary, findings last so they are the thing left on screen."""
    lines = ["Vendored sources (THIRD_PARTY.md):", ""]
    for report in reports:
        if not report.tracked:
            lines.append(f"  - {report.path}: not tracked -- {report.note}")
            continue
        current = report.file_version or "unknown"
        upstream = report.upstream_version or "not checked"
        lines.append(f"  - {report.path}: tree {current}, upstream {upstream}")

    findings = [finding for report in reports for finding in report.findings]
    lines.append("")
    if not findings:
        lines.append("No findings. Every tracked source matches its row and upstream.")
    else:
        lines.append(f"{len(findings)} finding(s):")
        for finding in findings:
            lines.append(f"  [{finding.kind}] {finding.source}: {finding.detail}")
    return "\n".join(lines)


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--offline",
        action="store_true",
        help="Skip upstream lookups; only check THIRD_PARTY.md against the files.",
    )
    parser.add_argument(
        "--check",
        action="store_true",
        help="Exit non-zero when there is anything to act on.",
    )
    parser.add_argument("--json", action="store_true", help="Emit machine-readable output.")
    args = parser.parse_args(argv)

    reports = run(offline=args.offline)

    if args.json:
        print(json.dumps([asdict(report) for report in reports], indent=2))
    else:
        print(render(reports))

    if args.check and any(report.findings for report in reports):
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
