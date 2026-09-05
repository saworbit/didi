@tool
extends VBoxContainer

## The Didi console: one place in the editor that answers whether an assistant
## can reach this project, what it would be reaching, and how to change it.
##
## The Dashboard is the whole design in one screen. Six cards, each with a red,
## amber or green light, each saying in one line what it found and what to do
## about it. A light on its own is decoration; the fact underneath it -- the
## path, the pid, the session -- is what someone actually acts on, so every card
## carries both and neither is hidden behind a click.
##
## The panel is built in code rather than from a scene on purpose. An addon that
## carries a .tscn carries the import state of that .tscn into every project it
## is copied into; this one is a handful of scripts and three SVGs, and it draws
## correctly the first time the editor sees it.

const Brand := preload("res://addons/didi/didi_brand.gd")
const ClientConfig := preload("res://addons/didi/didi_client_config.gd")
const Diagnostics := preload("res://addons/didi/didi_diagnostics.gd")
const Log := preload("res://addons/didi/didi_log.gd")
const Sessions := preload("res://addons/didi/didi_session.gd")
const Settings := preload("res://addons/didi/didi_settings.gd")

## What the bridge is doing, in the order of how bad it is.
enum Bridge { LIVE, LOADED_WITHOUT_SESSION, NOT_LOADED }

## The three lights. Everything the console reports resolves to one of them.
enum Lamp { GREEN, AMBER, RED }

const _HEADER_SIGNATURE_HEIGHT := 20.0
const _DOT := "●"
const _CONTENT_MEASURE := 1000.0
const _PEER_LIMIT := 4
const _LOG_LINES := 400
const _SETTINGS_TAB := 3

var _signature: TextureRect
var _state_dot: Label
var _state_line: Label
var _version_line: Label
var _refresh_button: Button
var _toast: Label
var _toast_timer: Timer

var _cards := {}
var _card_styles := {}
var _detail_rows := {}
var _detail_box: VBoxContainer
var _peers: VBoxContainer
var _peers_heading: Label
var _bridge_switch: CheckButton
var _auto_switch: CheckButton
var _technical_switch: CheckButton
var _bridge_result: Label

var _executable_field: LineEdit
var _executable_state: Label
var _client_picker: OptionButton
var _destination_line: Label
var _config_view: TextEdit
var _write_button: Button
var _connect_notice: Label

var _log_source: OptionButton
var _log_view: RichTextLabel
var _log_follow: CheckButton
var _log_levels := {}
var _log_filter: LineEdit
var _log_source_note: Label

var _refresh_seconds: SpinBox
var _log_level: OptionButton
var _skip_confirmations: CheckBox
var _skip_warning: Label
var _verify_switch: CheckButton
var _pipe_name: LineEdit

var _check_rows: VBoxContainer
var _report := ""
var _tabs: TabContainer
var _diagnostics_tab := -1
var _log_tab := -1

var _timer: Timer
var _confirm: ConfirmationDialog
var _confirm_bridge: ConfirmationDialog
var _failure: AcceptDialog
var _file_dialog: EditorFileDialog

var _bridge := Bridge.NOT_LOADED
var _own: Sessions.Session = null
var _sessions: Array = []
var _verified_binary := ""
var _last_bridge := -1
var _last_session_id := ""
var _last_peer_count := -1
var _suppress_writes := false


func _init() -> void:
	name = "Didi"
	add_theme_constant_override("separation", 0)
	_build_header()
	_build_tabs()
	_build_dialogs()

	_timer = Timer.new()
	_timer.one_shot = false
	add_child(_timer)
	_timer.timeout.connect(_refresh)

	visibility_changed.connect(_on_visibility_changed)


func _ready() -> void:
	_apply_theme()
	_reload_settings_controls()
	_refresh()
	_on_visibility_changed()


func _notification(what: int) -> void:
	if what == NOTIFICATION_THEME_CHANGED and is_inside_tree():
		_apply_theme()


# --- header ------------------------------------------------------------------

func _build_header() -> void:
	var header := HBoxContainer.new()
	header.add_theme_constant_override("separation", 10)
	var margin := MarginContainer.new()
	for side in ["margin_left", "margin_right", "margin_top", "margin_bottom"]:
		margin.add_theme_constant_override(side, 6)
	margin.add_child(header)
	add_child(margin)

	_signature = TextureRect.new()
	_signature.stretch_mode = TextureRect.STRETCH_KEEP_ASPECT_CENTERED
	_signature.tooltip_text = "Didi — native MCP bridge for Godot"
	header.add_child(_signature)

	header.add_child(VSeparator.new())

	_state_dot = Label.new()
	_state_dot.text = _DOT
	header.add_child(_state_dot)

	_state_line = Label.new()
	_state_line.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	header.add_child(_state_line)

	_toast = Label.new()
	_toast.visible = false
	_toast.clip_text = true
	_toast.size_flags_horizontal = Control.SIZE_SHRINK_END
	header.add_child(_toast)

	_version_line = Label.new()
	header.add_child(_version_line)

	_refresh_button = Button.new()
	_refresh_button.flat = true
	_refresh_button.tooltip_text = "Re-read everything now"
	_refresh_button.pressed.connect(_on_manual_refresh)
	header.add_child(_refresh_button)


func _build_tabs() -> void:
	_tabs = TabContainer.new()
	_tabs.size_flags_vertical = Control.SIZE_EXPAND_FILL
	add_child(_tabs)
	_tabs.add_child(_build_dashboard_tab())
	_tabs.add_child(_build_connect_tab())
	_log_tab = _tabs.get_tab_count()
	_tabs.add_child(_build_log_tab())
	_tabs.add_child(_build_settings_tab())
	_diagnostics_tab = _tabs.get_tab_count()
	_tabs.add_child(_build_diagnostics_tab())
	# Diagnostics and the log are re-read when they are looked at rather than on
	# the refresh timer. Their work is a sweep of every directory on PATH and a
	# quarter of a megabyte of log file, which is not worth repeating every two
	# seconds behind a tab nobody has open.
	_tabs.tab_changed.connect(_on_tab_changed)


# --- dashboard ---------------------------------------------------------------

