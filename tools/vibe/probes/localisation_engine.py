"""What localising a game and shipping its data files does, asked of the engine rather than of Didi.

#779 is the last open half of "the surface can build a game and cannot ship
it": nothing writes a translation `.csv` or a `.json` of game data. The
`project_add_export_preset` amendment kept that half out "for a separate
amendment with its own security review", and a general text writer is refused
already (`override.cfg`, measured by `export_preset_engine.py`). Writing the
bytes is the part an agent with a file system can do without Didi. What it
cannot see is what the engine then does with them, and whether a player sees
the result. Before a tool is designed, this probe settles the facts it would
depend on, on every engine it is given:

* **What the CSV importer does with each shape a writer might produce.** A
  translation CSV in the documented shape, then non-ASCII text in UTF-8 with
  and without a byte-order mark and in Windows-1252, CRLF line endings, a
  quoted cell holding a comma, a doubled quote and a line break, blank lines,
  a first header cell other than `keys`, a `_notes` column, locale headers in
  several spellings, a duplicate key, a semicolon file with and without a
  sidecar written first that sets the delimiter, a header with no rows and an
  empty file. Then game data rather than strings (`name,hp,speed`), imported
  as it falls and with a sidecar written first naming the `keep` and the
  `skip` importer. Each case is imported in its own project, so every importer
  line the engine prints belongs to one file. Recorded: the importer and type
  the sidecar names, the files it declares, whether a sidecar written first
  survives, the importer's options, and what each declared translation loads
  as, with its locale and the messages it holds.
* **What a running game shows.** A menu with a `Label` and a `Button` whose
  text is a key, run as the game under seven settings: nothing registered,
  the imported `.translation` files registered, then with
  `internationalization/locale/test` at `fr`, at `fr_CA` and at `de` (neither
  is a column), the `.csv` itself registered instead, and a missing file
  registered beside the real ones. Recorded: the locale, the loaded locales,
  `tr()` for each key, what `text` and `get_text()` return (which is what
  `ui_list_controls` reads), what `atr()` returns, and what the control
  actually displays. That last is judged by the engine's own text shaping:
  the control's character count and minimum width against two twins that never
  translate, one holding the key and one holding `tr(key)`. Then
  `TranslationServer.set_locale` from inside the game, and whether the label
  follows. The first run, with nothing registered, also opens every data file,
  which is the control for the export below.
* **What an export ships.** The same project exported with `--export-pack`
  under the default filters and with an include filter for `.txt`, `.json`
  and `.csv`, and the pack run as a game from an empty directory. Recorded:
  which data files the game can still open, whether the JSON loads, and
  whether the translations still apply.
* **What `locale/translations` can name.** Each kind of file registered alone,
  with `locale/test` at `fr`, and the game asked what loaded: the imported
  `.translation` files, a `.po` written by hand, a `.mo` written here, a
  Translation the engine saved as `.tres` and as `.res`, a `.tres` holding
  something else, a `.json`, the source `.csv`, a file that is not there, and
  a list naming one file twice. The evidence for #989's rules in
  `project_set_setting`.

Everything here is evidence for a localisation and data-file amendment in
`docs/SURFACE_AMENDMENTS.md` (#779). Needs no Didi build, no addon and no MCP
server. Every project is a throwaway under `--out` (a temporary directory by
default), and the files the engine wrote are kept there as evidence. Non-ASCII
text in a row is printed as `<U+XXXX>`, so a row reads the same on any console.

    python tools/vibe/probes/localisation_engine.py --godot C:/Godot/Godot_v4.5.1-stable_win64_console.exe \\
        --godot C:/Godot/Godot_v4.6.2-stable_win64_console.exe --godot C:/Godot/Godot_v4.7.2-stable_win64_console.exe
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import struct
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from probes.audio_bus_engine import Record, engine_version, run  # noqa: E402
from probes.export_preset_engine import BASE_KEYS, BASE_OPTIONS, PRESET, preset_text  # noqa: E402
from probes.import_config_engine import sidecar_sections  # noqa: E402

# Built from code points so this file stays ASCII and no tool on the way to it
# can rewrite an escape.
FR_SETTINGS = "Param" + chr(0xE8) + "tres"
JA_SETTINGS = chr(0x8A2D) + chr(0x5B9A)
BOM = b"\xef\xbb\xbf"

BASIC = "keys,en,fr\nMENU_START,Start,Commencer la partie\nEMPTY_FR,Only English,\n"
NON_ASCII = f"keys,en,fr,ja\nMENU_SETTINGS,Settings,{FR_SETTINGS},{JA_SETTINGS}\n"
SEMICOLON = "keys;en;fr\nMENU_START;Start;Commencer\n"
DATA = "name,hp,speed\nslime,10,1.5\ngoblin,25,2.0\n"

SEMICOLON_SIDECAR = '[remap]\n\nimporter="csv_translation"\ntype="Translation"\n\n[params]\n\ndelimiter=1\n'
KEEP_SIDECAR = '[remap]\n\nimporter="keep"\n'
SKIP_SIDECAR = '[remap]\n\nimporter="skip"\n'


def utf8(text: str) -> bytes:
    return text.encode("utf-8")


# (stem, what it asks, file bytes, a sidecar written before the first import).
CSV_CASES: list[tuple[str, str, bytes, str | None]] = [
    ("tr_basic", "the documented shape: keys, then a column per locale; one fr cell empty",
     utf8(BASIC), None),
    ("tr_utf8", "fr and ja text outside ASCII, UTF-8 with no byte-order mark",
     utf8(NON_ASCII), None),
    ("tr_utf8_bom", "the same with a UTF-8 byte-order mark",
     BOM + utf8(NON_ASCII), None),
    ("tr_cp1252", "the same fr text in Windows-1252 rather than UTF-8",
     f"keys,en,fr\nMENU_SETTINGS,Settings,{FR_SETTINGS}\n".encode("cp1252"), None),
    ("tr_crlf", "the documented shape with CRLF line endings",
     utf8(BASIC.replace("\n", "\r\n")), None),
    ("tr_quoted", "a quoted cell holding a comma, a doubled quote and a line break",
     utf8('keys,en,fr\nGREETING,"Hello, ""friend""","Bonjour,\nami"\n'), None),
    ("tr_blank_lines", "a blank line between two rows and two at the end",
     utf8("keys,en,fr\nMENU_START,Start,Commencer\n\nEMPTY_FR,Only English,Seulement\n\n\n"), None),
    ("tr_header_id", "the first header cell id rather than keys",
     utf8("id,en,fr\nMENU_START,Start,Commencer\n"), None),
    ("tr_underscore_column", "a _notes column between two locales",
     utf8("keys,en,_notes,fr\nMENU_START,Start,for translators,Commencer\n"), None),
    ("tr_locale_forms", "locale headers fr_FR, pt-BR, ZH_cn and french",
     utf8("keys,fr_FR,pt-BR,ZH_cn,french\nMENU_START,Commencer,Iniciar,Kaishi,Commencer\n"), None),
    ("tr_duplicate_key", "one key on two rows",
     utf8("keys,en,fr\nDUP,first,premier\nDUP,second,second\n"), None),
    ("tr_semicolon", "semicolon separated, the importer's default options",
     utf8(SEMICOLON), None),
    ("tr_semicolon_sidecar", "semicolon separated, a sidecar written first with delimiter=1",
     utf8(SEMICOLON), SEMICOLON_SIDECAR),
    ("tr_header_only", "a header row and nothing under it",
     utf8("keys,en,fr\n"), None),
    ("tr_empty", "an empty file",
     b"", None),
    ("data_plain", "game data rather than strings (name,hp,speed), imported as it falls",
     utf8(DATA), None),
    ("data_keep", "the same data, a sidecar written first naming the keep importer",
     utf8(DATA), KEEP_SIDECAR),
    ("data_skip", "the same data, a sidecar written first naming the skip importer",
     utf8(DATA), SKIP_SIDECAR),
]

