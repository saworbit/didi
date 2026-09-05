@tool
extends RefCounted

## The checks that answer "why can my assistant not see my project?" without
## anyone having to read a log.
##
## Every check reports one of three states and, whichever it reports, says what
## it looked at. A check that passes is as informative as one that fails: it
## names the path or the pid it found, so a person comparing two machines can
## see where they differ.

const ClientConfig := preload("res://addons/didi/didi_client_config.gd")
const Sessions := preload("res://addons/didi/didi_session.gd")

const EXTENSION_PATH := "res://addons/didi/didi.gdextension"
const LIBRARY_DIRECTORY := "res://addons/didi/bin/"

enum State { OK, WARN, FAIL }

class Check extends RefCounted:
	var title: String = ""
	var state: int = State.OK
	var detail: String = ""

	static func make(check_title: String, check_state: int, check_detail: String) -> Check:
		var check := Check.new()
		check.title = check_title
		check.state = check_state
		check.detail = check_detail
		return check


## The library filename this platform's engine will look for, taken from the
## same platform split the .gdextension file uses.
static func library_name() -> String:
	match OS.get_name():
		"Windows":
			return "didi_extension.dll"
		"macOS":
			return "libdidi_extension.dylib"
		_:
			return "libdidi_extension.so"


## Runs every check. `allow_process` gates the one check that starts a process,
## so a panel refreshing on a timer never spawns anything and a person pressing
## a button gets the full answer.
##
## Returns an Array of Check.
static func run(allow_process: bool) -> Array:
	var checks: Array = []
	var sessions := Sessions.discover()
	var own: Sessions.Session = Sessions.own(sessions)

	var library := LIBRARY_DIRECTORY.path_join(library_name())
	if FileAccess.file_exists(library):
		checks.append(Check.make("Extension binary", State.OK, library))
	else:
		checks.append(Check.make("Extension binary", State.FAIL,
			"%s is missing. Build Didi for this platform, or copy the released library into %s."
			% [library, LIBRARY_DIRECTORY]))

	if GDExtensionManager.is_extension_loaded(EXTENSION_PATH):
		checks.append(Check.make("Extension loaded", State.OK, EXTENSION_PATH))
	else:
		checks.append(Check.make("Extension loaded", State.FAIL,
			"Godot has not loaded %s. It is loaded at editor start, so a library added since then needs a restart."
			% EXTENSION_PATH))

	if own != null:
		checks.append(Check.make("Live bridge", State.OK,
			"Session %s published for this editor (pid %d)." % [own.short_id(), own.pid]))
	else:
		checks.append(Check.make("Live bridge", State.FAIL,
			"This editor has published no session, so live tools have nothing to reach. Check the Output panel for GDEXT_IPC errors."))

	var readable := PackedStringArray()
	for directory in Sessions.descriptor_directories():
		if DirAccess.open(directory) != null:
			readable.append(directory)
	if readable.is_empty():
		checks.append(Check.make("Session directory", State.WARN,
			"None of the directories a session could be published in exist yet: %s"
			% ", ".join(Sessions.descriptor_directories())))
	else:
		checks.append(Check.make("Session directory", State.OK, ", ".join(readable)))

	var executable := ClientConfig.resolve_executable()
	if executable.is_empty():
		checks.append(Check.make("Server executable", State.WARN,
			"No didi executable found. Set one under Connect, or in Editor Settings."))
	elif not FileAccess.file_exists(executable):
		checks.append(Check.make("Server executable", State.FAIL,
			"%s does not exist." % executable))
	else:
		checks.append(Check.make("Server executable", State.OK, executable))
		if allow_process:
			checks.append(_version_check(executable))

	var other := 0
	for session in sessions:
		if own == null or session.pid != own.pid:
			other += 1
	if other > 0:
		checks.append(Check.make("Other sessions", State.OK,
			"%d other Didi session%s published on this machine." % [other, " is" if other == 1 else "s are"]))

	return checks


## Asks the located binary what it is. This is the only check that starts a
## process, and it is the only way to tell a Didi executable from a file with
## the right name.
static func _version_check(executable: String) -> Check:
	var output: Array = []
	var code := OS.execute(executable, ["--version"], output, true)
	var text := ""
	for line in output:
		text += str(line)
	text = text.strip_edges()
	if code != 0:
		return Check.make("Server responds", State.FAIL,
			"%s exited with status %d. %s" % [executable, code, text])
	if not text.begins_with("didi"):
		return Check.make("Server responds", State.WARN,
			"%s ran but did not identify itself as Didi: %s" % [executable, text])
	return Check.make("Server responds", State.OK, text)


## The same checks as one block of text, for pasting into an issue.
##
## Carries the machine's engine version and platform because the first question
## on any report is which engine it came from. It carries no token: session
## secrets never leave the descriptor they were written in.
static func report_text(checks: Array) -> String:
	var engine := Engine.get_version_info()
	var lines := PackedStringArray()
	lines.append("Didi console report")
	lines.append("Plugin: %s" % plugin_version())
	lines.append("Engine: %s (%s)" % [str(engine.get("string", "unknown")), OS.get_name()])
	lines.append("Project: %s" % ClientConfig.project_root())
	lines.append("")
	for check in checks:
		lines.append("[%s] %s: %s" % [state_word(check.state), check.title, check.detail])
	return "\n".join(lines) + "\n"


static func state_word(state: int) -> String:
	match state:
		State.OK:
			return "ok"
		State.WARN:
			return "warn"
		_:
			return "fail"


static func plugin_version() -> String:
	var config := ConfigFile.new()
	if config.load("res://addons/didi/plugin.cfg") != OK:
		return "unknown"
	return str(config.get_value("plugin", "version", "unknown"))