func _build_dashboard_tab() -> Control:
	var page := _page("Dashboard")
	var body := _body(page)

	var switches := HBoxContainer.new()
	switches.add_theme_constant_override("separation", 18)
	body.add_child(switches)

	_bridge_switch = CheckButton.new()
	_bridge_switch.text = "Live bridge"
	_bridge_switch.tooltip_text = "Load or unload the Didi extension, which is what opens and closes the endpoint"
	_bridge_switch.toggled.connect(_on_bridge_toggled)
	switches.add_child(_bridge_switch)

	_auto_switch = CheckButton.new()
	_auto_switch.text = "Auto refresh"
	_auto_switch.tooltip_text = "Re-read the bridge state on a timer while this screen is open"
	_auto_switch.toggled.connect(_on_auto_refresh_toggled)
	switches.add_child(_auto_switch)

	_technical_switch = CheckButton.new()
	_technical_switch.text = "Technical detail"
	_technical_switch.tooltip_text = "Show session id, endpoint, descriptor path and the other exact values"
	_technical_switch.toggled.connect(_on_technical_toggled)
	switches.add_child(_technical_switch)

	_bridge_result = Label.new()
	_bridge_result.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_bridge_result.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	switches.add_child(_bridge_result)

	var grid := GridContainer.new()
	grid.columns = 3
	grid.add_theme_constant_override("h_separation", 10)
	grid.add_theme_constant_override("v_separation", 10)
	body.add_child(grid)
	for key in ["bridge", "extension", "binary", "client", "sessions", "peers"]:
		_cards[key] = _add_card(grid)

	_detail_box = VBoxContainer.new()
	_detail_box.add_theme_constant_override("separation", 6)
	body.add_child(_detail_box)

	var detail_grid := GridContainer.new()
	detail_grid.columns = 2
	detail_grid.add_theme_constant_override("h_separation", 14)
	detail_grid.add_theme_constant_override("v_separation", 4)
	_detail_box.add_child(detail_grid)
	for key in ["Session", "Endpoint", "Project", "Bridge protocol", "Descriptor"]:
		_detail_rows[key] = _add_detail_row(detail_grid, key)

	_peers_heading = Label.new()
	_peers_heading.visible = false
	body.add_child(_peers_heading)

	_peers = VBoxContainer.new()
	body.add_child(_peers)

	var tail := Control.new()
	tail.size_flags_vertical = Control.SIZE_EXPAND_FILL
	body.add_child(tail)
	return page


## One dashboard card: a light, a title, the fact, and what to do about it.
func _add_card(grid: GridContainer) -> Dictionary:
	var panel := PanelContainer.new()
	panel.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	grid.add_child(panel)

	var margin := MarginContainer.new()
	for side in ["margin_left", "margin_right", "margin_top", "margin_bottom"]:
		margin.add_theme_constant_override(side, 10)
	panel.add_child(margin)

	var box := VBoxContainer.new()
	box.add_theme_constant_override("separation", 2)
	margin.add_child(box)

	var head := HBoxContainer.new()
	box.add_child(head)
	var lamp := Label.new()
	lamp.text = _DOT
	head.add_child(lamp)
	var title := Label.new()
	title.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	head.add_child(title)
	var verdict := Label.new()
	head.add_child(verdict)

	var value := Label.new()
	# Arbitrary rather than word-smart: half of what a card shows is a filesystem
	# path, and a path broken after "C:" reads worse than one broken anywhere.
	value.autowrap_mode = TextServer.AUTOWRAP_ARBITRARY
	box.add_child(value)

	var hint := Label.new()
	# The hint is a sentence, so it breaks between words. The value above is
	# usually a path, so it breaks anywhere -- a path is not made of words and
	# wrapping it as if it were leaves a ragged column and a broken-looking path.
	hint.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	box.add_child(hint)

	var action := Button.new()
	action.size_flags_horizontal = Control.SIZE_SHRINK_BEGIN
	action.visible = false
	box.add_child(action)

	return {"panel": panel, "lamp": lamp, "title": title, "verdict": verdict,
		"value": value, "hint": hint, "action": action}


func _set_card(key: String, lamp: int, title: String, value: String, hint: String) -> void:
	var card: Dictionary = _cards.get(key, {})
	if card.is_empty():
		return
	card["lamp"].add_theme_color_override("font_color", _lamp_colour(lamp))
	card["title"].text = title
	card["verdict"].text = _lamp_word(lamp)
	card["verdict"].add_theme_color_override("font_color", _lamp_colour(lamp))
	card["value"].text = value
	card["value"].tooltip_text = value
	card["hint"].text = hint
	card["hint"].tooltip_text = hint
	card["hint"].add_theme_color_override("font_color", _dim())
	card["panel"].add_theme_stylebox_override("panel", _card_style(lamp))
	card["action"].visible = false


## Puts the obvious next step on the card itself.
##
## Every card can say what to do; a card that is one click from doing it is the
## difference between a dashboard and a diagnosis.
func _set_card_action(key: String, label: String, handler: Callable) -> void:
	var card: Dictionary = _cards.get(key, {})
	if card.is_empty():
		return
	var action: Button = card["action"]
	action.text = label
	action.visible = true
	for existing in action.pressed.get_connections():
		action.pressed.disconnect(existing["callable"])
	action.pressed.connect(handler)


## The card's own background, with the light's colour along its leading edge.
##
## A coloured dot is easy to miss on a page of dots. A coloured edge on the card
## is what makes a red one findable at a glance, which is the whole point of a
## light.
func _card_style(lamp: int) -> StyleBoxFlat:
	# Cached per lamp. The dashboard repaints on a timer, and building three
	# StyleBoxes a second to describe three colours that never change is work
	# nobody asked for.
	if _card_styles.has(lamp):
		return _card_styles[lamp]
	var style := StyleBoxFlat.new()
	style.bg_color = _colour("dark_color_2", Color(0, 0, 0, 0.18))
	style.border_color = _lamp_colour(lamp)
	style.set_border_width(SIDE_LEFT, int(3 * Brand.editor_scale()))
	style.set_corner_radius_all(int(3 * Brand.editor_scale()))
	for side in [SIDE_LEFT, SIDE_RIGHT, SIDE_TOP, SIDE_BOTTOM]:
		style.set_content_margin(side, 0)
	_card_styles[lamp] = style
	return style


func _lamp_word(lamp: int) -> String:
	match lamp:
		Lamp.GREEN:
			return "ready"
		Lamp.AMBER:
			return "check"
		_:
			return "blocked"


func _lamp_colour(lamp: int) -> Color:
	match lamp:
		Lamp.GREEN:
			return _colour("success_color", Color("5ab55a"))
		Lamp.AMBER:
			return _colour("warning_color", Color("e0a33a"))
		_:
			return _colour("error_color", Color("e5545c"))


# --- connect -----------------------------------------------------------------