# Loading a CSV's own path prints two engine errors whatever it holds, so it is
# asked once per importer rather than once per case.
LOAD_SOURCE = {"tr_basic", "data_plain", "data_keep", "data_skip"}

# Loads every translation a CSV's sidecar declares, bypassing the cache. Each
# case is its own project, so an importer warning belongs to one file.
READER_SOURCE = r'''extends SceneTree

const KEYS := ["MENU_START", "MENU_SETTINGS", "GREETING", "EMPTY_FR", "DUP", "slime", "goblin"]


func _esc(value: String) -> String:
	var out := ""
	for i in value.length():
		var code := value.unicode_at(i)
		out += value[i] if code >= 32 and code < 127 else "<U+%04X>" % code
	return out


func _row(label: String, value: String) -> void:
	printerr("DIDI_ROW\t%s\t%s" % [label, _esc(value)])


func _step(name: String) -> void:
	printerr("DIDI_STEP\t%s" % name)


func _translation(path: String) -> String:
	if not ResourceLoader.exists(path):
		return "no loader for it"
	var resource := ResourceLoader.load(path, "", ResourceLoader.CACHE_MODE_IGNORE)
	var translation := resource as Translation
	if translation == null:
		return "loads as %s, not a Translation" % ("null" if resource == null else resource.get_class())
	var parts := PackedStringArray()
	for key in KEYS:
		var message := String(translation.get_message(key))
		if message != "":
			parts.append("%s=%s" % [key, message])
	var text := "%s locale=%s %s" % [translation.get_class(), translation.locale,
			" ".join(parts) if parts.size() > 0 else "(none of the probe's keys)"]
	# An OptimizedTranslation keeps hashes, not its keys, so only the plain
	# class can say what else it holds.
	if translation.get_class() == "Translation":
		text += " keys=%s" % str(translation.get_message_list())
	return text


func _source(name: String) -> String:
	var path := "res://" + name
	var text := "file=%s loader=%s" % [FileAccess.file_exists(path), ResourceLoader.exists(path)]
	if ResourceLoader.exists(path) and OS.get_environment("DIDI_LOAD_SOURCE") == "1":
		var resource := ResourceLoader.load(path, "", ResourceLoader.CACHE_MODE_IGNORE)
		text += " loads as %s" % ("null" if resource == null else resource.get_class())
	return text


func _case(name: String) -> void:
	var stem := name.get_basename()
	_row("source", _source(name))
	if not FileAccess.file_exists("res://" + name + ".import"):
		return
	var sidecar := ConfigFile.new()
	var err := sidecar.load("res://" + name + ".import")
	if err != OK:
		_row("sidecar", "does not load: " + error_string(err))
		return
	for dest in sidecar.get_value("deps", "dest_files", []):
		_row("loads." + String(dest).get_file().trim_prefix(stem + "."), _translation(dest))


func _init() -> void:
	_step("read")
	var names := []
	for name in DirAccess.get_files_at("res://"):
		if name.get_extension() == "csv":
			names.append(name)
	names.sort()
	for name in names:
		_case(name)
	printerr("DIDI_PROBE_DONE")
	quit()
'''

