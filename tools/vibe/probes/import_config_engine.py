"""What changing an asset's import options does, asked of the engine rather than of Didi.

Phase 8 closes on "Import changes are previewable, explicit, and verified after
reimport" (docs/ROADMAP.md), and nothing on the surface writes a `.import`
sidecar: `project_audit_assets` reads them, `asset_reimport` reimports from
them, and the only way to change one is the editor's Import dock or a text
editor. The standing example is a game's music. Whether a track loops is an
import option (`loop` for OGG and MP3, `edit/loop_mode` for WAV), not something
a scene holds, so an agent today writes `stream.loop = true` into a `_ready()`.

Before a tool that changes one is designed, this probe settles the facts it
would depend on, on every engine it is given:

* **The binds and signals.** Every `EditorFileSystem` method such a tool would
  call, with its hash on each engine, and the signals it would listen for,
  read from `--dump-extension-api`.
* **What each importer writes.** A PNG, an SVG, a WAV, an OGG and an MP3 made
  with ffmpeg when it is on PATH, a translation CSV, a TTF when one is found,
  and a GLB the engine exports itself, imported with `--import`. Each
  sidecar's importer, type, uid and every `[params]` key, compared across
  engines, and each resource loaded back.
* **What an edited sidecar becomes.** A pristine imported project is copied
  per case, one sidecar is edited the way a writer would edit it, and
  `--import` runs again. Recorded: whether the engine reimported, what the
  line is once the engine has rewritten the file, whether the uid survived,
  the engine's own ERROR and WARNING lines, and what the resource loads as.
  The cases are the valid edits a tool exists for, then every way to be
  wrong: an unknown key, the wrong type, a value out of range, a removed key,
  a line the parser cannot start, a comment, and a different importer.
* **What an open editor does.** A headless editor runs a probe plugin that
  edits a sidecar behind the editor's back and then waits, calls
  `scan_sources`, `scan`, `update_file` and `reimport_files`, and records
  after each whether the asset was reimported, whether a resource the editor
  already holds shows the new value, and what a fresh load returns. The same
  plugin counts the frames the editor runs *inside* an import pass, reads
  `is_importing()` in each where the engine binds it, and calls
  `reimport_files` from inside the pass at two points, which is #914 asked of
  the engine alone.

Everything here is evidence for an import-configuration amendment in
`docs/SURFACE_AMENDMENTS.md` and for #914. Needs no Didi build, no addon and no
MCP server. Every project is a throwaway under `--out` (a temporary directory
by default), and the files the engine and the editor wrote are kept there.

    python tools/vibe/probes/import_config_engine.py --godot C:/Godot/Godot_v4.5.1-stable_win64_console.exe \\
        --godot C:/Godot/Godot_v4.6.2-stable_win64_console.exe --godot C:/Godot/Godot_v4.7.2-stable_win64_console.exe
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
import math
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import wave
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from probes.audio_bus_engine import Record, engine_version, run  # noqa: E402

BINDS = {
    "EditorFileSystem": ["scan", "scan_sources", "update_file", "reimport_files", "is_scanning",
                         "is_importing", "get_filesystem_path", "get_file_type",
                         "get_scanning_progress"],
    "EditorFileSystemDirectory": ["find_file_index", "get_file_import_is_valid"],
    "EditorInterface": ["get_resource_filesystem"],
    "ResourceLoader": ["load", "exists"],
}
SIGNALS = {
    "EditorFileSystem": ["resources_reimporting", "resources_reimported", "resources_reload",
                         "filesystem_changed", "sources_changed"],
}

# 8x8 opaque red, the same bytes the reimport probe uses.
PNG_B64 = ("iVBORw0KGgoAAAANSUhEUgAAAAgAAAAICAYAAADED76LAAAAG0lEQVR4nGNgYPj//z8DAwhh"
           "p/FKMoCo4WACAIMXf4EBK1dXAAAAAElFTkSuQmCC")
SVG = ('<svg xmlns="http://www.w3.org/2000/svg" width="8" height="8">'
       '<rect width="8" height="8" fill="#e04040"/></svg>\n')
CSV = "keys,en,es\nHELLO,Hello,Hola\n"
FONT_CANDIDATES = ["C:/Windows/Fonts/arial.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                   "/System/Library/Fonts/Supplemental/Arial.ttf"]
# The editor's per-resource folding state: UI memory, not import state, and its
# long names push a copy past Windows' path limit.
EDITOR_UI_STATE = shutil.ignore_patterns("*-folding-*")
ASSETS = ["sfx.wav", "music.ogg", "music.mp3", "tex.png", "icon.svg", "strings.csv", "font.ttf",
          "model.glb"]

GLB_SOURCE = r'''extends SceneTree

func _init() -> void:
    var root := Node3D.new()
    root.name = "Model"
    var box := MeshInstance3D.new()
    box.name = "Box"
    box.mesh = BoxMesh.new()
    root.add_child(box)
    box.owner = root
    var document := GLTFDocument.new()
    var state := GLTFState.new()
    var appended := document.append_from_scene(root, state)
    var written := document.write_to_filesystem(state, "res://model.glb")
    printerr("DIDI_ROW\tglb.write\tappend=%d write=%d" % [appended, written])
    root.free()
    printerr("DIDI_PROBE_DONE")
    quit()
'''

# Loads each asset the way a game would, bypassing the cache, and prints the
# properties an import option decides.
READER_SOURCE = r'''extends SceneTree

func _row(label: String, value: String) -> void:
    printerr("DIDI_ROW\t%s\t%s" % [label, value.replace("\n", " ")])


func _load(path: String) -> Resource:
    if not ResourceLoader.exists(path):
        return null
    return ResourceLoader.load(path, "", ResourceLoader.CACHE_MODE_IGNORE)


func _texture(path: String) -> String:
    var texture := _load(path) as Texture2D
    if texture == null:
        return "null"
    var text := "%s %dx%d" % [texture.get_class(), texture.get_width(), texture.get_height()]
    var image := texture.get_image()
    if image == null:
        return text + " image=null"
    return text + " mipmaps=%s format=%d compressed=%s" % [image.has_mipmaps(), image.get_format(), image.is_compressed()]


func _describe(name: String) -> String:
    var path := "res://" + name
    match name.get_extension():
        "wav":
            var wav := _load(path) as AudioStreamWAV
            if wav == null:
                return "null"
            return "loop_mode=%d loop_begin=%d loop_end=%d mix_rate=%d format=%d" % [wav.loop_mode, wav.loop_begin, wav.loop_end, wav.mix_rate, wav.format]
        "ogg", "mp3":
            var stream := _load(path)
            if stream == null:
                return "null"
            return "%s loop=%s loop_offset=%s" % [stream.get_class(), stream.get("loop"), stream.get("loop_offset")]
        "png", "svg":
            return _texture(path)
        "csv":
            var parts := PackedStringArray()
            for locale in ["en", "es"]:
                var translation := _load("res://strings.%s.translation" % locale) as Translation
                parts.append("%s=%s" % [locale, "null" if translation == null else "%s:%s" % [translation.get_class(), translation.get_message("HELLO")]])
            return " ".join(parts)
        "ttf":
            var font := _load(path) as FontFile
            if font == null:
                return "null"
            return "antialiasing=%d hinting=%d generate_mipmaps=%s subpixel=%d msdf=%s" % [font.antialiasing, font.hinting, font.generate_mipmaps, font.subpixel_positioning, font.multichannel_signed_distance_field]
        "glb":
            var scene := _load(path) as PackedScene
            if scene == null:
                return "null"
            var node := scene.instantiate()
            var text := "root=%s:%s children=%d" % [node.name, node.get_class(), node.get_child_count()]
            node.free()
            return text
    return "?"


func _init() -> void:
    var only := OS.get_environment("DIDI_PROBE_ONLY")
    for name in ["sfx.wav", "music.ogg", "music.mp3", "tex.png", "icon.svg", "strings.csv", "font.ttf", "model.glb"]:
        if only != "" and only != name:
            continue
        if FileAccess.file_exists("res://" + name):
            _row("load." + name, _describe(name))
    printerr("DIDI_PROBE_DONE")
    quit()
'''

PLUGIN_SOURCE = r'''@tool
extends EditorPlugin

const WAV := "res://sfx.wav"
const PNG := "res://tex.png"
const FRESH := "res://fresh.png"
const PNG_B64 := "__PNG_B64__"
const TRACE_CAP := 48

var fs: EditorFileSystem
var counts := {"reimporting": 0, "reimported": 0, "reload": 0, "filesystem_changed": 0}
var trace := PackedStringArray()
var in_call := false
var call_saw_signal := false
var before_frames := 0
var window_frames := 0
var reentry := ""
var held: AudioStreamWAV


func _enter_tree() -> void:
    fs = EditorInterface.get_resource_filesystem()
    fs.resources_reimporting.connect(func(r): _event("reimporting", r))
    fs.resources_reimported.connect(func(r): _event("reimported", r))
    fs.resources_reload.connect(func(r): _event("reload", r))
    fs.filesystem_changed.connect(func(): _event("filesystem_changed", null))
    set_process(true)
    _run.call_deferred()


func _note(text: String) -> void:
    if trace.size() < TRACE_CAP:
        trace.append(text)


func _event(kind: String, resources) -> void:
    counts[kind] += 1
    var files := ""
    if resources != null:
        var names := PackedStringArray()
        for path in resources:
            names.append(String(path).get_file())
        files = "(" + ",".join(names) + ")"
    _note(kind + files)
    if kind == "reimporting" and in_call:
        call_saw_signal = true
    # A plugin's _process never runs inside the editor's import pass, so the
    # pass is entered from its own signals: resources_reimporting fires while
    # the importing flag is set, filesystem_changed inside an open pass fires
    # in the tail after the flag is cleared and before resources_reimported.
    if reentry == "reimporting" and kind == "reimporting":
        reentry = ""
        _reenter("the resources_reimporting handler")
    elif reentry == "tail" and kind == "filesystem_changed" and _open():
        reentry = ""
        _reenter("a filesystem_changed handler inside the pass")


func _importing() -> String:
    return str(fs.call("is_importing")) if fs.has_method("is_importing") else "n/a"


func _open() -> bool:
    return counts["reimporting"] > counts["reimported"]


func _process(_delta: float) -> void:
    if in_call and not call_saw_signal:
        before_frames += 1
    elif _open():
        window_frames += 1


func _reenter(where: String) -> void:
    var before: int = counts["reimporting"]
    _note("reenter reimport_files(tex.png) from " + where)
    _row("reenter_is_importing", _importing())
    fs.reimport_files(PackedStringArray([PNG]))
    _row("reenter_own_reimporting_signals", counts["reimporting"] - before)
    _note("reenter returned")


func _row(label: String, value) -> void:
    printerr("DIDI_ROW\tlive.%s.%s.%s\t%s" % [variant, current, label, str(value).replace("\n", " ")])


func _begin(name: String) -> void:
    current = name
    printerr("DIDI_STEP\t%s.%s" % [variant, name])


var current := "startup"
var variant := OS.get_environment("DIDI_PROBE_VARIANT")


func _dest(path: String) -> String:
    var config := ConfigFile.new()
    if config.load(path + ".import") != OK:
        return ""
    return str(config.get_value("remap", "path", ""))


func _param_line(path: String, key: String) -> String:
    for line in FileAccess.get_file_as_string(path + ".import").split("\n"):
        if line.begins_with(key + "="):
            return line
    return "(absent)"


func _set_param(path: String, key: String, value: String) -> void:
    var lines := FileAccess.get_file_as_string(path + ".import").split("\n")
    var in_params := false
    var done := false
    for i in lines.size():
        if lines[i].begins_with("["):
            in_params = lines[i] == "[params]"
        elif in_params and lines[i].begins_with(key + "="):
            lines[i] = key + "=" + value
            done = true
    if not done:
        for i in lines.size():
            if lines[i] == "[params]":
                lines.insert(i + 2, key + "=" + value)
                break
    var file := FileAccess.open(path + ".import", FileAccess.WRITE)
    file.store_string("\n".join(lines))
    file.close()


func _wav(stream) -> String:
    return "null" if stream == null else "loop_mode=%d" % stream.loop_mode


func _settle() -> void:
    var started := Time.get_ticks_msec()
    var quiet := 0
    while quiet < 10 and Time.get_ticks_msec() - started < 20000:
        await get_tree().process_frame
        quiet = 0 if (fs.is_scanning() or _open()) else quiet + 1
    await get_tree().create_timer(0.5).timeout


func _call_reimport(paths: PackedStringArray) -> void:
    in_call = true
    call_saw_signal = false
    _note("call reimport_files")
    var frames := Engine.get_process_frames()
    fs.reimport_files(paths)
    _row("main_loop_frames_during_call", Engine.get_process_frames() - frames)
    _note("call returned")
    in_call = false


func _wait_second_start() -> void:
    # The editor compares a sidecar's modified time in whole seconds, so the
    # same-second cases start just after one begins. An unfocused editor runs
    # at a few frames a second and can step over a narrow moment every time,
    # so this waits on a timer; edit_kept_the_second says whether it worked.
    var into := fmod(Time.get_unix_time_from_system(), 1.0)
    await get_tree().create_timer(1.0 - into + 0.02).timeout


func _wait_next_second(path: String) -> void:
    var stamp := FileAccess.get_modified_time(path)
    while int(Time.get_unix_time_from_system()) <= stamp:
        await get_tree().process_frame


var mode_cycle := 1


func _next_mode() -> int:
    mode_cycle = 2 + (mode_cycle - 1) % 3
    return mode_cycle


func _edit_next() -> void:
    _set_param(WAV, "edit/loop_mode", str(_next_mode()))


func _reimport_then_edit() -> void:
    _call_reimport(PackedStringArray([WAV]))
    var stamp := FileAccess.get_modified_time(WAV + ".import")
    _edit_next()
    _row("edit_kept_the_second", FileAccess.get_modified_time(WAV + ".import") == stamp)


func _write_second_png() -> void:
    var file := FileAccess.open("res://fresh_two.png", FileAccess.WRITE)
    file.store_buffer(Marshalls.base64_to_raw(PNG_B64))
    file.close()


func _step(name: String, edit: Callable, act: Callable, wait_seconds: float) -> void:
    _begin(name)
    trace = PackedStringArray()
    before_frames = 0
    window_frames = 0
    var counts_before := counts.duplicate()
    edit.call()
    var dest := _dest(WAV)
    var dest_before := FileAccess.get_md5(dest)
    var written := FileAccess.get_file_as_string(WAV + ".import")
    act.call()
    if wait_seconds > 0.0:
        await get_tree().create_timer(wait_seconds).timeout
    await _settle()
    var deltas := PackedStringArray()
    for key in counts:
        if counts[key] != counts_before[key]:
            deltas.append("%s+%d" % [key, counts[key] - counts_before[key]])
    _row("signals", " ".join(deltas) if deltas.size() else "none")
    _row("wav_reimported", FileAccess.get_md5(_dest(WAV)) != dest_before or _dest(WAV) != dest)
    _row("sidecar_rewritten", FileAccess.get_file_as_string(WAV + ".import") != written)
    _row("loop_line", _param_line(WAV, "edit/loop_mode"))
    _row("bogus_line", _param_line(WAV, "didi/bogus"))
    _row("held", _wav(held))
    var fresh = ResourceLoader.load(WAV)
    _row("fresh", "%s same_object_as_held=%s" % [_wav(fresh), fresh == held])
    _row("ignore_cache", _wav(ResourceLoader.load(WAV, "", ResourceLoader.CACHE_MODE_IGNORE)))
    _row("process_calls_in_pass", "%d before the signal, %d inside" % [before_frames, window_frames])
    _row("trace", " > ".join(trace))


func _write_fresh_png() -> void:
    var file := FileAccess.open(FRESH, FileAccess.WRITE)
    file.store_buffer(Marshalls.base64_to_raw(PNG_B64))
    file.close()


func _run() -> void:
    await _settle()
    _begin("baseline")
    held = ResourceLoader.load(WAV) as AudioStreamWAV
    _row("held", _wav(held))
    _row("loop_line", _param_line(WAV, "edit/loop_mode"))
    await _step("behind_back", _edit_next, func(): pass, 3.0)
    await _wait_next_second(WAV + ".import")
    await _step("scan_sources_next_second", _edit_next, func(): fs.scan_sources(), 0.0)
    await _wait_second_start()
    await _step("scan_sources_same_second", _reimport_then_edit, func(): fs.scan_sources(), 0.0)
    await _wait_next_second(WAV + ".import")
    await _step("scan_next_second", _edit_next, func(): fs.scan(), 0.0)
    await _wait_second_start()
    await _step("scan_same_second", _reimport_then_edit, func(): fs.scan(), 0.0)
    await _wait_next_second(WAV + ".import")
    await _step("update_file", _edit_next, func(): fs.update_file(WAV), 2.0)
    await _step("reimport_files", _edit_next, func(): _call_reimport(PackedStringArray([WAV])), 0.0)
    await _step("unknown_key", func(): _set_param(WAV, "didi/bogus", "true"), func(): _call_reimport(PackedStringArray([WAV])), 0.0)
    reentry = "reimporting"
    await _step("reentry_while_importing", _edit_next, func(): _call_reimport(PackedStringArray([WAV])), 0.0)
    reentry = "tail"
    await _step("reentry_in_tail", _edit_next, func(): _call_reimport(PackedStringArray([WAV])), 0.0)
    reentry = ""
    await _step("new_file_scan", func(): _write_fresh_png(), func(): fs.scan(), 0.0)
    reentry = "tail"
    await _step("new_file_scan_reentry_in_tail", func(): _write_second_png(), func(): fs.scan(), 0.0)
    reentry = ""
    _begin("done")
    printerr("DIDI_PROBE_DONE")
    get_tree().quit()
'''.replace("__PNG_B64__", PNG_B64)


# ---------------------------------------------------------------------------
# Assets and projects


def write_wav(path: Path, seconds: float = 0.5, rate: int = 22050) -> None:
    frames = int(seconds * rate)
    with wave.open(str(path), "wb") as out:
        out.setnchannels(1)
        out.setsampwidth(2)
        out.setframerate(rate)
        out.writeframes(b"".join(struct.pack("<h", int(12000 * math.sin(2 * math.pi * 440 * i / rate)))
                                 for i in range(frames)))


def ffmpeg_encoders(ffmpeg: str | None) -> set[str]:
    if not ffmpeg:
        return set()
    out = subprocess.run([ffmpeg, "-hide_banner", "-encoders"], capture_output=True, text=True, timeout=60)
    return {line.split()[1] for line in out.stdout.splitlines() if len(line.split()) > 1 and line.startswith(" A")}


def encode(ffmpeg: str, encoders: set[str], source: Path, target: Path) -> str:
    codec = {"ogg": ["libvorbis", "vorbis"], "mp3": ["libmp3lame"]}[target.suffix[1:]]
    for name in codec:
        if name in encoders:
            extra = ["-strict", "-2"] if name == "vorbis" else []
            out = subprocess.run([ffmpeg, "-y", "-loglevel", "error", "-i", str(source), "-c:a", name, *extra,
                                  str(target)], capture_output=True, text=True, timeout=120)
            return f"{name} rc={out.returncode}" + (f" {out.stderr.strip()[:120]}" if out.returncode else "")
    return "no encoder"


def write_project(root: Path, version: str, *, plugin: bool = False) -> None:
    feature = ".".join(version.split(".")[:2])
    project = ["config_version=5", "", "[application]", "",
               'config/name="Didi import config probe"',
               f'config/features=PackedStringArray("{feature}")', ""]
    if plugin:
        project += ["[editor_plugins]", "",
                    'enabled=PackedStringArray("res://addons/didi_import_probe/plugin.cfg")', ""]
    (root / "project.godot").write_text("\n".join(project), encoding="utf-8", newline="\n")
    if plugin:
        addon = root / "addons" / "didi_import_probe"
        addon.mkdir(parents=True, exist_ok=True)
        (addon / "plugin.cfg").write_text(
            '[plugin]\n\nname="Didi import probe"\ndescription=""\nauthor=""\n'
            'version="1"\nscript="plugin.gd"\n', encoding="utf-8", newline="\n")
        (addon / "plugin.gd").write_text(PLUGIN_SOURCE, encoding="utf-8", newline="\n")


def sidecar_sections(text: str) -> dict[str, list[tuple[str, str]]]:
    """Top-level key=value lines per section; a continuation line is not a key."""
    sections: dict[str, list[tuple[str, str]]] = {}
    current = ""
    for line in text.splitlines():
        if line.startswith("[") and line.rstrip().endswith("]"):
            current = line.strip()[1:-1]
            sections.setdefault(current, [])
            continue
        match = re.match(r'^([A-Za-z_][A-Za-z0-9_/.\-]*)=(.*)$', line)
        if match:
            sections.setdefault(current, []).append((match.group(1), match.group(2)))
    return sections


def sidecar_value(text: str, section: str, key: str) -> str | None:
    for found, value in sidecar_sections(text).get(section, []):
        if found == key:
            return value
    return None


def edit_sidecar(text: str, op: str, key: str = "", value: str = "") -> str:
    lines = text.split("\n")
    section = ""
    out: list[str] = []
    done = False
    for line in lines:
        if line.startswith("["):
            section = line.strip()
        target = (section == "[params]" and op in ("set", "remove")) or (section == "[remap]" and op == "remap")
        if target and line.startswith(key + "="):
            done = True
            if op == "remove":
                continue
            out.append(f"{key}={value}")
            continue
        out.append(line)
    if not done and op in ("set", "raw"):
        index = out.index("[params]") + 1
        out.insert(index + 1, f"{key}={value}" if op == "set" else value)
    return "\n".join(out)


def md5(path: Path) -> str:
    return hashlib.md5(path.read_bytes()).hexdigest() if path.exists() else "absent"


def stamp(path: Path) -> int:
    return path.stat().st_mtime_ns if path.exists() else -1


def dest_files(project: Path, text: str) -> list[Path]:
    raw = sidecar_value(text, "deps", "dest_files") or "[]"
    return [project / p[len("res://"):] for p in re.findall(r'"(res://[^"]+)"', raw)]


# ---------------------------------------------------------------------------
# Parts


def binds(godot: str, work: Path, record: Record, engine: str) -> None:
    api_dir = work / "api"
    api_dir.mkdir(exist_ok=True)
    run([godot, "--headless", "--dump-extension-api"], cwd=api_dir, timeout=120)
    api_file = api_dir / "extension_api.json"
    if not api_file.exists():
        record.put(engine, "bind.(dump)", "no extension_api.json written")
        return
    api = json.loads(api_file.read_text(encoding="utf-8"))
    classes = {c["name"]: c for c in api["classes"]}
    for cls, methods in BINDS.items():
        by_name = {m["name"]: m for m in classes.get(cls, {}).get("methods", [])}
        for name in methods:
            method = by_name.get(name)
            value = "ABSENT" if method is None else str(method.get("hash"))
            record.put(engine, f"bind.{cls}.{name}", value)
    for cls, signals in SIGNALS.items():
        by_name = {s["name"]: s for s in classes.get(cls, {}).get("signals", [])}
        for name in signals:
            signal = by_name.get(name)
            value = "ABSENT" if signal is None else "(" + ", ".join(
                f"{a['name']}: {a['type']}" for a in signal.get("arguments", [])) + ")"
            record.put(engine, f"signal.{cls}.{name}", value)


def build_pristine(godot: str, version: str, work: Path, record: Record, engine: str,
                   ffmpeg: str | None, font: Path | None) -> Path:
    project = work / "pristine"
    if project.exists():
        shutil.rmtree(project)
    project.mkdir(parents=True)
    write_project(project, version)
    (project / "tex.png").write_bytes(base64.b64decode(PNG_B64))
    (project / "icon.svg").write_text(SVG, encoding="utf-8", newline="\n")
    (project / "strings.csv").write_text(CSV, encoding="utf-8", newline="\n")
    write_wav(project / "sfx.wav")
    encoders = ffmpeg_encoders(ffmpeg)
    for target in ("music.ogg", "music.mp3"):
        record.put(engine, f"asset.{target}", encode(ffmpeg, encoders, project / "sfx.wav", project / target)
                   if ffmpeg else "ffmpeg not on PATH")
        if (project / target).exists() and (project / target).stat().st_size == 0:
            (project / target).unlink()
    if font and font.exists():
        shutil.copyfile(font, project / "font.ttf")
        record.put(engine, "asset.font.ttf", font.name)
    else:
        record.put(engine, "asset.font.ttf", "no font found; pass --font")
    (project / "make_glb.gd").write_text(GLB_SOURCE, encoding="utf-8", newline="\n")
    _, text = run([godot, "--headless", "--path", str(project), "--script", "res://make_glb.gd"], timeout=120)
    record.take(engine, "asset.", text)
    (project / "make_glb.gd").unlink()
    _, text = run([godot, "--headless", "--path", str(project), "--import"], timeout=240)
    record.take(engine, "import.", text)
    (project / "reader.gd").write_text(READER_SOURCE, encoding="utf-8", newline="\n")
    for name in ASSETS:
        sidecar = project / (name + ".import")
        if not (project / name).exists():
            continue
        if not sidecar.exists():
            record.put(engine, f"import.{name}.sidecar", "absent")
            continue
        text = sidecar.read_text(encoding="utf-8", errors="replace")
        sections = sidecar_sections(text)
        remap = dict(sections.get("remap", []))
        params = sections.get("params", [])
        record.put(engine, f"import.{name}.importer", f"{remap.get('importer')} -> {remap.get('type')}"
                   + (f" valid={remap['valid']}" if "valid" in remap else ""))
        record.put(engine, f"import.{name}.param_keys", f"{len(params)}: " + ", ".join(k for k, _ in params))
        record.put(engine, f"import.{name}.defaults", " | ".join(f"{k}={v}" for k, v in params))
        record.put(engine, f"import.{name}.sections", ", ".join(sections))
    _, text = run([godot, "--headless", "--path", str(project), "--script", "res://reader.gd"], timeout=120)
    record.take(engine, "import.", text)
    return project


# (label, asset, op, key, value). A case whose key the pristine sidecar lacks is
# reported, not run, since that is itself the fact.
EDIT_CASES = [
    ("wav.loop_forward", "sfx.wav", "set", "edit/loop_mode", "2"),
    ("wav.loop_pingpong", "sfx.wav", "set", "edit/loop_mode", "3"),
    ("wav.loop_as_word", "sfx.wav", "set", "edit/loop_mode", '"forward"'),
    ("wav.loop_as_float", "sfx.wav", "set", "edit/loop_mode", "2.0"),
    ("wav.loop_as_bool", "sfx.wav", "set", "edit/loop_mode", "true"),
    ("wav.loop_out_of_range", "sfx.wav", "set", "edit/loop_mode", "99"),
    ("wav.loop_negative", "sfx.wav", "set", "edit/loop_mode", "-1"),
    ("wav.loop_removed", "sfx.wav", "remove", "edit/loop_mode", ""),
    ("wav.loop_unparseable", "sfx.wav", "set", "edit/loop_mode", ")"),
    ("wav.unknown_key", "sfx.wav", "set", "didi/bogus", "true"),
    ("wav.comment_only", "sfx.wav", "raw", "", "; a comment a writer left"),
    ("wav.max_rate", "sfx.wav", "set", "force/max_rate", "true"),
    ("wav.importer_keep", "sfx.wav", "remap", "importer", '"keep"'),
    ("ogg.loop_true", "music.ogg", "set", "loop", "true"),
    ("ogg.loop_as_int", "music.ogg", "set", "loop", "1"),
    ("ogg.loop_as_word", "music.ogg", "set", "loop", '"yes"'),
    ("ogg.loop_offset", "music.ogg", "set", "loop_offset", "0.25"),
    ("mp3.loop_true", "music.mp3", "set", "loop", "true"),
    ("png.mipmaps", "tex.png", "set", "mipmaps/generate", "true"),
    ("png.mipmaps_as_int", "tex.png", "set", "mipmaps/generate", "1"),
    ("png.vram", "tex.png", "set", "compress/mode", "2"),
    ("png.mode_out_of_range", "tex.png", "set", "compress/mode", "9"),
    ("png.quality_out_of_hint", "tex.png", "set", "compress/lossy_quality", "5.0"),
    ("svg.scale", "icon.svg", "set", "svg/scale", "2.0"),
    ("svg.scale_as_int", "icon.svg", "set", "svg/scale", "2"),
    ("csv.uncompressed", "strings.csv", "set", "compress", "false"),
    ("ttf.no_antialiasing", "font.ttf", "set", "antialiasing", "0"),
    ("glb.root_name", "model.glb", "set", "nodes/root_name", '"Imported"'),
    ("glb.root_type", "model.glb", "set", "nodes/root_type", '"Node2D"'),
]


def edits(godot: str, work: Path, pristine: Path, record: Record, engine: str) -> None:
    for label, asset, op, key, value in EDIT_CASES:
        prefix = f"edit.{label}."
        source_sidecar = pristine / (asset + ".import")
        if not source_sidecar.exists():
            record.put(engine, prefix + "(skipped)", f"no {asset} on this run")
            continue
        original = source_sidecar.read_text(encoding="utf-8", errors="replace")
        section = "remap" if op == "remap" else "params"
        if op in ("set", "remove", "remap") and key and sidecar_value(original, section, key) is None \
                and key != "didi/bogus":
            record.put(engine, prefix + "(skipped)", f"{key} is not a key this engine writes")
            continue
        case = work / "edits" / label
        if case.exists():
            shutil.rmtree(case)
        shutil.copytree(pristine, case, ignore=EDITOR_UI_STATE)
        sidecar = case / (asset + ".import")
        edited = edit_sidecar(original, op, key, value)
        sidecar.write_text(edited, encoding="utf-8", newline="\n")
        before = {str(p): md5(p) for p in dest_files(case, original)}
        stamps = {str(p): stamp(p) for p in dest_files(case, original)}
        uid_before = sidecar_value(original, "remap", "uid")
        _, text = run([godot, "--headless", "--path", str(case), "--import"], timeout=240)
        record.take(engine, prefix, text)
        after_text = sidecar.read_text(encoding="utf-8", errors="replace") if sidecar.exists() else ""
        # Written is the importer having run; changed is it having produced
        # something different. A reimport with the same options can be the
        # first without the second.
        after = {str(p): md5(p) for p in dest_files(case, after_text)}
        written = any(stamp(Path(k)) != v for k, v in stamps.items()) or set(after) != set(before)
        changed = any(after.get(k) != v for k, v in before.items()) or set(after) != set(before)
        record.put(engine, prefix + "output", f"written={written} changed={changed}")
        record.put(engine, prefix + "rewritten", str(after_text != edited))
        line_key = key if op in ("set", "remove") and key else ("importer" if op == "remap" else "")
        line_section = "remap" if op == "remap" else "params"
        if line_key:
            found = sidecar_value(after_text, line_section, line_key)
            record.put(engine, prefix + "line_after", "(absent)" if found is None else f"{line_key}={found}")
        if op == "raw":
            record.put(engine, prefix + "comment_kept", str(value in after_text))
        valid = sidecar_value(after_text, "remap", "valid")
        record.put(engine, prefix + "valid", "(no valid line)" if valid is None else valid)
        uid_after = sidecar_value(after_text, "remap", "uid")
        record.put(engine, prefix + "uid", "kept" if uid_after == uid_before else f"{uid_before} -> {uid_after}")
        env = dict(os.environ, DIDI_PROBE_ONLY=asset)
        _, text = run([godot, "--headless", "--path", str(case), "--script", "res://reader.gd"], env=env,
                      timeout=120)
        record.take(engine, prefix, text)


def editor(godot: str, version: str, work: Path, pristine: Path, record: Record, engine: str,
           variants: list[str]) -> None:
    # A headless editor sends its progress to the console, so nothing pumps the
    # main loop inside an import pass there. The editor Didi serves, and the
    # one the live harness runs, is a windowed one, and only that one shows
    # what happens inside the pass.
    for variant in variants:
        project = work / f"editor_{variant}"
        if project.exists():
            shutil.rmtree(project)
        shutil.copytree(pristine, project, ignore=EDITOR_UI_STATE)
        write_project(project, version, plugin=True)
        run([godot, "--headless", "--path", str(project), "--import"], timeout=240)
        env = dict(os.environ, DIDI_PROBE_VARIANT=variant)
        flags = ["--headless"] if variant == "headless" else []
        code, text = run([godot, *flags, "--editor", "--path", str(project)], env=env, timeout=300)
        done = record.take(engine, "", text)
        record.put(engine, f"live.{variant}.(run)", f"rc={code} finished={'yes' if done else 'NO'}")
        sidecar = project / "sfx.wav.import"
        after = sidecar.read_text(encoding="utf-8", errors="replace") if sidecar.exists() else ""
        record.put(engine, f"live.{variant}.after_quit.loop_line",
                   f"edit/loop_mode={sidecar_value(after, 'params', 'edit/loop_mode')}")
        record.put(engine, f"live.{variant}.after_quit.bogus_line",
                   str(sidecar_value(after, "params", "didi/bogus")))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--godot", action="append", default=[],
                        help="a Godot console binary; repeat for each engine line")
    parser.add_argument("--out", help="where the throwaway projects go (default: a temp dir)")
    parser.add_argument("--font", help="a .ttf to import (default: a system font if one is found)")
    parser.add_argument("--only", choices=("binds", "edits", "editor"), action="append",
                        help="run one part; repeat for several (the import itself always runs)")
    parser.add_argument("--headless-only", action="store_true",
                        help="skip the windowed editor, for a host with no display")
    args = parser.parse_args()
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    engines = args.godot or ([os.environ["GODOT_BIN"]] if os.environ.get("GODOT_BIN") else [])
    if not engines:
        print("pass --godot (repeatable) or set GODOT_BIN: this probe has no witness without one")
        return 2
    parts = args.only or ["binds", "edits", "editor"]
    ffmpeg = shutil.which("ffmpeg")
    font = Path(args.font) if args.font else next((Path(p) for p in FONT_CANDIDATES if Path(p).exists()), None)
    base = Path(args.out) if args.out else Path(tempfile.mkdtemp(prefix="didi_import_probe_"))
    print(f"working under {base}")
    print(f"ffmpeg: {ffmpeg or 'not on PATH, so no OGG or MP3 rows'}; font: {font or 'none'}")
    record = Record()
    versions = [engine_version(godot) for godot in engines]

    def one(godot: str, version: str) -> None:
        work = base / re.sub(r"[^0-9A-Za-z.]+", "_", version)
        work.mkdir(parents=True, exist_ok=True)
        if "binds" in parts:
            binds(godot, work, record, version)
        pristine = build_pristine(godot, version, work, record, version, ffmpeg, font)
        if "edits" in parts:
            edits(godot, work, pristine, record, version)
        if "editor" in parts:
            editor(godot, version, work, pristine, record, version,
                   ["headless"] if args.headless_only else ["headless", "gui"])
        print(f"  finished {version}  ({godot})", flush=True)

    with ThreadPoolExecutor(max_workers=len(engines)) as pool:
        for future in [pool.submit(one, g, v) for g, v in zip(engines, versions)]:
            future.result()
    record.report(versions)
    return 0


if __name__ == "__main__":
    sys.exit(main())
