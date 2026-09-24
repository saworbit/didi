"""What an export preset has to be, asked of the engine rather than of Didi.

#779 found that nothing on the surface writes `export_presets.cfg`, so
`project_export` is unreachable on any project nobody has exported by hand.
Before a tool that writes one is designed, this probe settles the facts it
would depend on, on every engine it is given:

* **What loads.** A preset is written by hand in several shapes -- every key
  the engine reads without a default, then each of those removed in turn, the
  options section present, empty and absent, the 4.7 `[runnable_presets]`
  section instead of the per-preset `runnable` key, and a preset after a gap in
  the numbering -- and each one is exported with `--export-pack`. A row records
  whether a pack came out, whether that pack then runs as a game, whether the
  export rewrote the file, and every ERROR and WARNING line the engine printed.
* **Which platform names exist.** The same preset under every platform name a
  caller might write, including the pre-4.3 `Linux/X11` spelling, a wrong
  letter case and the OS name rather than the platform name. The engine skips
  a preset whose platform it does not know without saying so, which is why the
  answer has to come from an export rather than from a load.
* **What an open editor does with a preset written underneath it.** A headless
  editor runs a probe plugin that registers a no-op export platform, which is
  the only public route to one of the editor's own `EditorExportPreset`
  objects. Setting a value on that preset makes the editor save every preset
  it holds, so the file on disk afterwards is the editor's memory written out.
  The plugin forces that save once to see the engine's own serialisation of a
  hand-written preset on each platform, then after writing a preset behind the
  editor's back and running the filesystem scan `editor_reload_project` runs,
  then after registering a second platform, and last after adding and removing
  a bare `EditorExportPlatformExtension` in one frame through an `EditorPlugin`
  that is never in the tree. The last two make the editor re-read the file;
  the scan does not.
* **Why not a general `.cfg` writer.** An `override.cfg` beside
  `project.godot`, with a renamed project and an autoload in it, and whether
  either takes effect.

Everything here is the evidence for the `project_add_export_preset` amendment
in `docs/SURFACE_AMENDMENTS.md`. Needs no Didi build, no addon and no MCP
server. Every project is a throwaway under `--out` (a temporary directory by
default), and the files the editor wrote are kept there as evidence.

    python tools/vibe/probes/export_preset_engine.py --godot C:/Godot/Godot_v4.5.1-stable_win64_console.exe \\
        --godot C:/Godot/Godot_v4.6.2-stable_win64_console.exe --godot C:/Godot/Godot_v4.7.2-stable_win64_console.exe
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

PRESET = "Probe"
PACK_MARKER = "DIDI_PACK_RAN"

# Every key 4.5.1 reads from a preset section without a default, plus the ones
# the tool would plausibly write. The variants below remove them one at a time.
BASE_KEYS = [
    ("name", f'"{PRESET}"'),
    ("platform", '"Windows Desktop"'),
    ("runnable", "false"),
    ("export_filter", '"all_resources"'),
    ("include_filter", '""'),
    ("exclude_filter", '""'),
    ("export_path", '""'),
]
BASE_OPTIONS = [("custom_template/debug", '""')]

PLATFORM_NAMES = [
    "Windows Desktop", "Linux", "macOS", "Android", "iOS", "Web", "visionOS",
    "Linux/X11", "windows desktop", "Windows", "HTML5",
]


def preset_text(index: int, keys: list[tuple[str, str]],
                options: list[tuple[str, str]] | None, *, empty_options: bool = False,
                overrides: dict[str, str] | None = None, drop: tuple[str, ...] = ()) -> str:
    lines = [f"[preset.{index}]", ""]
    for key, value in keys:
        if key in drop:
            continue
        lines.append(f"{key}={(overrides or {}).get(key, value)}")
    lines.append("")
    if options is not None or empty_options:
        lines += [f"[preset.{index}.options]", ""]
        for key, value in options or []:
            lines.append(f"{key}={value}")
        lines.append("")
    return "\n".join(lines) + "\n"


def load_variants() -> list[tuple[str, str, str]]:
    """(label, what it asks, file text). Every one exports the preset named PRESET."""
    other = preset_text(0, BASE_KEYS, BASE_OPTIONS, overrides={"name": '"Other"'})
    return [
        ("every_read_key", "the keys 4.5 reads without a default, and one option",
         preset_text(0, BASE_KEYS, BASE_OPTIONS)),
        ("no_options_section", "the same with no [preset.0.options] section",
         preset_text(0, BASE_KEYS, None)),
        ("empty_options_section", "an options header with no keys under it",
         preset_text(0, BASE_KEYS, None, empty_options=True)),
        ("no_runnable", "runnable left out",
         preset_text(0, BASE_KEYS, BASE_OPTIONS, drop=("runnable",))),
        ("runnable_true", "runnable=true, the key 4.7 reads only for compatibility",
         preset_text(0, BASE_KEYS, BASE_OPTIONS, overrides={"runnable": "true"})),
        ("runnable_presets_section", "4.7's [runnable_presets] section and no runnable key",
         '[runnable_presets]\n\n"Windows Desktop"="Probe"\n\n'
         + preset_text(0, BASE_KEYS, BASE_OPTIONS, drop=("runnable",))),
        ("no_include_exclude", "include_filter and exclude_filter left out",
         preset_text(0, BASE_KEYS, BASE_OPTIONS, drop=("include_filter", "exclude_filter"))),
        ("no_export_filter", "export_filter left out",
         preset_text(0, BASE_KEYS, BASE_OPTIONS, drop=("export_filter",))),
        ("no_export_path", "export_path left out",
         preset_text(0, BASE_KEYS, BASE_OPTIONS, drop=("export_path",))),
        ("unknown_option", "an option key no platform declares",
         preset_text(0, BASE_KEYS, BASE_OPTIONS + [("didi/not_an_option", "true")])),
        ("after_a_gap", "the preset numbered 2 with no preset 1",
         other + preset_text(2, BASE_KEYS, BASE_OPTIONS)),
        ("second_of_two", "the preset numbered 1, after one that loads",
         other + preset_text(1, BASE_KEYS, BASE_OPTIONS)),
        ("numbered_from_one", "the only preset numbered 1, with no preset 0",
         preset_text(1, BASE_KEYS, BASE_OPTIONS)),
        ("zero_padded_number", "the only preset in a section spelled [preset.00]",
         preset_text(0, BASE_KEYS, BASE_OPTIONS).replace("[preset.0]", "[preset.00]")
         .replace("[preset.0.options]", "[preset.00.options]")),
        ("name_twice", "two presets with this name, Linux first and Windows second",
         preset_text(0, BASE_KEYS + [("custom_features", '"didi_first"')], BASE_OPTIONS,
                     overrides={"platform": '"Linux"'})
         + preset_text(1, BASE_KEYS + [("custom_features", '"didi_second"')], BASE_OPTIONS)),
    ]


def engine_version(godot: str) -> str:
    out = subprocess.run([godot, "--version"], capture_output=True, text=True, timeout=60)
    return out.stdout.strip().splitlines()[-1] if out.stdout.strip() else "?"


def engine_lines(text: str) -> list[str]:
    """ERROR and WARNING lines with the `at:` line that follows each."""
    lines = text.splitlines()
    picked = []
    for i, line in enumerate(lines):
        stripped = line.strip()
        if re.match(r"^(ERROR|WARNING|SCRIPT ERROR|USER ERROR|USER WARNING):", stripped):
            where = ""
            if i + 1 < len(lines) and lines[i + 1].strip().startswith("at:"):
                where = "  [" + lines[i + 1].strip()[3:].strip() + "]"
            picked.append(stripped + where)
    return picked


def write_project(root: Path, version: str, presets: str | None, plugin: bool = False) -> None:
    if root.exists():
        shutil.rmtree(root)
    root.mkdir(parents=True)
    feature = ".".join(version.split(".")[:2])
    project = [
        "config_version=5", "",
        "[application]", "",
        'config/name="Didi export preset probe"',
        'run/main_scene="res://main.tscn"',
        f'config/features=PackedStringArray("{feature}")', "",
    ]
    if plugin:
        project += ["[editor_plugins]", "",
                    'enabled=PackedStringArray("res://addons/didi_export_probe/plugin.cfg")', ""]
    (root / "project.godot").write_text("\n".join(project), encoding="utf-8", newline="\n")
    (root / "main.gd").write_text(
        "extends Node\n\nfunc _ready() -> void:\n"
        f'\tprint("{PACK_MARKER} ", Engine.get_version_info().string,\n'
        '\t\t" first=", OS.has_feature("didi_first"), " second=", OS.has_feature("didi_second"))\n'
        "\tget_tree().quit()\n", encoding="utf-8", newline="\n")
    (root / "main.tscn").write_text(
        '[gd_scene load_steps=2 format=3]\n\n'
        '[ext_resource type="Script" path="res://main.gd" id="1"]\n\n'
        '[node name="Main" type="Node"]\nscript = ExtResource("1")\n',
        encoding="utf-8", newline="\n")
    if presets is not None:
        (root / "export_presets.cfg").write_text(presets, encoding="utf-8", newline="\n")
    if plugin:
        addon = root / "addons" / "didi_export_probe"
        addon.mkdir(parents=True)
        (addon / "plugin.cfg").write_text(
            '[plugin]\n\nname="Didi export preset probe"\ndescription=""\nauthor=""\n'
            'version="1"\nscript="plugin.gd"\n', encoding="utf-8", newline="\n")
        (addon / "plugin.gd").write_text(PLUGIN_SOURCE, encoding="utf-8", newline="\n")


def run(cmd: list[str], cwd: Path | None = None, env: dict | None = None,
        timeout: int = 240) -> tuple[int | None, str]:
    try:
        out = subprocess.run(cmd, cwd=cwd, env=env, capture_output=True, text=True,
                             encoding="utf-8", errors="replace", timeout=timeout)
        return out.returncode, out.stdout + out.stderr
    except subprocess.TimeoutExpired as exc:
        text = (exc.stdout or b"") + (exc.stderr or b"")
        if isinstance(text, bytes):
            text = text.decode("utf-8", "replace")
        return None, text


def export_and_run(godot: str, project: Path, preset: str, work: Path) -> dict:
    pck = work / "out.pck"
    if pck.exists():
        pck.unlink()
    presets_file = project / "export_presets.cfg"
    before = presets_file.read_bytes() if presets_file.exists() else None
    code, text = run([godot, "--headless", "--path", str(project), "--export-pack", preset, str(pck)])
    after = presets_file.read_bytes() if presets_file.exists() else None
    result = {"export_rc": code, "pck_bytes": pck.stat().st_size if pck.exists() else 0,
              "file_rewritten": before != after, "engine": engine_lines(text),
              "ran": False, "tail": text.strip().splitlines()[-3:]}
    if pck.exists():
        empty = work / "empty"
        empty.mkdir(exist_ok=True)
        _, ran = run([godot, "--headless", "--main-pack", str(pck)], cwd=empty, timeout=60)
        result["ran"] = PACK_MARKER in ran
        result["ran_line"] = next((l.strip() for l in ran.splitlines() if PACK_MARKER in l), "")
        result["run_engine"] = engine_lines(ran)
    return result


def show(label: str, asks: str, result: dict) -> None:
    verdict = ("pack, runs" if result["ran"] else
               "pack, does not run" if result["pck_bytes"] else "no pack")
    print(f"  {label:26} {verdict:18} rc={result['export_rc']!s:4} "
          f"rewrote_file={'yes' if result['file_rewritten'] else 'no '}  {asks}")
    if "=true" in result.get("ran_line", ""):
        print(f"      game:   {result['ran_line']}")
    for line in result["engine"]:
        print(f"      engine: {line}")
    for line in result.get("run_engine", []):
        print(f"      game:   {line}")
    if not result["pck_bytes"]:
        for line in result["tail"]:
            print(f"      tail:   {line.strip()[:200]}")


PLUGIN_SOURCE = r'''@tool
extends EditorPlugin

# A no-op export platform. Registering one is the only public route to the
# editor's own EditorExportPreset objects, through get_current_presets(), and
# registering or removing one makes the editor re-read export_presets.cfg.
class ProbePlatform extends EditorExportPlatformExtension:
	var platform_name := "DidiProbe"

	func _get_name() -> String:
		return platform_name

	func _get_os_name() -> String:
		return platform_name

	func _get_logo() -> Texture2D:
		return null

	func _get_export_options() -> Array[Dictionary]:
		return []

	func _get_preset_features(_preset: EditorExportPreset) -> PackedStringArray:
		return PackedStringArray()

	func _get_platform_features() -> PackedStringArray:
		return PackedStringArray()

	func _get_binary_extensions(_preset: EditorExportPreset) -> PackedStringArray:
		return PackedStringArray(["pck"])

	func _has_valid_export_configuration(_preset: EditorExportPreset, _debug: bool) -> bool:
		return true

	func _has_valid_project_configuration(_preset: EditorExportPreset) -> bool:
		return true


const CFG := "res://export_presets.cfg"
var out_dir := ""


func _enter_tree() -> void:
	out_dir = OS.get_environment("DIDI_PROBE_OUT")
	_run.call_deferred()


func _wait(seconds: float) -> void:
	await get_tree().create_timer(seconds).timeout


func _snap(label: String) -> void:
	var bytes := FileAccess.get_file_as_bytes(CFG)
	var file := FileAccess.open(out_dir.path_join(label + ".cfg"), FileAccess.WRITE)
	file.store_buffer(bytes)
	file.close()
	print("DIDI_PROBE_SNAP ", label, " ", bytes.size())


# What an outside writer does: append a section at the next free index.
func _append(preset_name: String) -> void:
	var text := FileAccess.get_file_as_string(CFG)
	var index := 0
	while text.contains("[preset.%d]" % index):
		index += 1
	text += "\n[preset.%d]\n\nname=\"%s\"\nplatform=\"Windows Desktop\"\nrunnable=false\n" % [index, preset_name]
	text += "export_filter=\"all_resources\"\ninclude_filter=\"\"\nexclude_filter=\"\"\nexport_path=\"\"\n"
	text += "\n[preset.%d.options]\n\ncustom_template/debug=\"\"\n" % index
	var file := FileAccess.open(CFG, FileAccess.WRITE)
	file.store_string(text)
	file.close()
	print("DIDI_PROBE_APPENDED ", preset_name, " at ", index)


# Make the editor write what it holds: any value set on one of its presets
# starts the 0.8 s save timer, and the save writes every preset in memory.
func _force_save(platform: EditorExportPlatform, step: int) -> bool:
	var mine := platform.get_current_presets()
	if mine.is_empty():
		print("DIDI_PROBE_FAIL the marker preset is not loaded at step ", step)
		return false
	mine[0].set("probe_step", step)
	await _wait(2.0)
	return true


func _run() -> void:
	await _wait(3.0)
	_snap("0_after_editor_opened")
	var probe := ProbePlatform.new()
	add_export_platform(probe)
	await _wait(1.5)
	if await _force_save(probe, 1):
		_snap("1_editor_save")
		_append("WrittenUnderneath")
		EditorInterface.get_resource_filesystem().scan_sources()
		await _wait(2.0)
		if await _force_save(probe, 2):
			_snap("2_editor_save_after_outside_write_and_scan_sources")
		_append("WrittenBeforeReload")
		var second := ProbePlatform.new()
		second.platform_name = "DidiProbe2"
		add_export_platform(second)
		await _wait(1.5)
		if await _force_save(probe, 3):
			_snap("3_editor_save_after_platform_registered")
		remove_export_platform(second)
		await _wait(1.5)
		# The cheapest form of the same thing, and one the extension can make
		# without any addon script: a bare extension platform, with no subclass
		# and no name, added and removed in one frame through an EditorPlugin
		# that is not this one and is never in the tree.
		_append("WrittenBeforeBareReload")
		var bare := EditorExportPlatformExtension.new()
		var caller := EditorPlugin.new()
		caller.add_export_platform(bare)
		caller.remove_export_platform(bare)
		caller.free()
		await _wait(1.5)
		if await _force_save(probe, 4):
			_snap("4_editor_save_after_bare_platform_added_and_removed")
		# The same save forced with nothing registered and no preset of the
		# probe's own in the file: a preset made by a bare platform that was
		# never added. Setting a value on any EditorExportPreset starts the
		# editor's save timer, and the save writes the editor's list, which
		# this preset is not in. The marker line is a comment, which the
		# editor's own writer does not keep, so its absence proves the save.
		var text := FileAccess.get_file_as_string(CFG) + "\n; didi-unsaved-marker\n"
		var marked := FileAccess.open(CFG, FileAccess.WRITE)
		marked.store_string(text)
		marked.close()
		var loose := EditorExportPlatformExtension.new().create_preset()
		loose.set("didi/loose_preset_value", true)
		await _wait(2.0)
		_snap("5_editor_save_forced_by_a_preset_from_an_unregistered_platform")
	remove_export_platform(probe)
	await _wait(1.0)
	print("DIDI_PROBE_DONE")
	get_tree().quit()
'''


def section_names(text: str) -> list[str]:
    names, current = [], None
    for line in text.splitlines():
        header = re.match(r"^\[(preset\.\d+)\]$", line.strip())
        if header:
            current = header.group(1)
            continue
        if current and line.startswith("name="):
            names.append(f"{current}:{line[5:]}")
            current = None
    return names


def editor_round_trip(godot: str, version: str, work: Path) -> None:
    project = work / "editor_project"
    # Runnable, so the editor's save shows where each line keeps that flag. One
    # more preset per platform, so the save lists the options each declares.
    initial = (preset_text(0, BASE_KEYS, BASE_OPTIONS,
                           overrides={"name": '"HandWritten"', "runnable": "true"})
               + preset_text(1, BASE_KEYS, [("didi/marker", "true")],
                             overrides={"name": '"Marker"', "platform": '"DidiProbe"'}))
    for index, platform in enumerate(PLATFORM_NAMES[1:7], start=2):
        initial += preset_text(index, BASE_KEYS, BASE_OPTIONS,
                               overrides={"name": f'"Each {platform}"', "platform": f'"{platform}"'})
    write_project(project, version, initial, plugin=True)
    snaps = work / "editor_snapshots"
    if snaps.exists():
        shutil.rmtree(snaps)
    snaps.mkdir()
    (snaps / "initial.cfg").write_text(initial, encoding="utf-8", newline="\n")
    env = dict(os.environ, DIDI_PROBE_OUT=str(snaps))
    # One import pass first, so the editor run below is not also the first open.
    run([godot, "--headless", "--path", str(project), "--import"], timeout=240)
    code, text = run([godot, "--headless", "--editor", "--path", str(project)], env=env, timeout=180)
    done = "DIDI_PROBE_DONE" in text
    print(f"  editor run rc={code} finished={'yes' if done else 'NO'}")
    for line in text.splitlines():
        if line.startswith(("DIDI_PROBE_FAIL", "DIDI_PROBE_APPENDED")):
            print(f"      {line}")
    for line in engine_lines(text):
        print(f"      engine: {line}")
    for snap in sorted(snaps.glob("*.cfg")):
        body = snap.read_text(encoding="utf-8", errors="replace")
        same = "   (byte-identical to initial)" if body == initial and snap.stem != "initial" else ""
        print(f"  {snap.stem:52} presets {section_names(body)}{same}")
    first_save = snaps / "1_editor_save.cfg"
    if first_save.exists():
        print("  --- the editor's own serialisation of the hand-written preset ---")
        body = first_save.read_text(encoding="utf-8", errors="replace")
        section, options = "", []
        for line in body.splitlines():
            if re.match(r"^\[", line):
                section = line.strip()
                if section in ("[preset.0]", "[runnable_presets]"):
                    print(f"      {section}")
                continue
            if section in ("[preset.0]", "[runnable_presets]") and line.strip():
                print(f"        {line}")
            elif section == "[preset.0.options]" and re.match(r"^[A-Za-z_][^=]*=", line):
                options.append(line.split("=", 1)[0])
        print(f"      [preset.0.options]  {len(options)} keys: {', '.join(options[:6])}, ...")
        print("  --- the options each platform's preset carries after the editor's save ---")
        platform_of, declared = {}, {}
        for line in body.splitlines():
            if re.match(r"^\[", line):
                section = line.strip()[1:-1]
                continue
            key = line.split("=", 1)[0] if re.match(r"^[A-Za-z_\"][^=]*=", line) else None
            if key is None:
                continue
            if re.fullmatch(r"preset\.\d+", section) and key == "platform":
                platform_of[section] = line.split("=", 1)[1].strip('"')
            elif section.endswith(".options"):
                declared.setdefault(section[:-len(".options")], []).append(key)
        for section, platform in platform_of.items():
            keys = declared.get(section, [])
            print(f"      {platform:16} {len(keys):3} option keys, custom_template/debug "
                  f"{'declared' if 'custom_template/debug' in keys else 'ABSENT'}")
    print(f"  snapshots kept in {snaps}")


def override_cfg(godot: str, version: str, work: Path) -> None:
    """Whether a .cfg beside project.godot changes the project, which is the case
    against a general-purpose writer that would accept the .cfg extension."""
    project = work / "override_project"
    write_project(project, version, None)
    (project / "main.gd").write_text(
        "extends Node\n\nfunc _ready() -> void:\n"
        '\tprint("DIDI_OVERRIDE name=", ProjectSettings.get_setting("application/config/name"),\n'
        '\t\t" autoload=", ProjectSettings.has_setting("autoload/Planted"))\n'
        "\tget_tree().quit()\n", encoding="utf-8", newline="\n")
    (project / "planted.gd").write_text(
        'extends Node\n\nfunc _ready() -> void:\n\tprint("DIDI_PLANTED_AUTOLOAD_RAN")\n',
        encoding="utf-8", newline="\n")
    run([godot, "--headless", "--path", str(project), "--import"])
    for label, text in (("without override.cfg", None),
                        ("with override.cfg", '[application]\n\nconfig/name="Overridden"\n\n'
                                              '[autoload]\n\nPlanted="*res://planted.gd"\n')):
        target = project / "override.cfg"
        if text is None:
            target.unlink(missing_ok=True)
        else:
            target.write_text(text, encoding="utf-8", newline="\n")
        _, out = run([godot, "--headless", "--path", str(project)], timeout=60)
        said = next((l.strip() for l in out.splitlines() if "DIDI_OVERRIDE" in l), "(no line)")
        planted = "an autoload it declares ran" if "DIDI_PLANTED_AUTOLOAD_RAN" in out else "no autoload ran"
        print(f"  {label:22} {said}   {planted}")
        for line in engine_lines(out):
            print(f"      engine: {line}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--godot", action="append", default=[],
                        help="a Godot console binary; repeat for each engine line")
    parser.add_argument("--out", help="where the throwaway projects go (default: a temp dir)")
    parser.add_argument("--only", choices=("load", "platforms", "editor", "override"),
                        action="append", help="run one part; repeat for several")
    args = parser.parse_args()
    engines = args.godot or ([os.environ["GODOT_BIN"]] if os.environ.get("GODOT_BIN") else [])
    if not engines:
        print("pass --godot (repeatable) or set GODOT_BIN: this probe has no witness without one")
        return 2
    parts = args.only or ["load", "platforms", "editor", "override"]
    base = Path(args.out) if args.out else Path(tempfile.mkdtemp(prefix="didi_export_probe_"))
    print(f"working under {base}")
    for godot in engines:
        version = engine_version(godot)
        work = base / re.sub(r"[^0-9A-Za-z.]+", "_", version)
        work.mkdir(parents=True, exist_ok=True)
        print(f"\n=== {version}  ({godot})")
        project = work / "project"
        if "load" in parts or "platforms" in parts:
            write_project(project, version, None)
            code, text = run([godot, "--headless", "--path", str(project), "--import"])
            print(f"  import pass rc={code}")
            for line in engine_lines(text):
                print(f"      engine: {line}")
        if "load" in parts:
            print("\n  -- what loads: each file exported with --export-pack", PRESET)
            for label, asks, text in load_variants():
                (project / "export_presets.cfg").write_text(text, encoding="utf-8", newline="\n")
                show(label, asks, export_and_run(godot, project, PRESET, work))
        if "platforms" in parts:
            print("\n  -- which platform names exist: the every_read_key preset under each name")
            for name in PLATFORM_NAMES:
                text = preset_text(0, BASE_KEYS, BASE_OPTIONS, overrides={"platform": f'"{name}"'})
                (project / "export_presets.cfg").write_text(text, encoding="utf-8", newline="\n")
                show(name, "", export_and_run(godot, project, PRESET, work))
        if "editor" in parts:
            print("\n  -- what an open editor does with a preset written underneath it")
            editor_round_trip(godot, version, work)
        if "override" in parts:
            print("\n  -- what a .cfg beside project.godot does: override.cfg")
            override_cfg(godot, version, work)
    return 0


if __name__ == "__main__":
    sys.exit(main())