GAME_CSV = (f"keys,en,fr\nMENU_START,Start,Commencer la partie\n"
            f"MENU_SETTINGS,Settings,{FR_SETTINGS}\nEMPTY_FR,Only English,\n")
DATA_JSON = '{"slime": {"hp": 10, "speed": 1.5}, "goblin": {"hp": 25, "speed": 2.0}}\n'
NOTES_TXT = "Level notes: the goblin guards the bridge.\n"

MAIN_TSCN = '''[gd_scene load_steps=2 format=3]

[ext_resource type="Script" path="res://main.gd" id="1"]

[node name="Main" type="Control"]
script = ExtResource("1")

[node name="Label" type="Label" parent="."]
text = "MENU_START"

[node name="Button" type="Button" parent="."]
offset_top = 40.0
text = "MENU_START"
'''

# The game. Every row goes to stderr, where the engine's own lines are, so the
# two arrive in the order they happened.
GAME_SOURCE = r'''extends Control

const KEYS := ["MENU_START", "MENU_SETTINGS", "EMPTY_FR", "NOT_A_KEY"]
const FILES := ["res://data.json", "res://notes.txt", "res://data_keep.csv", "res://data_skip.csv",
		"res://data_plain.csv", "res://strings.csv", "res://strings.fr.translation"]


func _esc(value: String) -> String:
	var out := ""
	for i in value.length():
		var code := value.unicode_at(i)
		out += value[i] if code >= 32 and code < 127 else "<U+%04X>" % code
	return out


func _row(label: String, value: String) -> void:
	printerr("DIDI_ROW\t%s\t%s" % [label, _esc(value)])


func _step(name: String) -> void:
	printerr("DIDI_STEP\t%s" % name)


func _measure(control: Control) -> String:
	var width := int(round(control.get_minimum_size().x))
	if control is Label:
		return "%d chars, %d px" % [(control as Label).get_total_character_count(), width]
	return "%d px" % width


func _twin(control: Control, text: String) -> Control:
	var twin := ClassDB.instantiate(control.get_class()) as Control
	twin.auto_translate_mode = Node.AUTO_TRANSLATE_MODE_DISABLED
	twin.set("text", text)
	add_child(twin)
	return twin


# What a control displays, judged by the engine's own shaping rather than by a
# property: its size against a twin holding the key and one holding tr(key),
# neither of which translates.
func _shows(control: Control) -> Array:
	var key: String = control.get("text")
	var as_key := _twin(control, key)
	var as_tr := _twin(control, tr(key))
	await get_tree().process_frame
	var shown := _measure(control)
	var key_size := _measure(as_key)
	var tr_size := _measure(as_tr)
	as_key.queue_free()
	as_tr.queue_free()
	var verdict := "neither the key nor tr(key)"
	if key_size == tr_size:
		verdict = "the key, and tr() returns the key" if shown == key_size else verdict
	elif shown == tr_size:
		verdict = "the translation"
	elif shown == key_size:
		verdict = "the key"
	return [verdict, "shown %s; key %s; tr %s" % [shown, key_size, tr_size]]


func _file(path: String) -> String:
	var text := "file=%s loader=%s" % [FileAccess.file_exists(path), ResourceLoader.exists(path)]
	if not FileAccess.file_exists(path):
		return text
	# Its size moves between engine lines, so only its presence is a fact here.
	if path.get_extension() == "translation":
		return text
	var body := FileAccess.get_file_as_string(path)
	return text + " first line: " + body.get_slice("\n", 0)


func _json(path: String) -> String:
	if not ResourceLoader.exists(path):
		return "no loader for it"
	var resource := load(path)
	if resource is JSON:
		return "JSON resource, data " + JSON.stringify((resource as JSON).data, "", true)
	return "loads as %s" % ("null" if resource == null else resource.get_class())


func _ready() -> void:
	_run.call_deferred()


func _run() -> void:
	_step("game")
	await get_tree().process_frame
	var label := $Label as Label
	var button := $Button as Button
	_row("setting.translations", str(ProjectSettings.get_setting("internationalization/locale/translations", PackedStringArray())))
	_row("setting.test", str(ProjectSettings.get_setting("internationalization/locale/test", "")))
	_row("locale", TranslationServer.get_locale())
	var loaded := Array(TranslationServer.get_loaded_locales())
	loaded.sort()
	_row("loaded_locales", str(loaded))
	for key in KEYS:
		_row("tr." + key, tr(key))
	if OS.get_environment("DIDI_BRIEF") == "1":
		printerr("DIDI_PROBE_DONE")
		get_tree().quit()
		return
	_row("label.text", label.text)
	_row("label.get_text", label.get_text())
	_row("label.auto_translate_mode", str(label.auto_translate_mode))
	_row("label.can_auto_translate", str(label.call("can_auto_translate")) if label.has_method("can_auto_translate") else "not bound")
	_row("label.atr", String(label.call("atr", label.text)) if label.has_method("atr") else "not bound")
	var seen := await _shows(label)
	_row("label.shows", seen[0])
	_row("label.shows_measured", seen[1])
	_row("button.text", button.text)
	seen = await _shows(button)
	_row("button.shows", seen[0])
	_row("button.shows_measured", seen[1])
	_step("set_locale")
	var target := "en" if TranslationServer.get_locale().begins_with("fr") else "fr"
	TranslationServer.set_locale(target)
	await get_tree().process_frame
	_row("set_locale.to", "%s, get_locale() now %s" % [target, TranslationServer.get_locale()])
	_row("set_locale.tr.MENU_START", tr("MENU_START"))
	seen = await _shows(label)
	_row("set_locale.label.shows", seen[0])
	if OS.get_environment("DIDI_DATA_ROWS") != "1":
		printerr("DIDI_PROBE_DONE")
		get_tree().quit()
		return
	_step("data")
	for path in FILES:
		_row("file." + path.get_file(), _file(path))
	_row("json.data.json", _json("res://data.json"))
	printerr("DIDI_PROBE_DONE")
	get_tree().quit()
'''

