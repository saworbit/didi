"""Which Controls draw their `text` translated, asked of the engine rather than of Didi.

#988: `ui_list_controls` reports a Control's `text` property, which in a
localised game is the translation key while the player reads the translation.
The fix reports what is drawn as well, and `Node.atr(text)` is the call a Label
makes when it draws. But not every Control with a `text` property translates
it: a `LineEdit`'s text is what the user typed, and reporting `atr()` of it
would invent a translation nobody sees. So before the fix decides which classes
get a `displayed_text`, this settles it, on every engine it is given:

* **What each class draws.** Every instantiable Control class with a `text`
  property is rendered three times in a windowed run, each in its own
  SubViewport: auto-translating with the key `MENU_START` as its text, and two
  twins with `auto_translate_mode` disabled, one holding the key and one holding
  its translation. The first run's pixels are compared with each twin's. A class
  that draws like the translation twin translates its `text`; one that draws
  like the key twin does not. Recorded beside it: what `atr(text)` and
  `can_auto_translate()` answer on the auto-translating copy and on the twin.
* **What an editor draws.** A headless editor with the project's translations
  registered and `internationalization/locale/test` at `fr` opens a menu scene.
  A probe plugin reads the edited scene's Label: its locale, the loaded locales,
  `atr(text)`, `can_auto_translate()`, and what it draws, judged by the engine's
  own text shaping (character count and minimum width) against two twins.
  An editor that shows keys while `atr()` answers a translation would make
  `atr()` the wrong witness there.

Evidence for the #988 fix. Needs no Didi build, no addon and no MCP server. The
`classes` part opens a window for a few seconds per engine. Every project is a
throwaway under `--out`.

    python tools/vibe/probes/control_text_engine.py --godot C:/Godot/Godot_v4.5.1-stable_win64_console.exe \\
        --godot C:/Godot/Godot_v4.6.2-stable_win64_console.exe --godot C:/Godot/Godot_v4.7.2-stable_win64_console.exe
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from probes.audio_bus_engine import Record, engine_version, run  # noqa: E402
from probes.import_config_engine import sidecar_sections  # noqa: E402

KEY = "MENU_START"
TRANSLATED = "Commencer la partie"

# Rendered, so run with a window. Each class gets three SubViewports and the
# pixels decide; a property cannot answer what a Control draws.
CLASSES_SOURCE = r'''extends SceneTree

const KEY := "MENU_START"
const TRANSLATED := "Commencer la partie"


func _row(label: String, value: String) -> void:
	printerr("DIDI_ROW\t%s\t%s" % [label, value])


func _has_text(class_name_: String) -> bool:
	for property in ClassDB.class_get_property_list(class_name_):
		if property["name"] == "text" and property["type"] == TYPE_STRING:
			return true
	return false


func _view(class_name_: String, text: String, mode: int) -> Array:
	var view := SubViewport.new()
	view.size = Vector2i(320, 64)
	view.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	view.transparent_bg = false
	var control := ClassDB.instantiate(class_name_) as Control
	control.auto_translate_mode = mode
	control.set("text", text)
	view.add_child(control)
	control.position = Vector2(4, 4)
	control.size = Vector2(312, 56)
	root.add_child(view)
	return [view, control]


func _initialize() -> void:
	_run.call_deferred()


func _run() -> void:
	var translation := Translation.new()
	translation.locale = "fr"
	translation.add_message(KEY, TRANSLATED)
	TranslationServer.add_translation(translation)
	TranslationServer.set_locale("fr")
	var names := []
	for class_name_ in ClassDB.get_class_list():
		if ClassDB.is_parent_class(class_name_, "Control") and ClassDB.can_instantiate(class_name_) \
				and _has_text(class_name_):
			names.append(class_name_)
	names.sort()
	_row("classes", ", ".join(names))
	for class_name_ in names:
		printerr("DIDI_STEP\t" + class_name_)
		var auto := _view(class_name_, KEY, Node.AUTO_TRANSLATE_MODE_INHERIT)
		var as_key := _view(class_name_, KEY, Node.AUTO_TRANSLATE_MODE_DISABLED)
		var as_translation := _view(class_name_, TRANSLATED, Node.AUTO_TRANSLATE_MODE_DISABLED)
		for i in 4:
			await RenderingServer.frame_post_draw
		var a: PackedByteArray = auto[0].get_texture().get_image().get_data()
		var k: PackedByteArray = as_key[0].get_texture().get_image().get_data()
		var t: PackedByteArray = as_translation[0].get_texture().get_image().get_data()
		var verdict := "draws neither the key nor the translation"
		if a == k and a == t:
			verdict = "draws the same either way"
		elif a == t:
			verdict = "draws the translation"
		elif a == k:
			verdict = "draws the key"
		_row(class_name_ + ".draws", verdict)
		var control: Control = auto[1]
		_row(class_name_ + ".atr", String(control.atr(control.get("text"))))
		_row(class_name_ + ".can_auto_translate", str(control.can_auto_translate()))
		var twin: Control = as_key[1]
		_row(class_name_ + ".twin_atr", String(twin.atr(twin.get("text"))))
		for pair in [auto, as_key, as_translation]:
			pair[0].queue_free()
		await process_frame
	printerr("DIDI_PROBE_DONE")
	quit()
'''

STRINGS_CSV = f"keys,en,fr\n{KEY},Start,{TRANSLATED}\n"

MENU_TSCN = f'''[gd_scene format=3]

[node name="Menu" type="Control"]

[node name="Title" type="Label" parent="."]
text = "{KEY}"

[node name="Play" type="Button" parent="."]
offset_top = 40.0
text = "{KEY}"
'''

EDITOR_PLUGIN = r'''@tool
extends EditorPlugin


func _row(label: String, value: String) -> void:
	printerr("DIDI_ROW\t%s\t%s" % [label, value])


func _enter_tree() -> void:
	_run.call_deferred()


func _measure(control: Control) -> String:
	var width := int(round(control.get_minimum_size().x))
	if control is Label:
		return "%d chars, %d px" % [(control as Label).get_total_character_count(), width]
	return "%d px" % width


func _twin(beside: Control, text: String) -> Control:
	var twin := ClassDB.instantiate(beside.get_class()) as Control
	twin.auto_translate_mode = Node.AUTO_TRANSLATE_MODE_DISABLED
	twin.text = text
	beside.get_parent().add_child(twin)
	return twin


func _run() -> void:
	await get_tree().create_timer(2.0).timeout
	EditorInterface.open_scene_from_path("res://menu.tscn")
	await get_tree().create_timer(2.0).timeout
	var scene := EditorInterface.get_edited_scene_root()
	if scene == null:
		_row("editor.scene", "no edited scene")
		printerr("DIDI_PROBE_DONE")
		get_tree().quit()
		return
	var loaded := Array(TranslationServer.get_loaded_locales())
	loaded.sort()
	_row("editor.locale", TranslationServer.get_locale())
	_row("editor.loaded_locales", str(loaded))
	_row("editor.tr", String(scene.tr("MENU_START")))
	for name in ["Title", "Play"]:
		var control := scene.get_node(name) as Control
		_row("editor.%s.auto_translate_mode" % name, str(control.auto_translate_mode))
		_row("editor.%s.can_auto_translate" % name, str(control.can_auto_translate()))
		_row("editor.%s.atr" % name, String(control.atr(control.text)))
		var as_key := _twin(control, control.text)
		var as_translation := _twin(control, String(control.atr(control.text)))
		await get_tree().process_frame
		var shown := _measure(control)
		var key_size := _measure(as_key)
		var tr_size := _measure(as_translation)
		var verdict := "neither"
		if key_size == tr_size:
			verdict = "the key, and atr() returns the key" if shown == key_size else verdict
		elif shown == tr_size:
			verdict = "what atr() returns"
		elif shown == key_size:
			verdict = "the key"
		_row("editor.%s.draws" % name, "%s (shown %s; key %s; atr %s)" % [verdict, shown, key_size, tr_size])
		as_key.free()
		as_translation.free()
	printerr("DIDI_PROBE_DONE")
	get_tree().quit()
'''


def write(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8", newline="\n")


def project_godot(version: str, extra: str = "") -> str:
    feature = ".".join(version.split(".")[:2])
    return (f'config_version=5\n\n[application]\n\nconfig/name="Didi control text probe"\n'
            f'config/features=PackedStringArray("{feature}")\n\n{extra}')


def fresh(root: Path) -> None:
    if root.exists():
        shutil.rmtree(root)
    root.mkdir(parents=True)


def classes(godot: str, version: str, work: Path, record: Record, engine: str) -> None:
    project = work / "classes"
    fresh(project)
    write(project / "project.godot", project_godot(version))
    write(project / "probe.gd", CLASSES_SOURCE)
    # A window, not --headless: a headless engine has no rendering device and
    # every SubViewport would read back empty.
    _, text = run([godot, "--path", str(project), "--script", "res://probe.gd",
                   "--resolution", "400x200"], timeout=240)
    if not record.take(engine, "classes.", text):
        record.put(engine, "classes.finished", "NO")


def editor(godot: str, version: str, work: Path, record: Record, engine: str) -> None:
    project = work / "editor"
    fresh(project)
    write(project / "project.godot", project_godot(version))
    write(project / "strings.csv", STRINGS_CSV)
    write(project / "menu.tscn", MENU_TSCN)
    run([godot, "--headless", "--path", str(project), "--import"], timeout=240)
    sidecar = project / "strings.csv.import"
    deps = dict(sidecar_sections(sidecar.read_text(encoding="utf-8")).get("deps", [])) if sidecar.exists() else {}
    dests = re.findall(r'res://[^"]+', deps.get("dest_files", ""))
    record.put(engine, "editor.registered", ", ".join(dests) or "nothing imported")
    listed = ", ".join(f'"{d}"' for d in dests)
    write(project / "project.godot", project_godot(
        version,
        f'[editor_plugins]\n\nenabled=PackedStringArray("res://addons/didi_text_probe/plugin.cfg")\n\n'
        f'[internationalization]\n\nlocale/translations=PackedStringArray({listed})\nlocale/test="fr"\n'))
    write(project / "addons" / "didi_text_probe" / "plugin.cfg",
          '[plugin]\n\nname="Didi control text probe"\ndescription=""\nauthor=""\nversion="1"\nscript="plugin.gd"\n')
    write(project / "addons" / "didi_text_probe" / "plugin.gd", EDITOR_PLUGIN)
    _, text = run([godot, "--headless", "--editor", "--path", str(project)], timeout=180)
    if not record.take(engine, "", text):
        record.put(engine, "editor.finished", "NO")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--godot", action="append", default=[],
                        help="a Godot console binary; repeat for each engine line")
    parser.add_argument("--out", help="where the throwaway projects go (default: a temp dir)")
    parser.add_argument("--only", choices=("classes", "editor"), action="append",
                        help="run one part; repeat for several")
    args = parser.parse_args()
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    engines = args.godot or ([os.environ["GODOT_BIN"]] if os.environ.get("GODOT_BIN") else [])
    if not engines:
        print("pass --godot (repeatable) or set GODOT_BIN: this probe has no witness without one")
        return 2
    parts = args.only or ["classes", "editor"]
    base = Path(args.out) if args.out else Path(tempfile.mkdtemp(prefix="didi_control_text_probe_"))
    print(f"working under {base}")
    record = Record()
    versions = [engine_version(godot) for godot in engines]

    def one(godot: str, version: str) -> None:
        work = base / re.sub(r"[^0-9A-Za-z.]+", "_", version)
        work.mkdir(parents=True, exist_ok=True)
        if "classes" in parts:
            classes(godot, version, work, record, version)
        if "editor" in parts:
            editor(godot, version, work, record, version)
        print(f"  finished {version}  ({godot})", flush=True)

    # One engine at a time: the classes part opens a window, and three at once
    # would fight over focus and the GPU for no gain in a run this short.
    for godot, version in zip(engines, versions):
        one(godot, version)
    record.report(versions)
    return 0


if __name__ == "__main__":
    sys.exit(main())
