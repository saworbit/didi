"""File a session's findings as issues, from files, in one pass.

A vibe session ends with a pile of findings and a decision nobody should have to
make thirteen times in a row: what exactly to type into `gh issue create`. Doing
it by hand drifts -- the fourth issue gets a thinner repro than the first, and
the labels stop matching.

So the findings live as markdown files with a manifest beside them, and this
files the lot. The manifest is also what makes the session reviewable *before*
anything is public: the whole batch can be read in one place, and a finding that
turns out to be a misreading of a schema is deleted rather than retracted from a
public thread.

Manifest shape (`issues.json` next to the bodies)::

    [{"title": "...", "labels": ["bug", "agent-ready"], "body": "01.md"}, ...]

Usage::

    python tools/vibe/report.py FINDINGS_DIR --dry-run   # print what would be filed
    python tools/vibe/report.py FINDINGS_DIR             # file them

Bodies are written for a reader who was not there: what was seen, what was
expected, and the exact calls that show it. The repro is the point -- a finding
without one is a hunch, and the `agent-ready` label in this repository promises
a minimal reproduction that a fix cycle can pick up unattended.
"""

from __future__ import annotations

import argparse
import json
import subprocess
from pathlib import Path

REPOSITORY_ROOT = Path(__file__).resolve().parents[2]


def load(directory: Path) -> list[dict]:
    manifest = json.loads((directory / "issues.json").read_text(encoding="utf-8"))
    for entry in manifest:
        body = directory / entry["body"]
        if not body.is_file():
            raise FileNotFoundError(f"{entry['title']!r} names a body file that is not there: {body}")
    return manifest


def command(entry: dict, directory: Path) -> list[str]:
    argv = [
        "gh",
        "issue",
        "create",
        "--title",
        entry["title"],
        "--body-file",
        str(directory / entry["body"]),
    ]
    for label in entry.get("labels", []):
        argv += ["--label", label]
    return argv


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("directory", help="Holds issues.json and the body files.")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--repo-root", default=str(REPOSITORY_ROOT))
    args = parser.parse_args()

    # Resolved, because `gh` runs with the repository as its working directory
    # and a relative --body-file is then looked for in the wrong place. A
    # findings directory lives in a temp path, not in the repository, so the
    # relative form fails on every entry at once -- thirteen identical "The
    # system cannot find the file specified" lines and nothing filed.
    directory = Path(args.directory).resolve()
    manifest = load(directory)
    print(f"{len(manifest)} findings in {directory}")

    filed: list[str] = []
    for index, entry in enumerate(manifest, start=1):
        argv = command(entry, directory)
        if args.dry_run:
            print(f"{index:2} would file: {entry['title']}")
            print(f"   labels: {', '.join(entry.get('labels', [])) or '(none)'}")
            continue
        result = subprocess.run(argv, capture_output=True, text=True, cwd=args.repo_root)
        output = (result.stdout + result.stderr).strip()
        last = output.splitlines()[-1] if output else ""
        print(f"{index:2} exit {result.returncode} {last}")
        filed.append(last)

    if filed:
        (directory / "filed.json").write_text(json.dumps(filed, indent=1), encoding="utf-8")
        print(f"urls -> {directory / 'filed.json'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