MISSING = "res://missing.fr.translation"

# (label, what it asks, what locale/translations lists, locale/test).
# "dests" is the .translation files the import declared for strings.csv.
GAME_VARIANTS: list[tuple[str, str, object, str | None]] = [
    ("nothing_registered", "the control: no translations setting at all", None, None),
    ("registered_host_locale", "the imported .translation files registered, no test locale",
     "dests", None),
    ("registered_test_fr", "the same with locale/test fr", "dests", "fr"),
    ("registered_test_fr_CA", "locale/test fr_CA, a region the CSV has no column for",
     "dests", "fr_CA"),
    ("registered_test_de", "locale/test de, a language the CSV has no column for", "dests", "de"),
    ("csv_registered_test_fr", "the .csv itself registered instead, locale/test fr",
     ["res://strings.csv"], "fr"),
    ("missing_registered_test_fr", "a .translation that does not exist registered beside the real ones",
     "dests+missing", "fr"),
]

# What a registration can name besides the imported .translation files. The
# .po is written by hand; the .tres and .res are Translations the engine saves
# itself, each with its own French string so a row says which one loaded.
PO_TEXT = ('msgid ""\nmsgstr ""\n"Content-Type: text/plain; charset=UTF-8\\n"\n"Language: fr\\n"\n\n'
           'msgid "MENU_START"\nmsgstr "Commencer (po)"\n')

