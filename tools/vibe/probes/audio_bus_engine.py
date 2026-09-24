"""What adding an audio bus does, asked of the engine rather than of Didi.

#771 found that nothing on the surface adds an audio bus, so
`audio_configure_bus` can only ever configure `Master` and a game cannot have
the `Music` and `SFX` buses every settings menu needs. Before a tool that adds
one is designed, this probe settles the facts it would depend on, on every
engine it is given:

* **The binds.** Every `AudioServer`, `AudioStreamPlayer` and
  `EditorUndoRedoManager` method the tool would call, with its hash on each
  engine, read from `--dump-extension-api`.
* **What AudioServer does with a name.** `add_bus` and `set_bus_name` asked
  about a new bus's default name, a name already in use, `Master`, a name
  differing only in case, the empty name, and names with spaces, quotes,
  slashes, a `&` prefix, control characters and 300 letters. Each is read back
  from the engine, looked up by name, and put through a layout the engine
  saves and loads again.
* **What a send does.** A new bus's default send, then sends to a bus that
  does not exist, to the bus itself, to a later bus, to an earlier one and to
  nothing, each read back; then whether the engine actually routes the sound
  that way. A looping tone plays on one bus and every bus's peak meter is
  read, with silence and a muted intermediate bus as the controls.
* **What a player's `bus` does** when it names a bus before that bus exists,
  after it is added, after it is renamed and after it is removed, and what a
  scene saved in each state records.
* **Where `add_bus` puts a bus** for each position argument, including 0.
* **What an open editor does.** A headless editor runs a probe plugin that
  adds buses the way the extension would call `AudioServer`, and records
  whether the editor's own autosave writes the layout, how long after, to
  which file when `audio/buses/default_bus_layout` is moved, whether the
  editor's Audio panel shows the new bus and its name, what a click on a
  stale name field does to the bus, which refresh corrects the panel without
  changing any bus, and which undo history an
  `EditorUndoRedoManager` action on `AudioServer` lands in. The project is
  then run as a game, which is what finally has to hear the bus.

Everything here is evidence for the `audio_add_bus` amendment in
`docs/SURFACE_AMENDMENTS.md`. Needs no Didi build, no addon and no MCP server.
Every project is a throwaway under `--out` (a temporary directory by default),
and the files the engine and the editor wrote are kept there as evidence.

    python tools/vibe/probes/audio_bus_engine.py --godot C:/Godot/Godot_v4.5.1-stable_win64_console.exe \\
        --godot C:/Godot/Godot_v4.6.2-stable_win64_console.exe --godot C:/Godot/Godot_v4.7.2-stable_win64_console.exe
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

BINDS = {
    "AudioServer": ["get_bus_count", "set_bus_count", "add_bus", "remove_bus", "move_bus",
                    "set_bus_name", "get_bus_name", "get_bus_index", "set_bus_send",
                    "get_bus_send", "set_bus_volume_db", "set_bus_mute",
                    "generate_bus_layout", "set_bus_layout"],
    "AudioStreamPlayer": ["set_bus", "get_bus"],
    "EditorUndoRedoManager": ["create_action", "add_do_method", "add_undo_method",
                              "commit_action", "get_object_history_id",
                              "get_history_undo_redo"],
    "UndoRedo": ["get_current_action_name", "has_undo"],
}

# Shared by the server script and the editor plugin: how a value is printed.
# JSON escapes control characters, so a row is always one line, and the & says
# a value is a StringName rather than a String.
FMT_SOURCE = r'''
func fmt(v) -> String:
	match typeof(v):
		TYPE_STRING_NAME:
			return "&" + fmt(String(v))
		TYPE_STRING:
			if v.length() > 40:
				return "<%d chars%s>" % [v.length(), "" if v == "x".repeat(v.length()) else ", not all x"]
			return JSON.stringify(v)
		TYPE_ARRAY, TYPE_PACKED_STRING_ARRAY:
			var parts := PackedStringArray()
			for x in v:
				parts.append(fmt(x))
			return "[" + ", ".join(parts) + "]"
		TYPE_FLOAT:
			return "%.1f" % v
	return str(v)


func row(label: String, value) -> void:
	printerr("DIDI_ROW\t%s\t%s" % [label, fmt(value)])


# For text that is already formatted rather than a value to show.
func raw(label: String, text: String) -> void:
	printerr("DIDI_ROW\t%s\t%s" % [label, text])


func step(label: String) -> void:
	printerr("DIDI_STEP\t%s" % label)


func names() -> Array:
	var out := []
	for i in AudioServer.get_bus_count():
		out.append(AudioServer.get_bus_name(i))
	return out


func sends() -> String:
	var parts := PackedStringArray()
	for i in AudioServer.get_bus_count():
		parts.append("%s->%s" % [fmt(AudioServer.get_bus_name(i)), fmt(AudioServer.get_bus_send(i))])
	return ", ".join(parts)
'''

SERVER_SOURCE = r'''extends SceneTree

var layout_changed := 0
var renamed := PackedStringArray()

const ODD := {
	"spaces_inside": "Music And Voice",
	"leading_space": " Music",
	"trailing_space": "Music ",
	"whitespace_only": "   ",
	"double_quote": "Say \"hi\"",
	"backslash": "back\\slash",
	"slash": "a/b",
	"colon": "a:b",
	"equals": "a=b",
	"brackets": "[bus]",
	"ampersand_literal": "&\"x\"",
	"unicode": "Ünïcødé 音",
	"tab": "tab\there",
	"newline": "new\nline",
	"digits": "123",
	"long_300": "",
}
''' + FMT_SOURCE + r'''

func _on_layout_changed() -> void:
	layout_changed += 1


func _on_renamed(index: int, old_name: StringName, new_name: StringName) -> void:
	renamed.append("%d:%s->%s" % [index, fmt(old_name), fmt(new_name)])


func reset(buses: Array) -> void:
	AudioServer.set_bus_count(1)
	AudioServer.set_bus_name(0, "Master")
	AudioServer.set_bus_send(0, "")
	AudioServer.set_bus_volume_db(0, 0.0)
	AudioServer.set_bus_mute(0, false)
	for entry in buses:
		AudioServer.add_bus(-1)
		var i := AudioServer.get_bus_count() - 1
		AudioServer.set_bus_name(i, entry[0])
		AudioServer.set_bus_send(i, entry[1])


func tone() -> AudioStreamWAV:
	var wav := AudioStreamWAV.new()
	wav.format = AudioStreamWAV.FORMAT_16_BITS
	wav.mix_rate = 22050
	wav.stereo = false
	var frames := 22050
	var data := PackedByteArray()
	data.resize(frames * 2)
	for i in frames:
		data.encode_s16(i * 2, int(sin(TAU * 441.0 * i / 22050.0) * 16000.0))
	wav.data = data
	wav.loop_mode = AudioStreamWAV.LOOP_FORWARD
	wav.loop_begin = 0
	wav.loop_end = frames
	return wav


func peaks() -> String:
	# The loudest block seen over a short window, per bus. A peak meter holds
	# only the last mixed block, so one read can land between blocks.
	var loudest := []
	for i in AudioServer.get_bus_count():
		loudest.append(-INF)
	for n in 8:
		await create_timer(0.05).timeout
		for i in AudioServer.get_bus_count():
			loudest[i] = max(loudest[i], AudioServer.get_bus_peak_volume_left_db(i, 0))
	var parts := PackedStringArray()
	for i in AudioServer.get_bus_count():
		parts.append("%s %s" % [AudioServer.get_bus_name(i), "silent" if loudest[i] < -100.0 else "%.0f dB" % loudest[i]])
	return ", ".join(parts)


func _initialize() -> void:
	_run()


func _run() -> void:
	AudioServer.connect("bus_layout_changed", _on_layout_changed)
	var has_renamed := AudioServer.has_signal("bus_renamed")
	row("signal.bus_renamed_exists", has_renamed)
	if has_renamed:
		AudioServer.connect("bus_renamed", _on_renamed)

	step("new bus")
	row("start.buses", names())
	row("start.Master_send", AudioServer.get_bus_send(0))
	var before := layout_changed
	AudioServer.add_bus(-1)
	row("add.count_after", AudioServer.get_bus_count())
	row("add.default_name", AudioServer.get_bus_name(1))
	row("add.default_send", AudioServer.get_bus_send(1))
	row("add.default_volume_db", AudioServer.get_bus_volume_db(1))
	row("add.default_mute", AudioServer.is_bus_mute(1))
	row("add.bus_layout_changed_emitted", layout_changed - before)
	AudioServer.add_bus(-1)
	row("add.second_default_name", AudioServer.get_bus_name(2))
	row("add.return_type_of_get_bus_name", type_string(typeof(AudioServer.get_bus_name(1))))
	row("add.return_type_of_get_bus_send", type_string(typeof(AudioServer.get_bus_send(1))))

	step("names")
	before = layout_changed
	renamed.clear()
	AudioServer.set_bus_name(1, "Music")
	row("name.set_Music", AudioServer.get_bus_name(1))
	row("name.bus_renamed_emitted", renamed)
	row("name.bus_layout_changed_emitted", layout_changed - before)
	row("name.index_of_Music", AudioServer.get_bus_index("Music"))
	row("name.index_of_music_lowercase", AudioServer.get_bus_index("music"))
	row("name.index_of_nothing", AudioServer.get_bus_index("Nothing"))
	renamed.clear()
	AudioServer.set_bus_name(2, "Music")
	row("name.duplicate_Music_became", AudioServer.get_bus_name(2))
	row("name.duplicate_bus_renamed_emitted", renamed)
	row("name.duplicate_index_of_Music", AudioServer.get_bus_index("Music"))
	AudioServer.set_bus_name(2, "Master")
	row("name.Master_on_bus_2_became", AudioServer.get_bus_name(2))
	AudioServer.set_bus_name(2, "music")
	row("name.lowercase_music_became", AudioServer.get_bus_name(2))
	AudioServer.set_bus_name(2, "")
	row("name.empty_became", AudioServer.get_bus_name(2))
	AudioServer.set_bus_name(2, "Music 2")
	AudioServer.set_bus_name(2, "Music")
	row("name.Music_when_Music_2_is_taken_by_itself", AudioServer.get_bus_name(2))
	AudioServer.set_bus_name(0, "Main")
	row("name.bus_0_renamed_Main", AudioServer.get_bus_name(0))
	row("name.index_of_Master_after", AudioServer.get_bus_index("Master"))
	AudioServer.set_bus_name(0, "Master")

	step("odd names")
	AudioServer.set_bus_count(1)
	var keys := ODD.keys()
	var asked := {}
	for key in keys:
		var wanted: String = "x".repeat(300) if key == "long_300" else ODD[key]
		asked[key] = wanted
		AudioServer.add_bus(-1)
		var i := AudioServer.get_bus_count() - 1
		AudioServer.set_bus_name(i, wanted)
		var got := AudioServer.get_bus_name(i)
		raw("odd.%s.kept" % key, "%s%s" % [fmt(got), "" if got == wanted else "  (asked " + fmt(wanted) + ")"])
		row("odd.%s.found_by_name" % key, AudioServer.get_bus_index(wanted) == i)
	var saved := ResourceSaver.save(AudioServer.generate_bus_layout(), "res://odd_layout.tres")
	row("odd.save_result", saved)
	AudioServer.set_bus_count(1)
	var loaded = ResourceLoader.load("res://odd_layout.tres", "", ResourceLoader.CACHE_MODE_IGNORE)
	raw("odd.reload_class", loaded.get_class() if loaded else "null")
	if loaded:
		AudioServer.set_bus_layout(loaded)
		for k in keys.size():
			var got := AudioServer.get_bus_name(k + 1) if k + 1 < AudioServer.get_bus_count() else "<missing>"
			raw("odd.%s.after_save_and_load" % keys[k], "same" if got == asked[keys[k]] else "CHANGED to " + fmt(got))

	step("sends")
	reset([["Music", "Master"], ["SFX", "Master"]])
	AudioServer.add_bus(-1)
	row("send.default_of_bus_added_after_others", AudioServer.get_bus_send(3))
	AudioServer.set_bus_count(3)
	AudioServer.set_bus_send(1, "Nope")
	row("send.to_missing_bus_reads", AudioServer.get_bus_send(1))
	AudioServer.set_bus_send(1, "Music")
	row("send.to_itself_reads", AudioServer.get_bus_send(1))
	AudioServer.set_bus_send(1, "SFX")
	row("send.to_later_bus_reads", AudioServer.get_bus_send(1))
	AudioServer.set_bus_send(2, "Music")
	row("send.to_earlier_bus_reads", AudioServer.get_bus_send(2))
	AudioServer.set_bus_send(1, "")
	row("send.to_empty_reads", AudioServer.get_bus_send(1))
	AudioServer.set_bus_send(0, "Music")
	row("send.of_Master_reads", AudioServer.get_bus_send(0))
	reset([["Music", "Master"], ["SFX", "Music"]])
	AudioServer.set_bus_name(1, "Tunes")
	raw("send.after_target_renamed", sends())
	AudioServer.remove_bus(1)
	raw("send.after_target_removed", sends())

	step("routing")
	var player := AudioStreamPlayer.new()
	root.add_child(player)
	player.stream = tone()
	var cases := [
		["silence_control", [["Music", "Master"], ["SFX", "Master"]], "", false],
		["tone_on_Master", [["Music", "Master"], ["SFX", "Master"]], "Master", false],
		["Music_sends_Master", [["Music", "Master"], ["SFX", "Master"]], "Music", false],
		["SFX_sends_earlier_Music", [["Music", "Master"], ["SFX", "Music"]], "SFX", false],
		["SFX_sends_earlier_Music_muted", [["Music", "Master"], ["SFX", "Music"]], "SFX", true],
		["Music_sends_later_SFX", [["Music", "SFX"], ["SFX", "Master"]], "Music", false],
		["Music_sends_missing_bus", [["Music", "Nope"], ["SFX", "Master"]], "Music", false],
		["Music_sends_itself", [["Music", "Music"], ["SFX", "Master"]], "Music", false],
		["Music_sends_nothing", [["Music", ""], ["SFX", "Master"]], "Music", false],
	]
	for c in cases:
		reset(c[1])
		if c[3]:
			AudioServer.set_bus_mute(1, true)
		if c[2] != "":
			player.bus = c[2]
			player.play()
		raw("route." + c[0], await peaks())
		player.stop()
		await create_timer(0.3).timeout
	player.queue_free()

	step("player bus")
	reset([])
	var p := AudioStreamPlayer.new()
	p.bus = "Later"
	row("player.bus_named_before_it_exists", p.bus)
	var scene := PackedScene.new()
	scene.pack(p)
	ResourceSaver.save(scene, "res://player_before_bus.tscn")
	AudioServer.add_bus(-1)
	AudioServer.set_bus_name(1, "Later")
	row("player.same_player_after_bus_added", p.bus)
	scene = PackedScene.new()
	scene.pack(p)
	ResourceSaver.save(scene, "res://player_after_bus.tscn")
	AudioServer.set_bus_name(1, "Renamed")
	row("player.after_bus_renamed", p.bus)
	AudioServer.set_bus_name(1, "Later")
	row("player.after_bus_named_back", p.bus)
	AudioServer.remove_bus(1)
	row("player.after_bus_removed", p.bus)
	p.free()

	step("positions")
	reset([["A", "Master"], ["B", "Master"]])
	AudioServer.add_bus(1)
	row("position.add_bus_1", names())
	reset([["A", "Master"], ["B", "Master"]])
	AudioServer.add_bus(0)
	row("position.add_bus_0", names())
	raw("position.add_bus_0_sends", sends())
	reset([["A", "Master"], ["B", "Master"]])
	AudioServer.add_bus(3)
	row("position.add_bus_3_equals_count", names())
	reset([["A", "Master"], ["B", "Master"]])
	AudioServer.add_bus(99)
	row("position.add_bus_99", names())
	reset([["A", "Master"], ["B", "Master"]])
	AudioServer.add_bus(-5)
	row("position.add_bus_minus_5", names())

	step("layout file")
	reset([["Music", "Master"], ["SFX", "Music"], ["Voice", "Master"]])
	AudioServer.set_bus_volume_db(1, -6.0)
	row("layout.save_result", ResourceSaver.save(AudioServer.generate_bus_layout(), "res://saved_layout.tres"))

	step("done")
	printerr("DIDI_PROBE_DONE")
	quit()
'''

PLUGIN_SOURCE = r'''@tool
extends EditorPlugin

const DEFAULT := "res://default_bus_layout.tres"
const CUSTOM := "res://custom_bus_layout.tres"
var out_dir := ""
var variant := ""
''' + FMT_SOURCE + r'''

func _enter_tree() -> void:
	out_dir = OS.get_environment("DIDI_PROBE_OUT")
	variant = OS.get_environment("DIDI_PROBE_VARIANT")
	_run.call_deferred()


func _wait(seconds: float) -> void:
	await get_tree().create_timer(seconds).timeout


func _bytes(path: String) -> PackedByteArray:
	return FileAccess.get_file_as_bytes(path) if FileAccess.file_exists(path) else PackedByteArray()


func _files() -> String:
	var parts := PackedStringArray()
	for path in [DEFAULT, CUSTOM]:
		parts.append("%s %s" % [path.get_file(), ("%d bytes" % _bytes(path).size()) if FileAccess.file_exists(path) else "absent"])
	return ", ".join(parts)


func _snap(label: String) -> void:
	for path in [DEFAULT, CUSTOM]:
		if FileAccess.file_exists(path):
			var file := FileAccess.open(out_dir.path_join(label + "__" + path.get_file()), FileAccess.WRITE)
			file.store_buffer(_bytes(path))
			file.close()
	raw("files." + label, _files())


# The names on the editor's own Audio panel: one EditorAudioBus strip per bus,
# the first LineEdit in each being the name field.
func _strips() -> String:
	var panels := EditorInterface.get_base_control().find_children("*", "EditorAudioBuses", true, false)
	if panels.is_empty():
		return "no EditorAudioBuses panel"
	var shown := []
	for strip in panels[0].find_children("*", "EditorAudioBus", true, false):
		var edits: Array = strip.find_children("*", "LineEdit", true, false)
		shown.append(edits[0].text if not edits.is_empty() else "?")
	return fmt(shown)


# Every bus's name, send, volume and effect count, to tell a refresh that
# changed nothing from one that changed something.
func _state() -> String:
	var parts := PackedStringArray()
	for i in AudioServer.get_bus_count():
		parts.append("%s->%s %.1f dB fx=%d" % [fmt(AudioServer.get_bus_name(i)), fmt(AudioServer.get_bus_send(i)),
			AudioServer.get_bus_volume_db(i), AudioServer.get_bus_effect_count(i)])
	return ", ".join(parts)


# What clicking into a strip's name field and away again does: the field's
# focus_exited, which the strip answers by renaming the bus to the field's
# text when the two differ. Returns the bus's name afterwards.
func _focus_exit_on(index: int) -> String:
	var panels := EditorInterface.get_base_control().find_children("*", "EditorAudioBuses", true, false)
	if panels.is_empty():
		return "no panel"
	var strips: Array = panels[0].find_children("*", "EditorAudioBus", true, false)
	if index >= strips.size():
		return "no strip %d" % index
	var edits: Array = strips[index].find_children("*", "LineEdit", true, false)
	if edits.is_empty():
		return "no name field"
	edits[0].focus_exited.emit()
	await _wait(0.2)
	return fmt(AudioServer.get_bus_name(index))


# Milliseconds until either layout file changes, or -1 after the limit.
func _await_write(limit: float = 6.0) -> int:
	var before := [_bytes(DEFAULT), _bytes(CUSTOM)]
	var waited := 0.0
	while waited < limit:
		await _wait(0.1)
		waited += 0.1
		if _bytes(DEFAULT) != before[0] or _bytes(CUSTOM) != before[1]:
			return int(round(waited * 1000.0))
	return -1


func _add(bus_name: String, send: String) -> void:
	AudioServer.add_bus(-1)
	var i := AudioServer.get_bus_count() - 1
	AudioServer.set_bus_name(i, bus_name)
	AudioServer.set_bus_send(i, send)


func _run() -> void:
	await _wait(3.0)
	row("layout_setting", ProjectSettings.get_setting("audio/buses/default_bus_layout"))
	raw("opened.buses", sends())
	raw("opened.panel", _strips())
	_snap("0_opened")

	step("add Music the way the extension would")
	_add("Music", "Master")
	raw("add_music.buses", sends())
	row("add_music.written_after_ms", await _await_write())
	await _wait(0.5)
	raw("add_music.panel", _strips())
	_snap("1_after_add_music")
	if variant == "settings_saved":
		# What a ProjectSettings.save() does to the layout setting once the
		# layout file exists. The live harness found a 4.7.2 project.godot
		# holding it as a uid:// after its project-setting requests.
		step("save the project settings once the layout file exists")
		row("saved.setting_before_save", ProjectSettings.get_setting("audio/buses/default_bus_layout"))
		row("saved.save_result", ProjectSettings.save())
		row("saved.setting_after_save", ProjectSettings.get_setting("audio/buses/default_bus_layout"))
		var kept := PackedStringArray()
		for line in FileAccess.get_file_as_string("res://project.godot").split("\n"):
			if line == "[audio]" or line.contains("default_bus_layout"):
				kept.append(line)
		row("saved.project_godot_lines", kept)
		_finish()
		return
	if variant != "fresh":
		_finish()
		return

	step("the panel after a rename, and what a click on it does")
	var music := AudioServer.get_bus_index("Music")
	AudioServer.add_bus_effect(music, AudioEffectReverb.new())
	AudioServer.set_bus_volume_db(music, -3.0)
	await _wait(0.3)
	var state := _state()
	raw("stale.state", state)
	raw("stale.panel", _strips())
	raw("stale.click_on_name_leaves_bus_named", await _focus_exit_on(music))
	AudioServer.set_bus_name(music, "Music")
	await _wait(0.3)
	raw("stale.panel_after_name_put_back", _strips())
	AudioServer.emit_signal("bus_layout_changed")
	await _wait(0.3)
	raw("refresh_signal.panel", _strips())
	raw("refresh_signal.state", "unchanged" if _state() == state else "CHANGED: " + _state())
	raw("refresh_signal.click_on_name_leaves_bus_named", await _focus_exit_on(music))
	AudioServer.set_bus_name(music, "Music")
	await _wait(0.3)
	raw("refresh_signal.panel_after_name_put_back", _strips())
	AudioServer.set_bus_count(AudioServer.get_bus_count())
	await _wait(0.3)
	raw("refresh_count.panel", _strips())
	raw("refresh_count.state", "unchanged" if _state() == state else "CHANGED: " + _state())
	raw("refresh_count.click_on_name_leaves_bus_named", await _focus_exit_on(music))
	AudioServer.set_bus_name(music, "Music")
	AudioServer.remove_bus_effect(music, 0)
	AudioServer.set_bus_volume_db(music, 0.0)
	AudioServer.emit_signal("bus_layout_changed")
	await _wait(1.5)

	step("add SFX sending to Music, then set its volume")
	_add("SFX", "Music")
	AudioServer.set_bus_volume_db(AudioServer.get_bus_index("SFX"), -6.0)
	row("add_sfx.written_after_ms", await _await_write())
	await _wait(0.5)
	raw("add_sfx.panel", _strips())
	_snap("2_after_add_sfx")

	step("add through EditorUndoRedoManager")
	var ur := get_undo_redo()
	var history := ur.get_object_history_id(AudioServer)
	row("undo.history_id_of_AudioServer", history)
	var count := AudioServer.get_bus_count()
	ur.create_action("Didi probe: add audio bus")
	ur.add_do_method(AudioServer, "add_bus", -1)
	ur.add_do_method(AudioServer, "set_bus_name", count, "Voice")
	ur.add_undo_method(AudioServer, "remove_bus", count)
	ur.commit_action()
	row("undo.buses_after_commit", names())
	var undo_redo := ur.get_history_undo_redo(history)
	row("undo.current_action_name", undo_redo.get_current_action_name())
	row("undo.has_undo", undo_redo.has_undo())
	row("undo.written_after_ms", await _await_write())
	_snap("3_after_undoable_add")
	_finish()


func _finish() -> void:
	step("done")
	printerr("DIDI_PROBE_DONE")
	get_tree().quit()
'''

GAME_SOURCE = r'''extends Node

func _ready() -> void:
	var parts := PackedStringArray()
	for i in AudioServer.get_bus_count():
		parts.append("%s->%s %.1f dB" % [JSON.stringify(AudioServer.get_bus_name(i)),
			JSON.stringify(String(AudioServer.get_bus_send(i))), AudioServer.get_bus_volume_db(i)])
	printerr("DIDI_ROW\tgame.buses\t" + ", ".join(parts))
	get_tree().quit()
'''


def engine_version(godot: str) -> str:
    out = subprocess.run([godot, "--version"], capture_output=True, text=True, timeout=60)
    return out.stdout.strip().splitlines()[-1] if out.stdout.strip() else "?"


def run(cmd: list[str], cwd: Path | None = None, env: dict | None = None,
        timeout: int = 240) -> tuple[int | None, str]:
    # stdout goes nowhere useful: every probe line and every engine ERROR is on
    # stderr, so reading that one stream keeps them in the order they happened.
    try:
        out = subprocess.run(cmd, cwd=cwd, env=env, stdout=subprocess.DEVNULL,
                             stderr=subprocess.PIPE, text=True, encoding="utf-8",
                             errors="replace", timeout=timeout)
        return out.returncode, out.stderr
    except subprocess.TimeoutExpired as exc:
        text = exc.stderr or b""
        if isinstance(text, bytes):
            text = text.decode("utf-8", "replace")
        return None, text


class Record:
    """Rows and engine lines per engine, keyed by label.

    The engines run at once, so each keeps its own order of labels and the
    report follows the first engine's, adding any label only a later one has.
    """

    def __init__(self) -> None:
        self.rows: dict[str, dict[str, str]] = {}
        self.order: dict[str, list[str]] = {}
        self.lines: dict[str, dict[str, list[str]]] = {}
        self.steps: dict[str, list[str]] = {}

    def put(self, engine: str, label: str, value: str) -> None:
        self.rows.setdefault(label, {})[engine] = value
        self.order.setdefault(engine, []).append(label)

    def take(self, engine: str, prefix: str, text: str) -> bool:
        step = f"{prefix}(startup)"
        lines = text.splitlines()
        done = False
        for i, line in enumerate(lines):
            stripped = line.strip()
            if stripped.startswith("DIDI_STEP\t"):
                step = prefix + stripped.split("\t", 1)[1]
            elif stripped.startswith("DIDI_ROW\t"):
                _, label, value = (stripped.split("\t", 2) + [""])[:3]
                self.put(engine, prefix + label, value)
            elif stripped == "DIDI_PROBE_DONE":
                done = True
            elif re.match(r"^(ERROR|WARNING|SCRIPT ERROR|USER ERROR|USER WARNING):", stripped):
                where = ""
                if i + 1 < len(lines) and lines[i + 1].strip().startswith("at:"):
                    where = "  [" + lines[i + 1].strip()[3:].strip() + "]"
                self.lines.setdefault(step, {}).setdefault(engine, []).append(stripped + where)
                self.steps.setdefault(engine, []).append(step)
        return done

    @staticmethod
    def ordered(per_engine: dict[str, list[str]], engines: list[str]) -> list[str]:
        out: list[str] = []
        for engine in engines:
            for key in per_engine.get(engine, []):
                if key not in out:
                    out.append(key)
        return out

    def report(self, engines: list[str]) -> None:
        print("\n=== rows (one value when every engine agrees)")
        for label in self.ordered(self.order, engines):
            values = self.rows[label]
            seen = [values.get(e, "(no row)") for e in engines]
            if len(set(seen)) == 1:
                print(f"  {label:58} {seen[0]}")
            else:
                print(f"  {label:58} DIFFERS")
                for engine, value in zip(engines, seen):
                    print(f"      {engine:40} {value}")
        print("\n=== engine ERROR and WARNING lines, by the step that was running")
        if not self.lines:
            print("  none")
        for step in self.ordered(self.steps, engines):
            per_engine = self.lines[step]
            print(f"  {step}")
            for engine in engines:
                for line in per_engine.get(engine, []):
                    print(f"      {engine:40} {line}")


def write_project(root: Path, version: str, *, main_scene: bool = False, plugin: bool = False,
                  layout_setting: str | None = None) -> None:
    if root.exists():
        shutil.rmtree(root)
    root.mkdir(parents=True)
    feature = ".".join(version.split(".")[:2])
    project = ["config_version=5", "", "[application]", "",
               'config/name="Didi audio bus probe"',
               f'config/features=PackedStringArray("{feature}")']
    if main_scene:
        project.append('run/main_scene="res://main.tscn"')
    project.append("")
    if layout_setting:
        project += ["[audio]", "", f'buses/default_bus_layout="{layout_setting}"', ""]
    if plugin:
        project += ["[editor_plugins]", "",
                    'enabled=PackedStringArray("res://addons/didi_audio_probe/plugin.cfg")', ""]
    (root / "project.godot").write_text("\n".join(project), encoding="utf-8", newline="\n")
    if main_scene:
        (root / "main.gd").write_text(GAME_SOURCE, encoding="utf-8", newline="\n")
        (root / "main.tscn").write_text(
            '[gd_scene load_steps=2 format=3]\n\n'
            '[ext_resource type="Script" path="res://main.gd" id="1"]\n\n'
            '[node name="Main" type="Node"]\nscript = ExtResource("1")\n',
            encoding="utf-8", newline="\n")
    if plugin:
        addon = root / "addons" / "didi_audio_probe"
        addon.mkdir(parents=True)
        (addon / "plugin.cfg").write_text(
            '[plugin]\n\nname="Didi audio bus probe"\ndescription=""\nauthor=""\n'
            'version="1"\nscript="plugin.gd"\n', encoding="utf-8", newline="\n")
        (addon / "plugin.gd").write_text(PLUGIN_SOURCE, encoding="utf-8", newline="\n")


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
            if method and method.get("hash_compatibility"):
                value += f"  compat {method['hash_compatibility']}"
            record.put(engine, f"bind.{cls}.{name}", value)


def server(godot: str, version: str, work: Path, record: Record, engine: str) -> None:
    project = work / "server_project"
    write_project(project, version)
    (project / "probe_server.gd").write_text(SERVER_SOURCE, encoding="utf-8", newline="\n")
    run([godot, "--headless", "--path", str(project), "--import"])
    code, text = run([godot, "--headless", "--path", str(project), "--script", "res://probe_server.gd"],
                     timeout=180)
    done = record.take(engine, "server.", text)
    record.put(engine, "server.(run)", f"rc={code} finished={'yes' if done else 'NO'}")
    for name in ("saved_layout.tres", "odd_layout.tres", "player_before_bus.tscn", "player_after_bus.tscn"):
        path = project / name
        body = path.read_text(encoding="utf-8", errors="replace") if path.exists() else "(not written)\n"
        # Only the lines that carry a bus, and a header so a missing one shows.
        keep = [line for line in body.splitlines() if "bus" in line or line.startswith("[")]
        record.put(engine, f"file.{name}", " | ".join(keep))


def editor(godot: str, version: str, work: Path, record: Record, engine: str) -> None:
    variants = [
        ("fresh", None, None),
        ("relocated", "res://custom_bus_layout.tres", None),
        ("existing", None, "saved_layout.tres"),
        ("settings_saved", None, None),
    ]
    for variant, setting, seed in variants:
        project = work / f"editor_{variant}"
        write_project(project, version, main_scene=True, plugin=True, layout_setting=setting)
        if seed:
            source = work / "server_project" / seed
            if not source.exists():
                record.put(engine, f"{variant}.(seed)", "run the server part first")
                continue
            shutil.copyfile(source, project / "default_bus_layout.tres")
        snaps = work / f"editor_{variant}_snapshots"
        if snaps.exists():
            shutil.rmtree(snaps)
        snaps.mkdir()
        run([godot, "--headless", "--path", str(project), "--import"])
        env = dict(os.environ, DIDI_PROBE_OUT=str(snaps), DIDI_PROBE_VARIANT=variant)
        code, text = run([godot, "--headless", "--editor", "--path", str(project)], env=env, timeout=180)
        done = record.take(engine, f"{variant}.", text)
        record.put(engine, f"{variant}.(run)", f"rc={code} finished={'yes' if done else 'NO'}")
        for name in ("default_bus_layout.tres", "custom_bus_layout.tres"):
            path = project / name
            if path.exists():
                keep = [line for line in path.read_text(encoding="utf-8", errors="replace").splitlines()
                        if line.startswith("bus/")]
                record.put(engine, f"{variant}.file_after_quit.{name}", " | ".join(keep) or "(no bus lines)")
            else:
                record.put(engine, f"{variant}.file_after_quit.{name}", "absent")
        code, text = run([godot, "--headless", "--path", str(project)], timeout=60)
        record.take(engine, f"{variant}.", text)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--godot", action="append", default=[],
                        help="a Godot console binary; repeat for each engine line")
    parser.add_argument("--out", help="where the throwaway projects go (default: a temp dir)")
    parser.add_argument("--only", choices=("binds", "server", "editor"), action="append",
                        help="run one part; repeat for several (editor's third variant needs server)")
    args = parser.parse_args()
    # Bus names are the engine's, not ASCII, and a Windows console is cp1252.
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    engines = args.godot or ([os.environ["GODOT_BIN"]] if os.environ.get("GODOT_BIN") else [])
    if not engines:
        print("pass --godot (repeatable) or set GODOT_BIN: this probe has no witness without one")
        return 2
    parts = args.only or ["binds", "server", "editor"]
    base = Path(args.out) if args.out else Path(tempfile.mkdtemp(prefix="didi_audio_probe_"))
    print(f"working under {base}")
    record = Record()
    versions = [engine_version(godot) for godot in engines]

    def one(godot: str, version: str) -> None:
        # Each engine has its own directory, editor settings file and user
        # data, so the lines run side by side.
        work = base / re.sub(r"[^0-9A-Za-z.]+", "_", version)
        work.mkdir(parents=True, exist_ok=True)
        if "binds" in parts:
            binds(godot, work, record, version)
        if "server" in parts:
            server(godot, version, work, record, version)
        if "editor" in parts:
            editor(godot, version, work, record, version)
        print(f"  finished {version}  ({godot})", flush=True)

    with ThreadPoolExecutor(max_workers=len(engines)) as pool:
        for future in [pool.submit(one, g, v) for g, v in zip(engines, versions)]:
            future.result()
    record.report(versions)
    return 0


if __name__ == "__main__":
    sys.exit(main())
