@tool
extends EditorPlugin

## Didi's editor presence.
##
## The console is registered as a main screen -- a tab beside 2D, 3D, Script and
## Asset Store -- for two reasons. It is the one place Godot draws a plugin's own
## icon, so Didi is recognisable in the editor rather than being a word in a row
## of words; and it is the only surface with room to answer the questions the
## console exists to answer without abbreviating them into a strip. The bottom
## panel was tried first and its buttons render text only: an icon set on one is
## simply not drawn, including an icon taken from the editor's own theme.
##
## Everything the console knows how to do lives in the scripts beside this one,
## so the plugin is only ever creating one control and handing it to the editor.

const Brand := preload("res://addons/didi/didi_brand.gd")
const Console := preload("res://addons/didi/didi_console.gd")
const Settings := preload("res://addons/didi/didi_settings.gd")

## Main screen tabs draw their icon at the editor's small icon size. The brand
## ships a heavier cut below 20 px for exactly this, and the mark is rasterised
## at that size rather than scaled down from a larger one.
const _TAB_ICON_HEIGHT := 16.0

var _console: Control


func _enter_tree() -> void:
	Settings.ensure_registered()
	_console = Console.new()
	_console.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_console.size_flags_vertical = Control.SIZE_EXPAND_FILL
	EditorInterface.get_editor_main_screen().add_child(_console)
	# Godot shows the main screen the editor was last on, so a plugin that made
	# itself visible here would steal the viewport from someone who was in the
	# middle of something. The tab is enough.
	_console.hide()
	print("[Didi] Didi Native MCP Editor Plugin active.")


func _exit_tree() -> void:
	if _console != null:
		_console.queue_free()
		_console = null
	print("[Didi] Didi Native MCP Editor Plugin deactivated.")


func _has_main_screen() -> bool:
	return true


func _get_plugin_name() -> String:
	return "Didi"


func _get_plugin_icon() -> Texture2D:
	return Brand.mark(_TAB_ICON_HEIGHT, Brand.editor_ink(EditorInterface.get_base_control()))


func _make_visible(is_visible: bool) -> void:
	if _console != null:
		_console.visible = is_visible