def mo_bytes() -> bytes:
    """A GNU .mo catalogue with a header naming fr and one message, which is
    what msgfmt would write for PO_TEXT's twin."""
    entries = sorted({
        b"": b"Content-Type: text/plain; charset=UTF-8\nLanguage: fr\n",
        b"MENU_START": b"Commencer (mo)",
    }.items())
    count = len(entries)
    originals_at = 28
    translations_at = originals_at + count * 8
    strings_at = translations_at + count * 8
    tables, blob = [b"", b""], b""
    for column, index in ((0, 0), (1, 1)):
        for entry in entries:
            text = entry[index]
            tables[column] += struct.pack("<II", len(text), strings_at + len(blob))
            blob += text + b"\0"
    header = struct.pack("<7I", 0x950412DE, 0, count, originals_at, translations_at, 0, 0)
    return header + tables[0] + tables[1] + blob


SAVER_SOURCE = r'''extends SceneTree


func _init() -> void:
	for extension in ["tres", "res"]:
		var translation := Translation.new()
		translation.locale = "fr"
		translation.add_message("MENU_START", "Commencer (%s)" % extension)
		var path: String = "res://saved_translation." + extension
		var err := ResourceSaver.save(translation, path)
		var uid := ResourceLoader.get_resource_uid(path)
		printerr("DIDI_ROW\tregister.saved_%s\terr=%d uid=%s" % [extension, err, "none" if uid == ResourceUID.INVALID_ID else "present"])
		if uid != ResourceUID.INVALID_ID:
			printerr("DIDI_UID\t%s\t%s" % [extension, ResourceUID.id_to_text(uid)])
	var style := StyleBoxFlat.new()
	printerr("DIDI_ROW\tregister.saved_style\terr=%d" % ResourceSaver.save(style, "res://not_a_translation.tres"))
	printerr("DIDI_PROBE_DONE")
	quit()
'''