func _build_connect_tab() -> Control:
	var page := _page("Connect")
	var body := _body(page)

	var intro := Label.new()
	intro.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	intro.text = "Your assistant starts Didi itself, so this is the configuration that decides how it starts: which binary, which project, and which options."
	body.add_child(intro)

	var executable_row := HBoxContainer.new()
	body.add_child(executable_row)
	var executable_label := Label.new()
	executable_label.text = "Server binary"
	executable_label.custom_minimum_size.x = 110.0 * Brand.editor_scale()
	executable_row.add_child(executable_label)

	_executable_field = LineEdit.new()
	_executable_field.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_executable_field.placeholder_text = "Path to " + ClientConfig.executable_name()
	_executable_field.text_changed.connect(_on_executable_typed)
	executable_row.add_child(_executable_field)

	var browse := Button.new()
	browse.text = "Browse"
	browse.pressed.connect(_on_browse_pressed)
	executable_row.add_child(browse)

	var detect := Button.new()
	detect.text = "Detect"
	detect.tooltip_text = "Look in the usual build locations and on PATH"
	detect.pressed.connect(_on_detect_pressed)
	executable_row.add_child(detect)

	_executable_state = Label.new()
	_executable_state.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	body.add_child(_executable_state)

	var client_row := HBoxContainer.new()
	body.add_child(client_row)
	var client_label := Label.new()
	client_label.text = "Client"
	client_label.custom_minimum_size.x = 110.0 * Brand.editor_scale()
	client_row.add_child(client_label)

	_client_picker = OptionButton.new()
	for client in [ClientConfig.Client.CLAUDE_CODE, ClientConfig.Client.CURSOR,
			ClientConfig.Client.CLAUDE_DESKTOP, ClientConfig.Client.VS_CODE]:
		_client_picker.add_item(ClientConfig.client_name(client), client)
	_client_picker.item_selected.connect(_on_client_selected)
	client_row.add_child(_client_picker)

	_destination_line = Label.new()
	_destination_line.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	client_row.add_child(_destination_line)

	_config_view = TextEdit.new()
	_config_view.editable = false
	_config_view.size_flags_vertical = Control.SIZE_SHRINK_BEGIN
	_config_view.custom_minimum_size.y = 260.0 * Brand.editor_scale()
	body.add_child(_config_view)

	_connect_notice = Label.new()
	_connect_notice.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	_connect_notice.visible = false
	body.add_child(_connect_notice)

	var actions := HBoxContainer.new()
	body.add_child(actions)
	var copy := Button.new()
	copy.text = "Copy configuration"
	copy.pressed.connect(_on_copy_config)
	actions.add_child(copy)

	_write_button = Button.new()
	_write_button.text = "Write into project"
	_write_button.pressed.connect(_on_write_config)
	actions.add_child(_write_button)

	var options := Button.new()
	options.text = "Change the options"
	options.tooltip_text = "Log level, endpoint name and confirmation policy live in Settings"
	options.pressed.connect(_on_open_settings)
	actions.add_child(options)

	var tail := Control.new()
	tail.size_flags_vertical = Control.SIZE_EXPAND_FILL
	body.add_child(tail)
	return page


# --- log ---------------------------------------------------------------------

func _build_log_tab() -> Control:
	var page := _page("Log")
	var body := _body(page)

	var controls := HBoxContainer.new()
	controls.add_theme_constant_override("separation", 10)
	body.add_child(controls)

	_log_source = OptionButton.new()
	_log_source.add_item("Console activity", 0)
	_log_source.add_item("Last project run", 1)
	_log_source.item_selected.connect(_on_log_source_changed)
	controls.add_child(_log_source)

	for entry in [[Log.Level.ERROR, "Errors"], [Log.Level.WARN, "Warnings"], [Log.Level.INFO, "Info"]]:
		var toggle := CheckBox.new()
		toggle.text = entry[1]
		toggle.button_pressed = true
		toggle.toggled.connect(_on_log_level_filter_toggled)
		_log_levels[entry[0]] = toggle
		controls.add_child(toggle)

	_log_follow = CheckButton.new()
	_log_follow.text = "Follow"
	_log_follow.tooltip_text = "Keep the newest line in view"
	_log_follow.toggled.connect(_on_log_follow_toggled)
	controls.add_child(_log_follow)

	_log_filter = LineEdit.new()
	_log_filter.placeholder_text = "Filter"
	_log_filter.custom_minimum_size.x = 180.0 * Brand.editor_scale()
	_log_filter.text_changed.connect(_on_log_filter_changed)
	controls.add_child(_log_filter)

	var copy := Button.new()
	copy.text = "Copy"
	copy.pressed.connect(_on_copy_log)
	controls.add_child(copy)

	var clear := Button.new()
	clear.text = "Clear"
	clear.tooltip_text = "Clear the console's own activity log"
	clear.pressed.connect(_on_clear_log)
	controls.add_child(clear)

	_log_source_note = Label.new()
	_log_source_note.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	body.add_child(_log_source_note)

	_log_view = RichTextLabel.new()
	_log_view.bbcode_enabled = true
	_log_view.selection_enabled = true
	_log_view.size_flags_vertical = Control.SIZE_EXPAND_FILL
	body.add_child(_log_view)
	return page


# --- settings ----------------------------------------------------------------

func _build_settings_tab() -> Control:
	var page := _page("Settings")
	var body := _body(page)

	var intro := Label.new()
	intro.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	intro.text = "Stored in Editor Settings under didi/, which lives with your editor rather than in this project — so nothing Didi can write to the project can change them."
	body.add_child(intro)

	var grid := GridContainer.new()
	grid.columns = 2
	grid.add_theme_constant_override("h_separation", 14)
	grid.add_theme_constant_override("v_separation", 10)
	body.add_child(grid)

	_add_setting_label(grid, "Refresh every", "How often the dashboard re-reads the bridge state while it is open.")
	_refresh_seconds = SpinBox.new()
	_refresh_seconds.min_value = 0.5
	_refresh_seconds.max_value = 30.0
	_refresh_seconds.step = 0.5
	_refresh_seconds.suffix = "s"
	_refresh_seconds.value_changed.connect(_on_refresh_seconds_changed)
	_add_setting_control(grid, _refresh_seconds, 120.0)

	_add_setting_label(grid, "Verify the binary",
		"Diagnostics asks the located binary for its version, which starts it briefly. Off means it is checked for existence only, and the dashboard says so rather than claiming more.")
	_verify_switch = CheckButton.new()
	_verify_switch.text = "Run it to confirm what it is"
	_verify_switch.toggled.connect(_on_verify_toggled)
	_add_setting_control(grid, _verify_switch, 0.0)

	_add_setting_label(grid, "Server log level",
		"Written into the generated configuration as --log-level. Takes effect the next time your client starts Didi.")
	_log_level = OptionButton.new()
	for level in Settings.LOG_LEVELS:
		_log_level.add_item(level)
	_log_level.item_selected.connect(_on_log_level_changed)
	_add_setting_control(grid, _log_level, 120.0)

	_add_setting_label(grid, "Endpoint name",
		"Written as --pipe-name. Leave empty unless something else on this machine already uses the default name.")
	_pipe_name = LineEdit.new()
	_pipe_name.placeholder_text = "default"
	_pipe_name.text_changed.connect(_on_pipe_name_changed)
	_add_setting_control(grid, _pipe_name, 240.0)

	_add_setting_label(grid, "Skip confirmation",
		"Adds --yolo to the generated configuration. Didi accepts this only as a launch argument, never from anything a tool call can reach.")
	_skip_confirmations = CheckBox.new()
	_skip_confirmations.text = "Destructive tools run without asking"
	_skip_confirmations.toggled.connect(_on_skip_confirmations_toggled)
	_add_setting_control(grid, _skip_confirmations, 0.0)

	_skip_warning = Label.new()
	_skip_warning.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	_skip_warning.visible = false
	_skip_warning.text = "Mutations will execute without review, and each affected result records confirmation: skipped. Intended for unattended runs."
	body.add_child(_skip_warning)

	var tail := Control.new()
	tail.size_flags_vertical = Control.SIZE_EXPAND_FILL
	body.add_child(tail)
	return page


