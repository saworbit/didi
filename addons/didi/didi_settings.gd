@tool
extends RefCounted

## Didi's editor-side preferences.
##
## These live in Godot's own `EditorSettings`, which is stored per user in the
## editor's configuration directory rather than anywhere inside `res://`. That
## placement is the point. Didi's file tools are project-scoped -- every path
## they accept is resolved and checked against the project root -- so settings
## kept outside the project cannot be rewritten by a tool call. A preference
## file inside `res://` would be editable by the very agent the preferences are
## meant to govern.
##
## Everything here is also visible under Editor Settings, so the console is a
## convenience rather than the only way in.

const PREFIX := "didi/"

const AUTO_REFRESH := "didi/console/auto_refresh"
const REFRESH_SECONDS := "didi/console/refresh_interval_seconds"
const SHOW_TECHNICAL := "didi/console/show_technical_detail"
const VERIFY_BY_RUNNING := "didi/console/verify_binary_by_running"
const LOG_FOLLOW := "didi/console/log_follow"
const SERVER_EXECUTABLE := "didi/server/executable_path"
const CLIENT_LOG_LEVEL := "didi/client/log_level"
const CLIENT_SKIP_CONFIRMATIONS := "didi/client/skip_confirmations"
const CLIENT_PIPE_NAME := "didi/client/pipe_name"

const LOG_LEVELS := ["DEBUG", "INFO", "WARN", "ERROR", "NONE"]

const _DEFAULTS := {
	AUTO_REFRESH: true,
	REFRESH_SECONDS: 2.0,
	SHOW_TECHNICAL: false,
	VERIFY_BY_RUNNING: false,
	LOG_FOLLOW: true,
	SERVER_EXECUTABLE: "",
	CLIENT_LOG_LEVEL: "INFO",
	CLIENT_SKIP_CONFIRMATIONS: false,
	CLIENT_PIPE_NAME: "",
}


## Declares every setting with the type and range the Editor Settings dialog
## needs to draw a real widget for it. Called once when the plugin enters the
## tree; calling it again is harmless.
static func ensure_registered() -> void:
	var settings := _settings()
	if settings == null:
		return
	for key in _DEFAULTS:
		if not settings.has_setting(key):
			settings.set_setting(key, _DEFAULTS[key])
		settings.set_initial_value(key, _DEFAULTS[key], false)
	settings.add_property_info({
		"name": REFRESH_SECONDS,
		"type": TYPE_FLOAT,
		"hint": PROPERTY_HINT_RANGE,
		"hint_string": "0.5,30.0,0.5",
	})
	settings.add_property_info({
		"name": SERVER_EXECUTABLE,
		"type": TYPE_STRING,
		"hint": PROPERTY_HINT_GLOBAL_FILE,
	})
	settings.add_property_info({
		"name": CLIENT_LOG_LEVEL,
		"type": TYPE_STRING,
		"hint": PROPERTY_HINT_ENUM,
		"hint_string": ",".join(LOG_LEVELS),
	})


static func get_value(key: String) -> Variant:
	var settings := _settings()
	if settings == null or not settings.has_setting(key):
		return _DEFAULTS.get(key, null)
	return settings.get_setting(key)


static func set_value(key: String, value: Variant) -> void:
	var settings := _settings()
	if settings == null:
		return
	settings.set_setting(key, value)


static func flag(key: String) -> bool:
	return bool(get_value(key))


static func default_value(key: String) -> Variant:
	return _DEFAULTS.get(key, null)


static func refresh_seconds() -> float:
	return clampf(float(get_value(REFRESH_SECONDS)), 0.5, 30.0)


## The log level the generated client configuration will ask the server for.
static func client_log_level() -> String:
	var level := str(get_value(CLIENT_LOG_LEVEL)).to_upper()
	return level if LOG_LEVELS.has(level) else "INFO"


## Whether the generated client configuration will pass `--yolo`.
##
## This is a launch argument and nothing else. Didi accepts YOLO only from the
## arguments the person starting the process chose, never from anything a tool
## call can reach, and this console does not change that: it writes the flag
## into a configuration file for a human to review and a client to launch with.
static func client_skip_confirmations() -> bool:
	return flag(CLIENT_SKIP_CONFIRMATIONS)


## An explicit endpoint name for the generated configuration, or "" for none.
##
## Only useful when something else on the machine is already using the default
## name, so it is off by default and empty means absent rather than blank.
static func client_pipe_name() -> String:
	return str(get_value(CLIENT_PIPE_NAME)).strip_edges()


static func _settings() -> EditorSettings:
	if not Engine.is_editor_hint():
		return null
	return EditorInterface.get_editor_settings()
