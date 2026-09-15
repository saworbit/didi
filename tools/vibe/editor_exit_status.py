"""Does the editor exit cleanly, and is it the addon that decides?

A probe calls a tool and reads the answer. Nothing in this harness has ever
looked at what the editor process does on the way *out* -- and on both POSIX
runners the thirteenth session found a headless editor aborting immediately
after `[Didi] Didi Native MCP Editor Plugin deactivated.`

An abort on shutdown is the kind of finding that needs a control more than most,
because the subject is a whole engine and the obvious alternative explanation --
"Godot does this" -- is entirely plausible. So this runs the same editor
invocation twice on the same fixture, once with the built addon installed and
once without, and prints both exit statuses side by side. The row that must be
clean is the one with no addon in it; if that aborts too, the finding is not
didi's.

Two invocations, because they exercise different teardowns:

* `--import`, which opens the project, imports, and exits on its own.
* `--editor --quit`, which brings the editor all the way up and then shuts it
  down the way closing it would.

    python tools/vibe/editor_exit_status.py --godot GODOT_EXE [--build-tree build]

Prints a table. A non-zero status with the addon and zero without it is the
attribution; anything else is a lead that needs narrowing before it is a
finding.
"""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import sandbox  # noqa: E402

INVOCATIONS: list[tuple[str, list[str]]] = [
    ("--import", ["--headless", "--import"]),
    ("--editor --quit", ["--editor", "--headless", "--quit"]),
]


def run(godot: str, project: Path, extra: list[str], timeout: float) -> tuple[int | None, str]:
    argv = [godot, *extra, "--path", str(project)]
    try:
        done = subprocess.run(argv, capture_output=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return None, "timed out"
    output = (done.stdout + done.stderr).decode(errors="replace")
    # The last few lines are where a teardown crash prints; the rest is the
    # import progress bar and is the same either way.
    tail = [line for line in output.splitlines() if line.strip()][-3:]
    return done.returncode, " | ".join(tail)[:300]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--godot", required=True, help="Editor binary to run.")
    parser.add_argument("--build-tree", help="Take the addon from this build directory.")
    parser.add_argument("--timeout", type=float, default=180.0)
    parser.add_argument("--keep", action="store_true", help="Leave the fixtures behind.")
    args = parser.parse_args()

    root = Path(tempfile.mkdtemp(prefix="didi-exit-"))
    try:
        rows: list[tuple[str, str, int | None, str]] = []
        for with_addon in (True, False):
            project = root / ("with-addon" if with_addon else "no-addon")
            sandbox.create(
                project,
                with_addon=with_addon,
                build_tree=Path(args.build_tree) if args.build_tree else None,
                overwrite=True,
            )
            for fixture in sorted((Path(__file__).resolve().parent / "fixtures").iterdir()):
                if fixture.is_file():
                    shutil.copy(fixture, project / fixture.name)
            for label, extra in INVOCATIONS:
                status, tail = run(args.godot, project, extra, args.timeout)
                rows.append(("addon" if with_addon else "no addon", label, status, tail))

        print(f"\n{'project':10} {'invocation':18} {'exit':>6}  last lines")
        for addon, label, status, tail in rows:
            print(f"{addon:10} {label:18} {str(status):>6}  {tail}")

        dirty = [r for r in rows if r[0] == "addon" and r[2] not in (0,)]
        clean = [r for r in rows if r[0] == "no addon" and r[2] in (0,)]
        print(
            f"\nwith the addon: {len(dirty)} of {len(INVOCATIONS)} invocations did not exit 0."
            f"\nwithout it:     {len(clean)} of {len(INVOCATIONS)} exited 0."
        )
    finally:
        if not args.keep:
            shutil.rmtree(root, ignore_errors=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