# --- diagnostics -------------------------------------------------------------

func _build_diagnostics_tab() -> Control:
	var page := _page("Diagnostics")
	var body := _body(page)

	var actions := HBoxContainer.new()
	body.add_child(actions)
	var run := Button.new()
	run.text = "Run checks"
	run.pressed.connect(_on_run_checks_pressed)
	actions.add_child(run)

	var copy_report := Button.new()
	copy_report.text = "Copy report"
	copy_report.pressed.connect(_on_copy_report)
	actions.add_child(copy_report)

	var scroll := ScrollContainer.new()
	scroll.size_flags_vertical = Control.SIZE_EXPAND_FILL
	scroll.horizontal_scroll_mode = ScrollContainer.SCROLL_MODE_DISABLED
	body.add_child(scroll)

	_check_rows = VBoxContainer.new()
	_check_rows.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	scroll.add_child(_check_rows)
	return page


# --- shared construction -----------------------------------------------------

func _build_dialogs() -> void:
	_confirm = ConfirmationDialog.new()
	_confirm.confirmed.connect(_on_write_confirmed)
	add_child(_confirm)

	_confirm_bridge = ConfirmationDialog.new()
	_confirm_bridge.title = "Close the live bridge"
	_confirm_bridge.ok_button_text = "Close it"
	_confirm_bridge.confirmed.connect(_on_bridge_close_confirmed)
	_confirm_bridge.canceled.connect(_on_bridge_close_cancelled)
	add_child(_confirm_bridge)

	_failure = AcceptDialog.new()
	_failure.title = "Didi"
	add_child(_failure)

	_file_dialog = EditorFileDialog.new()
	_file_dialog.file_mode = EditorFileDialog.FILE_MODE_OPEN_FILE
	_file_dialog.access = EditorFileDialog.ACCESS_FILESYSTEM
	_file_dialog.title = "Select the Didi server binary"
	_file_dialog.file_selected.connect(_on_executable_chosen)
	add_child(_file_dialog)

	_toast_timer = Timer.new()
	_toast_timer.one_shot = true
	_toast_timer.wait_time = 4.0
	_toast_timer.timeout.connect(_on_toast_expired)
	add_child(_toast_timer)


## One tab page, with its content held to a readable measure.
##
## The console fills the editor's main screen, which on a wide monitor is two
## thousand pixels of it. Sentences set that wide are not read, they are scanned
## past, so the content column stops at a measure and the rest of the width is
## left empty rather than filled for the sake of filling it.
func _page(title: String) -> Control:
	var margin := MarginContainer.new()
	margin.name = title
	for side in ["margin_left", "margin_right", "margin_top", "margin_bottom"]:
		margin.add_theme_constant_override(side, 12)

	var row := HBoxContainer.new()
	margin.add_child(row)

	var body := VBoxContainer.new()
	body.add_theme_constant_override("separation", 8)
	body.custom_minimum_size.x = _CONTENT_MEASURE * Brand.editor_scale()
	row.add_child(body)

	var spacer := Control.new()
	spacer.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	row.add_child(spacer)

	margin.set_meta("body", body)
	return margin


func _body(page: Control) -> VBoxContainer:
	return page.get_meta("body") as VBoxContainer


func _add_detail_row(grid: GridContainer, title: String) -> LineEdit:
	var label := Label.new()
	label.text = title
	grid.add_child(label)
	var value := LineEdit.new()
	value.editable = false
	value.flat = true
	value.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	grid.add_child(value)
	return value


func _add_setting_label(grid: GridContainer, title: String, explanation: String) -> void:
	var box := VBoxContainer.new()
	box.add_theme_constant_override("separation", 0)
	var label := Label.new()
	label.text = title
	box.add_child(label)
	var hint := Label.new()
	hint.text = explanation
	hint.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	hint.custom_minimum_size.x = 340.0 * Brand.editor_scale()
	hint.add_theme_color_override("font_color", _dim())
	box.add_child(hint)
	grid.add_child(box)


## Places a setting's control in the grid at its own size.
##
## A control added straight to a GridContainer is stretched to the cell, and a
## cell here is as tall as a wrapped paragraph and as wide as the page. A spin
## box drawn that way looks like a rendering fault rather than a spin box.
func _add_setting_control(grid: GridContainer, control: Control, width: float) -> void:
	control.size_flags_horizontal = Control.SIZE_SHRINK_BEGIN
	control.size_flags_vertical = Control.SIZE_SHRINK_BEGIN
	if width > 0.0:
		control.custom_minimum_size.x = width * Brand.editor_scale()
	grid.add_child(control)


# --- theming -----------------------------------------------------------------

func _apply_theme() -> void:
	var ink := Brand.editor_ink(self)
	_signature.texture = Brand.signature(_HEADER_SIGNATURE_HEIGHT, ink)
	if _signature.texture != null:
		_signature.custom_minimum_size = _signature.texture.get_size() / Brand.editor_scale()
	if has_theme_icon("Reload", "EditorIcons"):
		_refresh_button.icon = get_theme_icon("Reload", "EditorIcons")
	else:
		_refresh_button.text = "Refresh"
	for label in [_version_line, _destination_line, _executable_state, _toast,
			_log_source_note, _bridge_result]:
		if label != null:
			label.add_theme_color_override("font_color", _dim())
	if _config_view != null and has_theme_font("source", "EditorFonts"):
		_config_view.add_theme_font_override("font", get_theme_font("source", "EditorFonts"))
	if _log_view != null and has_theme_font("source", "EditorFonts"):
		_log_view.add_theme_font_override("normal_font", get_theme_font("source", "EditorFonts"))
	for label in [_skip_warning, _connect_notice]:
		if label != null:
			label.add_theme_color_override("font_color", _lamp_colour(Lamp.AMBER))
	_card_styles.clear()
	_paint_header()


