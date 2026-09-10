"""Build a throwaway Godot project to point a vibe-testing session at.

Exploratory testing needs a project that is *yours*: something you can let a
mutation tool rewrite, group the wrong node in, and write settings into, without
wondering afterwards whether the damage matters. `tests/godot_smoke` is not that
project -- it is the fixture the live harness asserts against, and editing it
breaks a suite.

The fixture here is deliberately tiny: a root, one child with a known property,
and one script with a class, a signal, an exported variable and two methods.
That is enough to exercise the scene, script, symbol and impact tools, and
small enough that a wrong answer is obvious on sight rather than something you
have to diff.

Two facts about the addon are worth having in one place, because both have cost
real time before:

* Install from **`build-ninja/addons/didi`** (or whichever build tree is
  current), never from the repository's own `addons/didi`. The build assembles
  a complete addon; the source tree's `bin/didi_extension.dll` is gitignored and
  written by no build step, so it holds whatever was last dropped there by hand.
* The plugin has to be listed in `[editor_plugins]` in `project.godot` or the
  editor loads the project without ever publishing a session, and every live
  tool then reports `offline_fallback` with no explanation that looks like a
  bug.
"""

from __future__ import annotations

import argparse
import shutil
import subprocess
from pathlib import Path

REPOSITORY_ROOT = Path(__file__).resolve().parents[2]

PROJECT_GODOT = """config_version=5

[application]

config/name="{name}"
run/main_scene="res://main.tscn"
config/features=PackedStringArray("4.5", "Forward Plus")
"""

EDITOR_PLUGINS = """
[editor_plugins]

enabled=PackedStringArray("res://addons/didi/plugin.cfg")
"""

MAIN_TSCN = """[gd_scene load_steps=1 format=3 uid="uid://bvibesandbox01"]

[node name="Main" type="Node2D"]

[node name="Child" type="Sprite2D" parent="."]
position = Vector2(10, 20)
"""

PLAYER_GD = '''extends Node2D
class_name Player

signal died(reason: String)

@export var speed: float = 100.0
var _hp := 10

func _ready() -> void:
\tprint("ready")

func take_damage(amount: int) -> void:
\t_hp -= amount
\tif _hp <= 0:
\t\tdied.emit("dead")
'''


def addon_source(build_tree: Path | None = None) -> Path:
    """The assembled addon to install, newest build tree first.

    Mirrors how `tests/didi_binary.py` picks a binary, and for the same reason:
    both build trees exist on a developer machine and one of them is stale, so
    modification time is a better answer than a fixed order.
    """
    if build_tree is not None:
        return Path(build_tree) / "addons" / "didi"
    candidates = [
        path
        for path in (
            REPOSITORY_ROOT / "build-ninja" / "addons" / "didi",
            REPOSITORY_ROOT / "build" / "addons" / "didi",
        )
        if (path / "plugin.cfg").is_file()
    ]
    if not candidates:
        raise FileNotFoundError(
            "No built addon found. Build the project first; the repository's own "
            "addons/didi is not a substitute -- its bin/ is gitignored and no "
            "build step writes it."
        )
    return max(candidates, key=lambda path: path.stat().st_mtime)


def create(
    destination: Path,
    name: str = "VibeSandbox",
    with_addon: bool = True,
    build_tree: Path | None = None,
    overwrite: bool = False,
) -> Path:
    """Write the fixture project and return its root."""
    destination = Path(destination)
    if destination.exists():
        if not overwrite:
            raise FileExistsError(
                f"{destination} already exists. Pass overwrite to replace it, or "
                f"pick a fresh directory -- a sandbox reused across sessions "
                f"carries the previous session's mutations."
            )
        shutil.rmtree(destination)
    destination.mkdir(parents=True)

    project_file = PROJECT_GODOT.format(name=name)
    if with_addon:
        shutil.copytree(addon_source(build_tree), destination / "addons" / "didi")
        project_file += EDITOR_PLUGINS
    (destination / "project.godot").write_text(project_file, encoding="utf-8")
    (destination / "main.tscn").write_text(MAIN_TSCN, encoding="utf-8")
    (destination / "player.gd").write_text(PLAYER_GD, encoding="utf-8")
    return destination


def launch_editor(project: Path, godot: str, log_file: Path | None = None) -> subprocess.Popen:
    """Open the editor on the sandbox so the live half of the surface answers.

    Returned so the caller can stop it. Leaving an editor running past a session
    is not harmless: it keeps publishing a session descriptor, and the next
    server started anywhere on the machine can attach to it (see issue #387).
    """
    argv = [str(godot), "--editor", "--path", str(project)]
    if log_file is not None:
        argv += ["--log-file", str(log_file)]
    return subprocess.Popen(argv)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("destination", help="Directory to create. Use a temp path.")
    parser.add_argument("--name", default="VibeSandbox")
    parser.add_argument("--no-addon", action="store_true", help="Offline-only sandbox.")
    parser.add_argument("--build-tree", help="Take the addon from this build directory.")
    parser.add_argument("--overwrite", action="store_true")
    parser.add_argument(
        "--launch",
        metavar="GODOT_EXE",
        help="Open the editor on the new project once it is written.",
    )
    args = parser.parse_args()

    project = create(
        Path(args.destination),
        name=args.name,
        with_addon=not args.no_addon,
        build_tree=Path(args.build_tree) if args.build_tree else None,
        overwrite=args.overwrite,
    )
    print(project)
    if args.launch:
        process = launch_editor(project, args.launch, log_file=project.parent / "editor.log")
        print(f"editor pid {process.pid} -- stop it when the session ends")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