# (label, what it asks, what locale/translations lists). "dests" is the imported
# .translation files; "uid:tres" is the uid:// of the saved .tres.
REGISTER_VARIANTS: list[tuple[str, str, object]] = [
    ("imported_translations", "the .translation files the CSV import wrote (the control)", "dests"),
    ("po_file", "a gettext .po file written by hand", ["res://strings_fr.po"]),
    ("mo_file", "a gettext .mo file, the compiled form of the same catalogue", ["res://strings_fr.mo"]),
    ("saved_tres", "a Translation the engine saved as .tres", ["res://saved_translation.tres"]),
    ("saved_res", "a Translation the engine saved as .res", ["res://saved_translation.res"]),
    ("uid_of_tres", "the uid:// of that .tres", "uid:tres"),
    ("other_tres", "a .tres the engine saved holding a StyleBoxFlat", ["res://not_a_translation.tres"]),
    ("json_file", "a JSON data file", ["res://data.json"]),
    ("csv_source", "the CSV the translations were imported from", ["res://strings.csv"]),
    ("missing", "a .translation that does not exist", ["res://missing.fr.translation"]),
    ("fr_listed_twice", "the imported .translation files with fr listed twice", "dests+fr"),
]

# (label, what it asks, include_filter as the preset stores it).
EXPORT_VARIANTS = [
    ("default_filters", "export_filter all_resources, include_filter empty", '""'),
    ("data_include_filter", "the same with include_filter *.txt,*.json,*.csv", '"*.txt,*.json,*.csv"'),
]


def project_godot(version: str, name: str, *, main_scene: bool = False,
                  translations: list[str] | None = None, locale_test: str | None = None) -> str:
    feature = ".".join(version.split(".")[:2])
    lines = ["config_version=5", "", "[application]", "", f'config/name="{name}"']
    if main_scene:
        lines.append('run/main_scene="res://main.tscn"')
    lines += [f'config/features=PackedStringArray("{feature}")', ""]
    if translations is not None or locale_test is not None:
        lines += ["[internationalization]", ""]
        if translations is not None:
            quoted = ", ".join(f'"{path}"' for path in translations)
            lines.append(f"locale/translations=PackedStringArray({quoted})")
        if locale_test is not None:
            lines.append(f'locale/test="{locale_test}"')
        lines.append("")
    return "\n".join(lines) + "\n"


def fresh(root: Path) -> None:
    if root.exists():
        shutil.rmtree(root)
    root.mkdir(parents=True)


def write(path: Path, text: str) -> None:
    path.write_text(text, encoding="utf-8", newline="\n")


def declared_dests(sidecar: Path) -> list[str]:
    if not sidecar.exists():
        return []
    deps = dict(sidecar_sections(sidecar.read_text(encoding="utf-8", errors="replace")).get("deps", []))
    return re.findall(r'res://[^"]+', deps.get("dest_files", ""))


def import_case(godot: str, version: str, root: Path, stem: str, body: bytes,
                sidecar: str | None) -> tuple[str, str | None, str]:
    project = root / stem
    fresh(project)
    write(project / "project.godot", project_godot(version, f"Didi localisation probe: {stem}"))
    (project / f"{stem}.csv").write_bytes(body)
    if sidecar is not None:
        write(project / f"{stem}.csv.import", sidecar)
    _, imported = run([godot, "--headless", "--path", str(project), "--import"], timeout=300)
    path = project / f"{stem}.csv.import"
    written = path.read_text(encoding="utf-8", errors="replace") if path.exists() else None
    write(project / "reader.gd", READER_SOURCE)
    env = dict(os.environ, DIDI_LOAD_SOURCE="1" if stem in LOAD_SOURCE else "0")
    _, read = run([godot, "--headless", "--path", str(project), "--script", "res://reader.gd"],
                  env=env, timeout=120)
    return imported, written, read