func _colour(colour_name: String, fallback: Color) -> Color:
	if has_theme_color(colour_name, "Editor"):
		return get_theme_color(colour_name, "Editor")
	return fallback


func _dim() -> Color:
	var ink := Brand.editor_ink(self)
	return Color(ink.r, ink.g, ink.b, 0.65)


# --- refresh -----------------------------------------------------------------

func _on_visibility_changed() -> void:
	# The signal can arrive while this control is entering the tree and its own
	# children have not been propagated into it yet, which is a timer that
	# cannot be started. Asking the timer directly is the only guard that holds
	# for that window.
	if _timer == null or not _timer.is_inside_tree():
		return
	if is_visible_in_tree() and Settings.flag(Settings.AUTO_REFRESH):
		_timer.wait_time = Settings.refresh_seconds()
		_timer.start()
		_refresh()
	else:
		_timer.stop()


func _refresh() -> void:
	_sessions = Sessions.discover()
	_own = Sessions.own(_sessions)
	var loaded := GDExtensionManager.is_extension_loaded(Diagnostics.EXTENSION_PATH)
	if _own != null:
		_bridge = Bridge.LIVE
	elif loaded:
		_bridge = Bridge.LOADED_WITHOUT_SESSION
	else:
		_bridge = Bridge.NOT_LOADED
	_record_transitions()
	_paint_header()
	_paint_cards()
	_refresh_details()
	_refresh_connect()
	if _tabs != null and _tabs.current_tab == _log_tab:
		_refresh_log()


## Writes what changed into the console's own log.
##
## The dashboard shows the present. This is what makes the past answerable: a
## bridge that dropped while nobody was looking leaves a line with a time on it.
func _record_transitions() -> void:
	if _last_bridge != _bridge:
		if _last_bridge != -1:
			match _bridge:
				Bridge.LIVE:
					Log.info("Live bridge up.")
				Bridge.LOADED_WITHOUT_SESSION:
					Log.warn("Extension loaded but no session is published.")
				_:
					Log.warn("Didi extension is not loaded.")
		_last_bridge = _bridge

	var session_id := _own.session_id if _own != null else ""
	if session_id != _last_session_id:
		if not session_id.is_empty():
			Log.info("Session %s published for this editor (pid %d)." % [_own.short_id(), _own.pid])
		elif not _last_session_id.is_empty():
			Log.info("Session %s retired." % _last_session_id.substr(0, 8))
		_last_session_id = session_id

	var peers := _sessions.size() - (1 if _own != null else 0)
	if _last_peer_count != -1 and peers != _last_peer_count:
		Log.info("Other published sessions on this machine: %d." % peers)
	_last_peer_count = peers


func _paint_header() -> void:
	if _state_dot == null:
		return
	_state_dot.add_theme_color_override("font_color", _lamp_colour(_bridge_lamp()))
	_state_line.text = _state_summary()
	_version_line.text = _version_summary()


func _bridge_lamp() -> int:
	match _bridge:
		Bridge.LIVE:
			return Lamp.GREEN
		Bridge.LOADED_WITHOUT_SESSION:
			return Lamp.AMBER
		_:
			return Lamp.RED


func _state_summary() -> String:
	match _bridge:
		Bridge.LIVE:
			return "Live — an assistant can reach this editor"
		Bridge.LOADED_WITHOUT_SESSION:
			return "Loaded, but no session is published"
		_:
			return "Not running — nothing can reach this editor"


func _version_summary() -> String:
	var version := Diagnostics.plugin_version()
	if _own != null and not _own.protocol_version.is_empty():
		return "Didi %s · bridge %s" % [version, _own.protocol_version]
	return "Didi %s" % version


func _paint_cards() -> void:
	_suppress_writes = true
	_bridge_switch.button_pressed = _bridge != Bridge.NOT_LOADED
	_suppress_writes = false

	match _bridge:
		Bridge.LIVE:
			_set_card("bridge", Lamp.GREEN, "Live bridge", "Up for %s" % _uptime_text(),
				"Scene inspection and mutation, viewport capture and signals execute in this window.")
		Bridge.LOADED_WITHOUT_SESSION:
			_set_card("bridge", Lamp.AMBER, "Live bridge", "No session published",
				"The extension started and its endpoint did not. The Output panel carries the reason, tagged GDEXT_IPC or IPC_SERVER.")
		_:
			_set_card("bridge", Lamp.RED, "Live bridge", "Closed",
				"Live tools report unavailable. File-based tools still work: they never needed the editor.")
			_set_card_action("bridge", "Open it", _on_load_extension)

	var library := Diagnostics.LIBRARY_DIRECTORY.path_join(Diagnostics.library_name())
	if GDExtensionManager.is_extension_loaded(Diagnostics.EXTENSION_PATH):
		_set_card("extension", Lamp.GREEN, "Extension", "Loaded", library)
	elif FileAccess.file_exists(library):
		_set_card("extension", Lamp.AMBER, "Extension", "Present, not loaded",
			"%s exists. Godot loads extensions at editor start, so a library added since then needs loading now or a restart." % library)
		_set_card_action("extension", "Load it", _on_load_extension)
	else:
		_set_card("extension", Lamp.RED, "Extension", "Missing",
			"%s is not there. Build Didi for this platform, or copy the released library in." % library)

	var executable := ClientConfig.resolve_executable()
	if executable.is_empty():
		_set_card("binary", Lamp.RED, "Server binary", "Not found",
			"Looked in the usual build locations and on PATH. Anything inside the project has to be chosen by hand.")
		_set_card_action("binary", "Detect", _on_detect_pressed)
	elif not FileAccess.file_exists(executable):
		_set_card("binary", Lamp.RED, "Server binary", "Missing", "%s does not exist." % executable)
		_set_card_action("binary", "Choose one", _on_browse_pressed)
	elif _verified_binary == executable:
		_set_card("binary", Lamp.GREEN, "Server binary", "Verified", executable)
	else:
		_set_card("binary", Lamp.AMBER, "Server binary", "Found, not verified", executable)
		_set_card_action("binary", "Verify it", _on_verify_binary_now)

	_paint_client_card()

	if _own != null:
		_set_card("sessions", Lamp.GREEN, "This editor",
			"Session %s · pid %d" % [_own.short_id(), _own.pid], _own.project_path)
	else:
		_set_card("sessions", Lamp.AMBER, "This editor", "pid %d · no session" % OS.get_process_id(),
			ClientConfig.project_root())

	var peers := _sessions.size() - (1 if _own != null else 0)
	if peers == 0:
		_set_card("peers", Lamp.GREEN, "Other sessions", "None",
			"Nothing else on this machine is publishing a Didi session.")
	else:
		_set_card("peers", Lamp.AMBER, "Other sessions", "%d published" % peers,
			"An assistant pointed at another project reaches that project rather than this one.")
		_set_card_action("peers", "List them", _on_show_peers)


