@tool
extends RefCounted

## The launch configuration an MCP client needs in order to run Didi against
## this project.
##
## This is where the console's configuration control actually lands. Didi's
## server is a process the *client* starts, so the arguments it starts with --
## the project root, the log level, whether destructive tools ask first -- are
## decided by a client configuration file and nothing else. Generating that file
## from the editor, with the project path already correct and the executable
## already located, removes the step people most often get wrong.

const Settings := preload("res://addons/didi/didi_settings.gd")

## Clients this can write configuration for. The shape is the same for all of
## them (`mcpServers`), which is what the Quickstart documents for each.
enum Client { CLAUDE_CODE, CURSOR, CLAUDE_DESKTOP, VS_CODE }

const CLIENT_NAMES := {
	Client.CLAUDE_CODE: "Claude Code",
	Client.CURSOR: "Cursor",
	Client.CLAUDE_DESKTOP: "Claude Desktop",
	Client.VS_CODE: "VS Code (Cline / Roo Code)",
}

## Where each client reads the configuration from. Two of these are inside the
## project, which is why the console can offer to write them; the other two are
## user-level files this addon deliberately does not touch.
const CLIENT_DESTINATIONS := {
	Client.CLAUDE_CODE: ".mcp.json in the project root",
	Client.CURSOR: ".cursor/mcp.json in the project root",
	Client.CLAUDE_DESKTOP: "claude_desktop_config.json in your Claude application data directory",
	Client.VS_CODE: "your VS Code MCP settings",
}

## The project-relative file the console can write, for the clients that read
## one from inside the project. Absent for the rest.
const CLIENT_PROJECT_FILE := {
	Client.CLAUDE_CODE: "res://.mcp.json",
	Client.CURSOR: "res://.cursor/mcp.json",
}


## The absolute path of the project this editor has open, in the form the
## `--project` argument expects.
static func project_root() -> String:
	return ProjectSettings.globalize_path("res://").rstrip("/")


static func executable_name() -> String:
	return "didi.exe" if OS.get_name() == "Windows" else "didi"


## Where Didi's server binary is, or "" when it cannot be found.
##
## The configured path wins outright, including when it is wrong: a person who
## typed a path wants to see that path fail, not to have a different binary
## quietly substituted for it.
static func resolve_executable() -> String:
	var configured := str(Settings.get_value(Settings.SERVER_EXECUTABLE)).strip_edges()
	if not configured.is_empty():
		return configured
	return detect_executable()


static var _detected := ""
static var _detected_valid := false

## A best guess at the server binary, from the places it is normally installed
## or built. Pure filesystem lookup -- nothing here starts a process.
##
## Cached, because the sweep walks every directory on PATH and the console asks
## for this every time it repaints. `forget_detection` drops the cache for the
## cases where the answer can have changed.
static func detect_executable() -> String:
	if not _detected_valid:
		_detected = ""
		for candidate in _candidates():
			if FileAccess.file_exists(candidate):
				_detected = candidate
				break
		_detected_valid = true
	return _detected


static func forget_detection() -> void:
	_detected_valid = false


## Where to look for the binary, and deliberately where not to.
##
## Nothing inside the project is a candidate. A path under `res://` is a path
## Didi's own file tools can write to, so a binary found there would be a binary
## an assistant could have put there -- and Diagnostics runs the located binary
## to ask its version. Keeping `res://` out of automatic detection means the only
## way an in-project binary is ever run is that a person browsed to it and chose
## it, which is a decision rather than an accident.
static func _candidates() -> PackedStringArray:
	var name := executable_name()
	var root := project_root()
	var parent := root.get_base_dir()
	var candidates := PackedStringArray()

	# A contributor's build tree, beside a project that lives inside the repo.
	for relative in ["build/Release", "build", "build-ninja"]:
		candidates.append(parent.path_join(relative).path_join(name))
	# Anything already on PATH. Splitting the variable keeps this a filesystem
	# question; asking the shell would mean starting a process to answer it.
	var separator := ";" if OS.get_name() == "Windows" else ":"
	for directory in OS.get_environment("PATH").split(separator, false):
		candidates.append(directory.strip_edges().simplify_path().path_join(name))
	return candidates


