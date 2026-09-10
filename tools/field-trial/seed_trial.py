"""Seed a Didi field trial working directory.

Deliberately minimal. Didi exits 2 when --project names a directory without a
project.godot, so the project file is forced by the architecture. Everything
past it, copying the addon, enabling the plugin, starting the editor, is the
tester's job, because that is exactly where real users get stuck.
"""

from __future__ import annotations

import argparse
import datetime
import hashlib
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


def parse_build_id(version_output: str) -> str | None:
    """The build id out of `didi --version`, or None from a build too old to print one.

    Parsed rather than assumed, because a seed that guesses this is worse than a
    seed that admits it does not know: the whole point of recording it is to be
    able to say later which build answered.
    """
    for line in version_output.splitlines():
        stripped = line.strip()
        if stripped.startswith("build "):
            return stripped[len("build ") :].strip() or None
    return None


# Both probes below run the server under test to ask it about itself, and both
# are documented as never fatal. A probe that never returns is worse than a
# fatal one: it takes the whole seed with it and says nothing. Generous, because
# a cold start on a loaded machine is not a fault.
_PROBE_TIMEOUT_SECONDS = 60


def _server_build_id(didi_exe: Path) -> str | None:
    """What the server says it was built from, or None when it will not say.

    Never fatal. A build too old to print a build id, or a path that turns out
    not to be executable at all, still leaves a seed worth having; what it must
    not do is leave a baseline that quietly claims an identity it never read.
    """
    try:
        result = subprocess.run(
            [str(didi_exe), "--version"], capture_output=True, text=True, check=False,
            timeout=_PROBE_TIMEOUT_SECONDS,
        )
    except (OSError, subprocess.TimeoutExpired):
        return None
    if result.returncode != 0:
        return None
    return parse_build_id(result.stdout)


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _extension_binary(addon_root: Path) -> Path | None:
    """The compiled half of an assembled addon, whatever this platform calls it."""
    binaries = sorted((addon_root / "bin").glob("didi_extension.*"))
    return binaries[0] if binaries else None


def addon_record(didi_exe: Path, repository: Path) -> dict:
    """Which GDExtension the tester is supposed to install, and which one is lying around.

    The seed deliberately does not install the addon; working out that the live
    tools need one is part of the experiment. Recording it is a different thing,
    and the run that made it necessary is field trial 03, which spent an hour
    concluding a shipped capability did not exist because it had installed the
    repository's `addons/didi` instead of the one the build assembles. That
    directory is gitignored and written by no build step, so it holds whatever
    was last dropped there by hand, and the two are indistinguishable by eye.

    Both are hashed here so review can say in one line which of them answered.
    """
    build_addon = didi_exe.parent / "addons" / "didi"
    built = _extension_binary(build_addon)
    if built is None:
        raise FileNotFoundError(
            f"No assembled addon beside {didi_exe}: expected {build_addon / 'bin'}. "
            "The tester has to install one for any live tool to work, so a trial seeded "
            "without one measures the offline surface and calls it the whole product."
        )
    stale = _extension_binary(repository / "addons" / "didi")
    record = {
        "install_from": str(build_addon),
        "extension_binary": str(built),
        "extension_sha256": _sha256(built),
        "repository_copy": str(stale) if stale else None,
        "repository_copy_sha256": _sha256(stale) if stale else None,
    }
    record["repository_copy_is_stale"] = bool(
        stale and record["repository_copy_sha256"] != record["extension_sha256"]
    )
    return record


def write_manifest(didi_exe: Path, destination: Path, fallback: Path | None = None) -> str:
    """Put the surface this trial is scored against beside the trial, and say where it came from.

    Dumped from the binary under test rather than copied from wherever a
    manifest happens to be lying, because the two are not the same file and the
    difference is silent. Gating trial 01 hit this for real: the on-disk
    manifest claimed 83 canonical and 80 implemented while the binary being
    tested emitted 94 and 91, so the uncalled set, which is the interesting half
    of a coverage report, was wrong about eleven tools.

    A supplied path is the fallback rather than the preference, for a caller
    deliberately scoring against a pinned surface.
    """
    try:
        result = subprocess.run(
            [str(didi_exe), "--dump-tool-manifest"], capture_output=True, text=True, check=False,
            timeout=_PROBE_TIMEOUT_SECONDS,
        )
    except (OSError, subprocess.TimeoutExpired):
        result = None
    if result is not None and result.returncode == 0:
        try:
            parsed = json.loads(result.stdout)
            parsed["names"]["implemented"]
        except (json.JSONDecodeError, KeyError, TypeError):
            parsed = None
        if parsed is not None:
            destination.write_text(result.stdout, encoding="utf-8")
            return "dumped"
    if fallback is not None and fallback.is_file():
        shutil.copyfile(fallback, destination)
        return "copied"
    return "none"


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

    # Everything that can refuse the seed runs before anything is created. A
    # half-seeded directory is worse than none: the next attempt hits the
    # already-exists guard above and reports a stale run that never happened.
    addon = addon_record(didi_exe, repository)
    build_id = _server_build_id(didi_exe)

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

    manifest_source = write_manifest(didi_exe, target / "tool-manifest.baseline.json", manifest)

    baseline = {
        "commit": _commit(repository),
        "didi_executable": str(didi_exe),
        # The identity of the build, not the version it claims. Two builds of
        # 1.7.0 are the same version and different software, and the live half
        # of every call is served by a file the seed never touches.
        "server_build_id": build_id,
        "addon": addon,
        "godot_executable": str(godot_exe),
        "repository": str(repository),
        "seeded_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "manifest_source": manifest_source,
        "manifest_copied": manifest_source != "none",
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