func _paint_client_card() -> void:
	var client := _selected_client()
	var target := ClientConfig.project_file(client)
	if target.is_empty():
		_set_card("client", Lamp.AMBER, "Client configuration", "Outside this project",
			"%s reads %s, which this console generates but will not write." % [
				ClientConfig.client_name(client), ClientConfig.destination(client)])
		return
	if FileAccess.file_exists(target):
		_set_card("client", Lamp.GREEN, "Client configuration", target,
			"Present in the project. Connect shows what it should contain.")
	else:
		_set_card("client", Lamp.AMBER, "Client configuration", "%s absent" % target,
			"Writing it creates the file with this project's path already filled in. You are shown it first.")
		_set_card_action("client", "Write it", _on_write_config)


func _uptime_text() -> String:
	if _own == null:
		return "—"
	var seconds := int(_own.age_seconds())
	if seconds < 60:
		return "%d s" % seconds
	if seconds < 3600:
		return "%d min %d s" % [seconds / 60, seconds % 60]
	return "%d h %d min" % [seconds / 3600, (seconds % 3600) / 60]


func _refresh_details() -> void:
	_detail_box.visible = Settings.flag(Settings.SHOW_TECHNICAL)
	if _detail_box.visible:
		_set_detail("Session", _own.session_id if _own != null else "—")
		_set_detail("Endpoint", _own.endpoint if _own != null else "—")
		_set_detail("Project", ClientConfig.project_root())
		_set_detail("Bridge protocol", _own.protocol_version if _own != null else "—")
		_set_detail("Descriptor", _own.source_path if _own != null
			else ", ".join(Sessions.descriptor_directories()))
	_refresh_peers()


func _set_detail(key: String, value: String) -> void:
	var field: LineEdit = _detail_rows.get(key)
	if field != null and field.text != value:
		field.text = value


## The other sessions on this machine.
##
## Listed because the question they answer is real -- an assistant that attaches
## to the wrong project reaches the wrong scene tree -- and capped because the
## answer stops being useful past a handful. What is shown is what is on disk: a
## descriptor is retired when its process shuts down cleanly, so a process that
## was killed leaves one behind. Didi itself verifies liveness at the moment a
## client attaches, using process facts GDScript cannot ask for, so this list is
## the weaker of the two views and says so.
func _refresh_peers() -> void:
	for child in _peers.get_children():
		_peers.remove_child(child)
		child.queue_free()
	var others: Array = []
	for session in _sessions:
		if _own == null or session.pid != _own.pid:
			others.append(session)
	var show_list := Settings.flag(Settings.SHOW_TECHNICAL) and not others.is_empty()
	_peers_heading.visible = show_list
	if not show_list:
		return

	var project := ClientConfig.project_root()
	var same_project := 0
	for session in others:
		if _same_project(session.project_path, project):
			same_project += 1
	_peers_heading.text = "%d other session%s published on this machine%s" % [
		others.size(),
		"" if others.size() == 1 else "s",
		(", %d for this project" % same_project) if same_project > 0 else "",
	]

	for index in range(min(others.size(), _PEER_LIMIT)):
		var session = others[index]
		var row := Label.new()
		row.add_theme_color_override("font_color", _dim())
		row.text = "%s  ·  %s  ·  pid %d  ·  %s  ·  %s" % [
			session.short_id(), session.kind, session.pid,
			session.age_phrase(), session.project_path]
		if _same_project(session.project_path, project):
			row.text += "  ·  this project"
		_peers.add_child(row)

	var note := Label.new()
	note.add_theme_color_override("font_color", _dim())
	note.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	note.text = ""
	if others.size() > _PEER_LIMIT:
		note.text = "and %d more. " % (others.size() - _PEER_LIMIT)
	note.text += "A session is listed while its descriptor is on disk; one whose process was killed rather than closed stays until Didi next reaps it."
	_peers.add_child(note)


## Whether two recorded project paths are the same project. Descriptors record
## the platform's own separators, and the editor reports forward slashes, so a
## textual comparison has to normalise before it means anything.
func _same_project(left: String, right: String) -> bool:
	var a := left.replace("\\", "/").rstrip("/")
	var b := right.replace("\\", "/").rstrip("/")
	if OS.get_name() == "Windows":
		return a.to_lower() == b.to_lower()
	return a == b


func _refresh_connect() -> void:
	if _executable_field == null:
		return
	var configured := str(Settings.get_value(Settings.SERVER_EXECUTABLE))
	if not _executable_field.has_focus() and _executable_field.text != configured:
		_executable_field.text = configured

	var executable := ClientConfig.resolve_executable()
	if executable.is_empty():
		_executable_state.text = "No binary found yet. Detect looks in nearby build directories and on PATH; anything inside the project has to be chosen with Browse."
	elif not FileAccess.file_exists(executable):
		_executable_state.text = "%s does not exist." % executable
	elif configured.is_empty():
		_executable_state.text = "Found %s" % executable
	else:
		_executable_state.text = executable

	var client := _selected_client()
	_destination_line.text = "→ " + ClientConfig.destination(client)
	var generated := ClientConfig.to_json(executable)
	if _config_view.text != generated:
		_config_view.text = generated
	var project_file := ClientConfig.project_file(client)
	_write_button.disabled = project_file.is_empty()
	if project_file.is_empty():
		_write_button.tooltip_text = "%s is outside this project — copy the configuration instead." % ClientConfig.destination(client)
	else:
		_write_button.tooltip_text = "Write %s" % project_file

	var notices := PackedStringArray()
	if Settings.client_skip_confirmations():
		notices.append("This configuration passes --yolo: destructive tools will not ask before running.")
	if ClientConfig.is_inside_project(executable):
		notices.append("This binary is inside the project, which is somewhere Didi's own file tools can write. Detect never picks such a path; this one was chosen here.")
	_connect_notice.visible = not notices.is_empty()
	_connect_notice.text = "\n".join(notices)


func _selected_client() -> int:
	if _client_picker == null or _client_picker.selected < 0:
		return ClientConfig.Client.CLAUDE_CODE
	return _client_picker.get_item_id(_client_picker.selected)


# --- log ---------------------------------------------------------------------

