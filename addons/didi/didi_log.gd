@tool
extends RefCounted

## What the console has watched happen, and what the project's last run wrote.
##
## Two sources, because there are two and no more. Didi's own native logging goes
## to the process's standard error, which a Godot editor started from a desktop
## shortcut does not have anywhere to show -- there is no file to tail and no
## API that hands it over. Pretending otherwise would be a log view that is
## always empty and never says why.
##
## So the console records what it can prove: every state change it observes and
## every action taken through it, timestamped. Alongside that it reads the log
## Godot writes for a *run* of the project, which is where a game-side Didi
## session and everything the game printed actually end up.

## Godot writes this when the project runs. The editor process writes no log of
## its own, which is why it is not offered as a third source.
const RUN_LOG := "user://logs/godot.log"

## Enough history to answer "what happened while I was away" without becoming a
## memory leak in a long editor session.
const CAPACITY := 400

## The tail of a run log worth reading. A long play session can produce
## megabytes, and the end is the part anyone wants.
const RUN_LOG_TAIL_BYTES := 262144

enum Level { INFO, WARN, ERROR }

class Record extends RefCounted:
	var unix_time: float = 0.0
	var level: int = Level.INFO
	var text: String = ""

	func stamp() -> String:
		return Time.get_time_string_from_unix_time(int(unix_time))

	func level_word() -> String:
		match level:
			Level.ERROR:
				return "error"
			Level.WARN:
				return "warn"
			_:
				return "info"


# Static so the history survives the console being rebuilt -- a plugin toggled
# off and on again, or an editor theme change -- which is exactly when someone
# is most likely to want to look back at what just happened.
static var _records: Array = []


static func note(level: int, text: String) -> void:
	var record := Record.new()
	record.unix_time = Time.get_unix_time_from_system()
	record.level = level
	record.text = text
	_records.append(record)
	if _records.size() > CAPACITY:
		_records = _records.slice(_records.size() - CAPACITY)


static func info(text: String) -> void:
	note(Level.INFO, text)


static func warn(text: String) -> void:
	note(Level.WARN, text)


static func error(text: String) -> void:
	note(Level.ERROR, text)


static func records() -> Array:
	return _records


static func clear() -> void:
	_records = []


## Whether a run log exists to read at all.
static func run_log_exists() -> bool:
	return FileAccess.file_exists(RUN_LOG)


static func run_log_path() -> String:
	return ProjectSettings.globalize_path(RUN_LOG)


## The tail of the log Godot wrote for the last run of this project.
##
## Returns an Array of Record. A run in progress is holding this file open and
## appending to it; reading it anyway is the point, and a read that fails
## reports as one line saying so rather than as an empty view.
static func read_run_log() -> Array:
	if not run_log_exists():
		return []
	var file := FileAccess.open(RUN_LOG, FileAccess.READ)
	if file == null:
		var failure := Record.new()
		failure.unix_time = Time.get_unix_time_from_system()
		failure.level = Level.WARN
		failure.text = "Could not read %s (%s)" % [
			run_log_path(), error_string(FileAccess.get_open_error())]
		return [failure]
	var length := file.get_length()
	if length > RUN_LOG_TAIL_BYTES:
		file.seek(length - RUN_LOG_TAIL_BYTES)
	var text := file.get_as_text()
	file.close()

	var modified := FileAccess.get_modified_time(RUN_LOG)
	var out: Array = []
	var carried := Level.INFO
	for line in text.split("\n"):
		var raw := line as String
		var trimmed := raw.strip_edges()
		if trimmed.is_empty():
			continue
		var record := Record.new()
		# The file carries no per-line timestamp, so every line is stamped with
		# the file's own modification time. That is honest about what is known:
		# these lines are from that run, not from this second.
		record.unix_time = float(modified)
		# Godot writes an error as a line and then indents its location under it.
		# A continuation classified on its own reads as ordinary output, and
		# filtering to errors would then hide the line naming the file and line
		# number -- the half anyone actually wants. Continuations inherit.
		if raw.begins_with(" ") or raw.begins_with("\t"):
			record.level = carried
		else:
			carried = _classify(trimmed)
			record.level = carried
		record.text = trimmed
		out.append(record)
	return out


## Godot's log lines carry their severity as a prefix rather than a field.
static func _classify(line: String) -> int:
	var upper := line.to_upper()
	if upper.begins_with("ERROR") or upper.begins_with("SCRIPT ERROR") or upper.begins_with("USER ERROR"):
		return Level.ERROR
	if upper.begins_with("WARNING") or upper.begins_with("USER WARNING"):
		return Level.WARN
	return Level.INFO
