"""Seed a Didi field trial working directory.

Deliberately minimal. Didi exits 2 when --project names a directory without a
project.godot, so the project file is forced by the architecture. Everything
past it, copying the addon, enabling the plugin, starting the editor, is the
tester's job, because that is exactly where real users get stuck.
"""

from __future__ import annotations

import argparse
import datetime
import json
from pathlib import Path
import shutil
import subprocess

PROJECT_GODOT = """; Seeded by tools/field-trial/seed_trial.py. Bare on purpose.

config_version=5

[application]

config/name="Didi Field Trial"
config/features=PackedStringArray("4.7", "Forward Plus")
"""


def _commit(repository: Path) -> str:
    result = subprocess.run(
        ["git", "-C", str(repository), "rev-parse", "HEAD"],
        capture_output=True,
        text=True,
        check=True,
    )
    return result.stdout.strip()


def seed(
    target: Path,
    didi_exe: Path,
    godot_exe: Path,
    repository: Path,
    manifest: Path | None = None,
) -> dict:
    """Create the trial directory and return the baseline record written into it."""
    for label, path in (("Didi executable", didi_exe), ("Godot executable", godot_exe)):
        if not path.is_file():
            raise FileNotFoundError(f"{label} not found: {path}")
    if target.exists():
        raise FileExistsError(
            f"{target} already exists. A trial is scored against its seed, so reusing "
            "a directory silently mixes two runs. Delete it or pick another path."
        )

    target.mkdir(parents=True)
    (target / "project.godot").write_text(PROJECT_GODOT, encoding="utf-8")
    (target / ".mcp.json").write_text(
        json.dumps(
            {
                "mcpServers": {
                    "didi": {
                        "command": str(didi_exe),
                        "args": ["--project", str(target), "--log-level", "DEBUG"],
                    }
                }
            },
            indent=2,
        )
        + "\n",
        encoding="utf-8",
    )

    if manifest is not None and manifest.is_file():
        shutil.copyfile(manifest, target / "tool-manifest.baseline.json")

    baseline = {
        "commit": _commit(repository),
        "didi_executable": str(didi_exe),
        "godot_executable": str(godot_exe),
        "repository": str(repository),
        "seeded_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "manifest_copied": manifest is not None and manifest.is_file(),
    }
    (target / "baseline.json").write_text(
        json.dumps(baseline, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    return baseline


def main(argv: list[str] | None = None) -> int:
    repository = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--target", required=True, type=Path)
    parser.add_argument(
        "--didi-exe", type=Path, default=repository / "build-ninja" / "didi.exe"
    )
    parser.add_argument(
        "--godot-exe",
        type=Path,
        default=Path(r"C:\Godot\Godot_v4.7.2-stable_win64_console.exe"),
    )
    parser.add_argument(
        "--manifest", type=Path, default=repository / "build-ninja" / "tool-manifest.json"
    )
    args = parser.parse_args(argv)

    baseline = seed(
        target=args.target,
        didi_exe=args.didi_exe,
        godot_exe=args.godot_exe,
        repository=repository,
        manifest=args.manifest,
    )
    print(json.dumps(baseline, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