func _refresh_log() -> void:
	if _log_view == null:
		return
	var showing_run := _log_source.get_selected_id() == 1
	var records: Array = Log.read_run_log() if showing_run else Log.records()
	if showing_run:
		if Log.run_log_exists():
			_log_source_note.text = "Godot's log for the last run of this project, at %s. Didi's own server writes to its process's standard error, which your MCP client captures; the level for that is in Settings." % Log.run_log_path()
		else:
			_log_source_note.text = "No run log yet. Godot writes one when the project runs; the editor process does not write one at all."
	else:
		_log_source_note.text = "What this console has watched happen, oldest first. It survives the panel being rebuilt, and is never written to disk."

	var filter := _log_filter.text.strip_edges().to_lower()
	var lines := PackedStringArray()
	var shown := 0
	for index in range(records.size() - 1, -1, -1):
		if shown >= _LOG_LINES:
			break
		var record = records[index]
		var toggle: CheckBox = _log_levels.get(record.level)
		if toggle != null and not toggle.button_pressed:
			continue
		if not filter.is_empty() and not record.text.to_lower().contains(filter):
			continue
		lines.append("[color=#%s]%s  %s[/color]  %s" % [
			_lamp_colour(_level_lamp(record.level)).to_html(false),
			record.stamp(), record.level_word().rpad(5), _escape_bbcode(record.text)])
		shown += 1
	lines.reverse()

	_log_view.text = ""
	if lines.is_empty():
		_log_view.append_text("[color=#%s]Nothing to show.[/color]" % _dim().to_html(false))
		return
	_log_view.append_text("\n".join(lines))
	if _log_follow.button_pressed:
		_log_view.scroll_to_line(max(0, _log_view.get_line_count() - 1))


func _level_lamp(level: int) -> int:
	match level:
		Log.Level.ERROR:
			return Lamp.RED
		Log.Level.WARN:
			return Lamp.AMBER
		_:
			return Lamp.GREEN


## Log lines are arbitrary text and BBCode is markup, so a path containing a
## bracket would otherwise be interpreted rather than shown.
func _escape_bbcode(text: String) -> String:
	return text.replace("[", "[lb]")


# --- diagnostics -------------------------------------------------------------

func _run_checks(allow_process: bool) -> void:
	if _check_rows == null:
		return
	var checks := Diagnostics.run(allow_process)
	_report = Diagnostics.report_text(checks)
	for child in _check_rows.get_children():
		_check_rows.remove_child(child)
		child.queue_free()
	for check in checks:
		_check_rows.add_child(_check_row(check))


func _check_row(check) -> Control:
	var row := HBoxContainer.new()
	var dot := Label.new()
	dot.text = _DOT
	dot.add_theme_color_override("font_color", _lamp_colour(_check_lamp(check.state)))
	dot.size_flags_vertical = Control.SIZE_SHRINK_BEGIN
	row.add_child(dot)

	var text := VBoxContainer.new()
	text.add_theme_constant_override("separation", 0)
	text.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	row.add_child(text)

	var title := Label.new()
	title.text = check.title
	text.add_child(title)

	var detail := Label.new()
	detail.text = check.detail
	detail.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	detail.add_theme_color_override("font_color", _dim())
	text.add_child(detail)
	return row


func _check_lamp(state: int) -> int:
	match state:
		Diagnostics.State.OK:
			return Lamp.GREEN
		Diagnostics.State.WARN:
			return Lamp.AMBER
		_:
			return Lamp.RED


# --- settings ----------------------------------------------------------------

func _reload_settings_controls() -> void:
	_suppress_writes = true
	_refresh_seconds.set_value_no_signal(Settings.refresh_seconds())
	var level := Settings.client_log_level()
	for index in _log_level.item_count:
		if _log_level.get_item_text(index) == level:
			_log_level.select(index)
			break
	_skip_confirmations.set_pressed_no_signal(Settings.client_skip_confirmations())
	_skip_warning.visible = _skip_confirmations.button_pressed
	_verify_switch.set_pressed_no_signal(Settings.flag(Settings.VERIFY_BY_RUNNING))
	_pipe_name.text = Settings.client_pipe_name()
	_auto_switch.set_pressed_no_signal(Settings.flag(Settings.AUTO_REFRESH))
	_technical_switch.set_pressed_no_signal(Settings.flag(Settings.SHOW_TECHNICAL))
	_log_follow.set_pressed_no_signal(Settings.flag(Settings.LOG_FOLLOW))
	_suppress_writes = false


func _on_refresh_seconds_changed(value: float) -> void:
	Settings.set_value(Settings.REFRESH_SECONDS, value)
	if _timer != null:
		_timer.wait_time = Settings.refresh_seconds()


func _on_log_level_changed(index: int) -> void:
	Settings.set_value(Settings.CLIENT_LOG_LEVEL, _log_level.get_item_text(index))
	_refresh_connect()


func _on_pipe_name_changed(text: String) -> void:
	Settings.set_value(Settings.CLIENT_PIPE_NAME, text.strip_edges())
	_refresh_connect()


func _on_skip_confirmations_toggled(pressed: bool) -> void:
	Settings.set_value(Settings.CLIENT_SKIP_CONFIRMATIONS, pressed)
	_skip_warning.visible = pressed
	if pressed:
		Log.warn("Generated configuration now passes --yolo.")
	else:
		Log.info("Generated configuration no longer passes --yolo.")
	_refresh_connect()


func _on_verify_toggled(pressed: bool) -> void:
	Settings.set_value(Settings.VERIFY_BY_RUNNING, pressed)


func _on_auto_refresh_toggled(pressed: bool) -> void:
	if _suppress_writes:
		return
	Settings.set_value(Settings.AUTO_REFRESH, pressed)
	_on_visibility_changed()
	if not pressed:
		_refresh()


func _on_technical_toggled(pressed: bool) -> void:
	if _suppress_writes:
		return
	Settings.set_value(Settings.SHOW_TECHNICAL, pressed)
	_refresh_details()


func _on_log_follow_toggled(pressed: bool) -> void:
	if _suppress_writes:
		return
	Settings.set_value(Settings.LOG_FOLLOW, pressed)
	_refresh_log()


func _on_log_level_filter_toggled(_pressed: bool) -> void:
	_refresh_log()


func _on_log_filter_changed(_text: String) -> void:
	_refresh_log()


func _on_log_source_changed(_index: int) -> void:
	_refresh_log()


# --- actions -----------------------------------------------------------------

func _on_manual_refresh() -> void:
	ClientConfig.forget_detection()
	_refresh()
	if _tabs != null and _tabs.current_tab == _diagnostics_tab:
		_run_checks(Settings.flag(Settings.VERIFY_BY_RUNNING))
	elif _tabs != null and _tabs.current_tab == _log_tab:
		_refresh_log()


func _on_tab_changed(tab: int) -> void:
	if tab == _diagnostics_tab:
		_run_checks(Settings.flag(Settings.VERIFY_BY_RUNNING))
	elif tab == _log_tab:
		_refresh_log()


func _on_open_settings() -> void:
	if _tabs != null:
		_tabs.current_tab = _SETTINGS_TAB