def importer(godot: str, version: str, work: Path, record: Record, engine: str) -> None:
    root = work / "importer"
    fresh(root)
    with ThreadPoolExecutor(max_workers=6) as pool:
        results = list(pool.map(lambda case: import_case(godot, version, root, case[0], case[2], case[3]),
                                CSV_CASES))
    for (stem, asks, _body, sidecar), (imported, written, read) in zip(CSV_CASES, results):
        prefix = f"import.{stem}."
        record.put(engine, prefix + "asks", asks)
        record.take(engine, prefix, imported)
        if written is None:
            record.put(engine, prefix + "sidecar", "none written")
        else:
            sections = sidecar_sections(written)
            remap = {k: v.strip('"') for k, v in sections.get("remap", [])}
            record.put(engine, prefix + "importer", f"{remap.get('importer')}, type {remap.get('type', '(none)')}"
                       + (f", valid={remap['valid']}" if "valid" in remap else ""))
            dests = declared_dests(root / stem / f"{stem}.csv.import")
            record.put(engine, prefix + "declares", ", ".join(Path(d).name for d in dests) or "nothing")
            if sidecar is not None:
                record.put(engine, prefix + "sidecar_written_first",
                           "kept byte for byte" if written == sidecar else "rewritten by the engine")
            if stem == "tr_basic" or sidecar is not None:
                params = sections.get("params", [])
                record.put(engine, prefix + "params", " | ".join(f"{k}={v}" for k, v in params) or "none")
        if not record.take(engine, prefix, read):
            record.put(engine, prefix + "read_finished", "NO")


def build_game(godot: str, version: str, work: Path, record: Record, engine: str) -> tuple[Path, list[str]]:
    project = work / "game"
    fresh(project)
    write(project / "project.godot", project_godot(version, "Didi localisation probe: game", main_scene=True))
    write(project / "main.tscn", MAIN_TSCN)
    write(project / "main.gd", GAME_SOURCE)
    write(project / "strings.csv", GAME_CSV)
    write(project / "data.json", DATA_JSON)
    write(project / "notes.txt", NOTES_TXT)
    write(project / "data_plain.csv", DATA)
    write(project / "data_keep.csv", DATA)
    write(project / "data_keep.csv.import", KEEP_SIDECAR)
    write(project / "data_skip.csv", DATA)
    write(project / "data_skip.csv.import", SKIP_SIDECAR)
    _, text = run([godot, "--headless", "--path", str(project), "--import"], timeout=300)
    record.take(engine, "game_import.", text)
    dests = declared_dests(project / "strings.csv.import")
    record.put(engine, "game_import.strings.csv.declares", ", ".join(dests) or "nothing")
    return project, dests


def game_variants(godot: str, version: str, project: Path, dests: list[str],
                  record: Record, engine: str) -> None:
    for label, asks, listed, locale_test in GAME_VARIANTS:
        translations = (dests if listed == "dests" else dests + [MISSING] if listed == "dests+missing"
                        else listed)
        write(project / "project.godot", project_godot(
            version, "Didi localisation probe: game", main_scene=True,
            translations=translations, locale_test=locale_test))
        record.put(engine, f"game.{label}.asks", asks)
        env = dict(os.environ, DIDI_DATA_ROWS="1" if listed is None else "0")
        code, text = run([godot, "--headless", "--path", str(project)], env=env, timeout=90)
        if not record.take(engine, f"game.{label}.", text):
            record.put(engine, f"game.{label}.finished", f"NO, rc={code}")


def exports(godot: str, version: str, work: Path, project: Path, dests: list[str],
            record: Record, engine: str) -> None:
    write(project / "project.godot", project_godot(
        version, "Didi localisation probe: game", main_scene=True, translations=dests, locale_test="fr"))
    empty = work / "empty"
    empty.mkdir(exist_ok=True)
    for label, asks, include in EXPORT_VARIANTS:
        write(project / "export_presets.cfg",
              preset_text(0, BASE_KEYS, BASE_OPTIONS, overrides={"include_filter": include}))
        record.put(engine, f"export.{label}.asks", asks + "; translations registered, locale/test fr")
        pck = work / f"{label}.pck"
        pck.unlink(missing_ok=True)
        code, text = run([godot, "--headless", "--path", str(project), "--export-pack", PRESET, str(pck)],
                         timeout=300)
        record.take(engine, f"export.{label}.build.", text)
        record.put(engine, f"export.{label}.pack", "written" if pck.exists() else f"none, rc={code}")
        if not pck.exists():
            continue
        code, text = run([godot, "--headless", "--main-pack", str(pck)], cwd=empty,
                         env=dict(os.environ, DIDI_DATA_ROWS="1"), timeout=90)
        if not record.take(engine, f"export.{label}.", text):
            record.put(engine, f"export.{label}.finished", f"NO, rc={code}")