## Whether this machine's filesystem calls two paths that differ only in case
## the same file.
##
## NTFS folds, and so does the default macOS volume: APFS is case-insensitive
## and case-preserving there. Linux and the other Unix filesystems do not. Both
## comparisons below asked `OS.get_name() == "Windows"` and meant this, which is
## right for two platforms out of three and wrong for the one that shares the
## behaviour it was written for (#655). The C++ side settled the same question
## in #546 by taking the on-disk spelling from `std::filesystem::canonical`
## rather than trusting the argument.
##
## This is really a property of the volume rather than of the platform, so a
## case-sensitive APFS volume folds here when it does not on disk. That is the
## safe direction for both callers: the Peers list calls one session the same
## project as another, and the binary notice is shown rather than withheld.
static func filesystem_folds_case() -> bool:
	return OS.get_name() == "Windows" or OS.get_name() == "macOS"


## A path in the form this machine's filesystem compares. Descriptors record the
## platform's own separators and the editor reports forward slashes, so a
## textual comparison has to normalise before it means anything.
static func comparable_path(path: String) -> String:
	var value := path.replace("\\", "/").rstrip("/")
	if filesystem_folds_case():
		return value.to_lower()
	return value


## Whether a chosen path lives inside this project.
##
## A person may legitimately keep the binary beside the addon; the console says
## so plainly rather than refusing, because the choice was theirs to make.
static func is_inside_project(path: String) -> bool:
	if path.is_empty():
		return false
	return comparable_path(path).begins_with(comparable_path(project_root()) + "/")


## The arguments the client should launch the server with.
##
## `--project` is first and never optional: Didi refuses to start without an
## explicit project root, so a configuration without it is a configuration that
## exits before it serves anything.
static func arguments() -> PackedStringArray:
	var args := PackedStringArray(["--project", project_root()])
	args.append("--log-level")
	args.append(Settings.client_log_level())
	var pipe := Settings.client_pipe_name()
	if not pipe.is_empty():
		args.append("--pipe-name")
		args.append(pipe)
	if Settings.client_skip_confirmations():
		args.append("--yolo")
	return args


## The configuration block, ready to paste or write.
##
## `executable` is passed in rather than resolved here so the panel can show the
## configuration for a path the person is still editing, and so a caller that
## has no executable yet gets a block with an obvious placeholder in it instead
## of a silently broken one.
static func to_json(executable: String) -> String:
	var command := executable if not executable.is_empty() else "PATH/TO/" + executable_name()
	var block := {
		"mcpServers": {
			"didi": {
				"command": command,
				"args": Array(arguments()),
			}
		}
	}
	# sort_keys off so the block reads command-then-args, the order the Quickstart
	# shows and the order a person checks it in.
	return JSON.stringify(block, "  ", false) + "\n"


static func client_name(client: int) -> String:
	return str(CLIENT_NAMES.get(client, "MCP client"))


static func destination(client: int) -> String:
	return str(CLIENT_DESTINATIONS.get(client, ""))


## The file inside the project this client reads, or "" when it reads a
## user-level file instead. The console only offers to write the former.
static func project_file(client: int) -> String:
	return str(CLIENT_PROJECT_FILE.get(client, ""))


## Writes the configuration to the project file for `client`.
##
## Returns an empty string on success, or a sentence explaining the refusal.
## Merging into an existing file is deliberately not attempted: the file may
## carry other servers and other people's edits, and a console that silently
## rewrote it would be the wrong kind of helpful. The caller shows what is
## there and asks first.
static func write_project_file(client: int, executable: String) -> String:
	var target := project_file(client)
	if target.is_empty():
		return "%s reads %s, which is outside this project." % [client_name(client), destination(client)]
	var directory := target.get_base_dir()
	if not DirAccess.dir_exists_absolute(directory):
		if DirAccess.make_dir_recursive_absolute(directory) != OK:
			return "Could not create %s." % directory
	var file := FileAccess.open(target, FileAccess.WRITE)
	if file == null:
		return "Could not open %s for writing (%s)." % [target, error_string(FileAccess.get_open_error())]
	file.store_string(to_json(executable))
	file.close()
	return ""