func _on_client_selected(_index: int) -> void:
	_refresh_connect()
	_paint_client_card()


func _on_executable_typed(text: String) -> void:
	Settings.set_value(Settings.SERVER_EXECUTABLE, text.strip_edges())
	ClientConfig.forget_detection()
	_verified_binary = ""
	_refresh_connect()


func _on_browse_pressed() -> void:
	_file_dialog.popup_centered_ratio(0.6)


func _on_executable_chosen(path: String) -> void:
	Settings.set_value(Settings.SERVER_EXECUTABLE, path)
	_executable_field.text = path
	_verified_binary = ""
	Log.info("Server binary set to %s." % path)
	_refresh_connect()
	_paint_cards()


func _on_detect_pressed() -> void:
	ClientConfig.forget_detection()
	var found := ClientConfig.detect_executable()
	if found.is_empty():
		Log.warn("No Didi binary found in the usual locations or on PATH.")
		_show_toast("No Didi binary found — set one with Browse.")
		return
	Settings.set_value(Settings.SERVER_EXECUTABLE, found)
	_executable_field.text = found
	_verified_binary = ""
	Log.info("Detected server binary at %s." % found)
	_refresh_connect()
	_paint_cards()
	_show_toast("Found %s" % found)


func _on_load_extension() -> void:
	_apply_bridge_status(GDExtensionManager.load_extension(Diagnostics.EXTENSION_PATH), false)


func _on_verify_binary_now() -> void:
	_run_checks(true)
	var executable := ClientConfig.resolve_executable()
	_verified_binary = executable if _report.contains("[ok] Server responds") else ""
	Log.info("Verified %s by running it." % executable if not _verified_binary.is_empty()
		else "Could not verify %s by running it." % executable)
	_paint_cards()


func _on_show_peers() -> void:
	Settings.set_value(Settings.SHOW_TECHNICAL, true)
	_suppress_writes = true
	_technical_switch.button_pressed = true
	_suppress_writes = false
	_refresh_details()


func _on_run_checks_pressed() -> void:
	ClientConfig.forget_detection()
	var verify := Settings.flag(Settings.VERIFY_BY_RUNNING)
	_run_checks(verify)
	if verify:
		var executable := ClientConfig.resolve_executable()
		_verified_binary = executable if _report.contains("[ok] Server responds") else ""
	Log.info("Diagnostics run%s." % (" with the binary verified" if verify else ""))
	_paint_cards()


func _on_copy_config() -> void:
	DisplayServer.clipboard_set(_config_view.text)
	_show_toast("Configuration copied.")


func _on_copy_report() -> void:
	# Copying a report nobody has run would put an empty one on the clipboard,
	# which is the least useful thing a bug report can contain.
	if _report.is_empty():
		_run_checks(false)
	DisplayServer.clipboard_set(_report)
	_show_toast("Report copied.")


func _on_copy_log() -> void:
	DisplayServer.clipboard_set(_log_view.get_parsed_text())
	_show_toast("Log copied.")


func _on_clear_log() -> void:
	Log.clear()
	Log.info("Console activity log cleared.")
	_refresh_log()


func _on_write_config() -> void:
	var target := ClientConfig.project_file(_selected_client())
	if target.is_empty():
		return
	_confirm.title = "Write %s" % target
	if FileAccess.file_exists(target):
		_confirm.dialog_text = "%s already exists and will be replaced, including any other MCP servers configured in it.\n\nWrite it anyway?" % target
	else:
		_confirm.dialog_text = "Create %s with this configuration?" % target
	_confirm.popup_centered()


func _on_write_confirmed() -> void:
	var client := _selected_client()
	var failure := ClientConfig.write_project_file(client, ClientConfig.resolve_executable())
	if failure.is_empty():
		var target := ClientConfig.project_file(client)
		Log.info("Wrote %s." % target)
		_show_toast("Wrote %s" % target)
		EditorInterface.get_resource_filesystem().scan()
		_paint_client_card()
	else:
		# A write that failed is the one outcome worth interrupting for: the
		# person is about to go and look for a file that is not there.
		Log.error(failure)
		_show_failure(failure)


## Opens or closes the live bridge for real.
##
## The bridge is the GDExtension's, not this plugin's: the endpoint is opened
## when the library initialises and closed when it is deinitialised, so the only
## honest way to close it from here is to unload the library. Godot answers that
## request with a status, including one that means "not without a restart", and
## whatever it answers is what gets reported -- the console never says the bridge
## closed because it asked.
func _on_bridge_toggled(pressed: bool) -> void:
	if _suppress_writes:
		return
	if pressed:
		_apply_bridge_status(GDExtensionManager.load_extension(Diagnostics.EXTENSION_PATH), false)
		return
	_confirm_bridge.dialog_text = "Unloading the Didi extension closes its endpoint and retires this editor's session. Live tools will report unavailable until it is loaded again.\n\nFile-based tools are unaffected: they never needed the editor."
	_confirm_bridge.popup_centered()


func _on_bridge_close_confirmed() -> void:
	_apply_bridge_status(GDExtensionManager.unload_extension(Diagnostics.EXTENSION_PATH), true)


func _on_bridge_close_cancelled() -> void:
	# The switch moved when it was clicked; the state did not.
	_suppress_writes = true
	_bridge_switch.button_pressed = _bridge != Bridge.NOT_LOADED
	_suppress_writes = false


func _apply_bridge_status(status: int, closing: bool) -> void:
	var message := _bridge_status_text(status, closing)
	_bridge_result.text = message
	if status == GDExtensionManager.LOAD_STATUS_OK:
		Log.info(message)
		_bridge_result.add_theme_color_override("font_color", _dim())
	else:
		Log.warn(message)
		_bridge_result.add_theme_color_override("font_color", _lamp_colour(Lamp.AMBER))
	_refresh()


func _bridge_status_text(status: int, closing: bool) -> String:
	match status:
		GDExtensionManager.LOAD_STATUS_OK:
			return "Bridge closed." if closing else "Bridge opened."
		GDExtensionManager.LOAD_STATUS_NEEDS_RESTART:
			return "Godot cannot do that without a restart. The bridge changes state the next time this editor starts."
		GDExtensionManager.LOAD_STATUS_ALREADY_LOADED:
			return "The extension is already loaded."
		GDExtensionManager.LOAD_STATUS_NOT_LOADED:
			return "The extension is not loaded."
		_:
			return "Godot refused: the extension could not be %s. The Output panel carries the reason." % ("unloaded" if closing else "loaded")


func _show_failure(message: String) -> void:
	_failure.dialog_text = message
	_failure.popup_centered()


func _on_toast_expired() -> void:
	_toast.visible = false


func _show_toast(message: String) -> void:
	_toast.text = message
	_toast.visible = true
	_toast_timer.start()