def registrations(godot: str, version: str, project: Path, dests: list[str],
                  record: Record, engine: str) -> None:
    write(project / "strings_fr.po", PO_TEXT)
    (project / "strings_fr.mo").write_bytes(mo_bytes())
    write(project / "saver.gd", SAVER_SOURCE)
    _, text = run([godot, "--headless", "--path", str(project), "--script", "res://saver.gd"], timeout=120)
    record.take(engine, "", text)
    uids = dict(line.split("\t")[1:3] for line in text.splitlines() if line.startswith("DIDI_UID\t"))
    (project / "saver.gd").unlink()
    # An import pass, so the editor's filesystem and uid cache know the new files.
    run([godot, "--headless", "--path", str(project), "--import"], timeout=240)
    fr = [d for d in dests if d.endswith(".fr.translation")]
    for label, asks, listed in REGISTER_VARIANTS:
        if listed == "dests":
            translations = dests
        elif listed == "dests+fr":
            translations = dests + fr
        elif listed == "uid:tres":
            if "tres" not in uids:
                record.put(engine, f"register.{label}.asks", asks + ": no uid was assigned, not run")
                continue
            translations = [uids["tres"]]
        else:
            translations = listed
        write(project / "project.godot", project_godot(
            version, "Didi localisation probe: game", main_scene=True,
            translations=translations, locale_test="fr"))
        record.put(engine, f"register.{label}.asks", asks)
        code, text = run([godot, "--headless", "--path", str(project)],
                         env=dict(os.environ, DIDI_BRIEF="1"), timeout=90)
        if not record.take(engine, f"register.{label}.", text):
            record.put(engine, f"register.{label}.finished", f"NO, rc={code}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--godot", action="append", default=[],
                        help="a Godot console binary; repeat for each engine line")
    parser.add_argument("--out", help="where the throwaway projects go (default: a temp dir)")
    parser.add_argument("--only", choices=("importer", "game", "export", "register"), action="append",
                        help="run one part; repeat for several")
    args = parser.parse_args()
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    engines = args.godot or ([os.environ["GODOT_BIN"]] if os.environ.get("GODOT_BIN") else [])
    if not engines:
        print("pass --godot (repeatable) or set GODOT_BIN: this probe has no witness without one")
        return 2
    parts = args.only or ["importer", "game", "export", "register"]
    base = Path(args.out) if args.out else Path(tempfile.mkdtemp(prefix="didi_localisation_probe_"))
    print(f"working under {base}")
    record = Record()
    versions = [engine_version(godot) for godot in engines]

    def one(godot: str, version: str) -> None:
        work = base / re.sub(r"[^0-9A-Za-z.]+", "_", version)
        work.mkdir(parents=True, exist_ok=True)
        if "importer" in parts:
            importer(godot, version, work, record, version)
        if "game" in parts or "export" in parts or "register" in parts:
            project, dests = build_game(godot, version, work, record, version)
            if "game" in parts:
                game_variants(godot, version, project, dests, record, version)
            if "export" in parts:
                exports(godot, version, work, project, dests, record, version)
            if "register" in parts:
                registrations(godot, version, project, dests, record, version)
        print(f"  finished {version}  ({godot})", flush=True)

    with ThreadPoolExecutor(max_workers=len(engines)) as pool:
        for future in [pool.submit(one, g, v) for g, v in zip(engines, versions)]:
            future.result()
    record.report(versions)
    return 0


if __name__ == "__main__":
    sys.exit(main())
