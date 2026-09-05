@tool
extends RefCounted

## What Didi's live bridge is actually doing, read from the session descriptors
## the extension publishes.
##
## A descriptor is written when a Godot process binds its IPC endpoint and is
## retired when that process shuts down cleanly, so the set of descriptors on
## disk is the closest thing to a live registry of who an MCP client can reach.
##
## The token is the one field this file refuses to carry. A descriptor on disk
## contains the shared secret that authenticates a client to that session, and a
## console has no use for it: not to display, not to copy, not to write into a
## report. `_adopt` copies the fields it names and drops everything else, so a
## secret cannot reach the panel by being added to the descriptor later.

const DESCRIPTOR_DIRECTORY_NAME := "didi-sessions"

## One published session, minus its secret.
class Session extends RefCounted:
	var session_id: String = ""
	var pid: int = 0
	var kind: String = ""
	var project_path: String = ""
	var endpoint: String = ""
	var started_at_ms: int = 0
	var protocol_version: String = ""
	var source_path: String = ""

	## Editor sessions expose the live editor; game sessions expose a running
	## project. The distinction decides which tools can reach this process at
	## all, so the panel never conflates them.
	func is_editor() -> bool:
		return kind == "editor"

	func age_seconds() -> float:
		if started_at_ms <= 0:
			return 0.0
		return max(0.0, (Time.get_unix_time_from_system() * 1000.0 - float(started_at_ms)) / 1000.0)

	## Whether this is the session published by the process asking.
	##
	## There is deliberately no "is that other process still alive?" here.
	## `OS.is_process_running` answers only for processes Godot itself started,
	## and returns false for every other pid -- including live ones. A panel that
	## called it would label running editors as gone, which is worse than saying
	## nothing. What a descriptor proves is that a process published it and has
	## not retired it; that is the claim this makes.
	func is_own_process() -> bool:
		return pid == OS.get_process_id()

	## A short handle for the panel. The full 32-character id is exact and
	## unreadable; the first eight distinguish sessions on one machine.
	func short_id() -> String:
		return session_id.substr(0, 8) if session_id.length() >= 8 else session_id


	## How long ago this session was published, in words.
	func age_phrase() -> String:
		var seconds := int(age_seconds())
		if seconds < 90:
			return "%d s ago" % seconds
		if seconds < 5400:
			return "%d min ago" % (seconds / 60)
		if seconds < 172800:
			return "%d h ago" % (seconds / 3600)
		return "%d days ago" % (seconds / 86400)


## Every session this machine is currently publishing, newest first.
##
## Returns an Array of Session. It is deliberately untyped: a typed array over a
## nested class is not something every 4.x engine accepts, and this addon has to
## load on the 4.5 floor it declares.
static func discover() -> Array:
	var found: Array = []
	var seen := {}
	for directory in descriptor_directories():
		var handle := DirAccess.open(directory)
		if handle == null:
			continue
		for name in handle.get_files():
			if not name.ends_with(".json"):
				continue
			var path := directory.path_join(name)
			if seen.has(path):
				continue
			seen[path] = true
			var session := _read(path)
			if session != null:
				found.append(session)
	found.sort_custom(func(a, b) -> bool: return a.started_at_ms > b.started_at_ms)
	return found


## The session published by this very editor process, if there is one.
##
## Matching on our own pid is exact: it cannot pick up another editor, a running
## game, or a descriptor left behind by a process that has gone.
static func own(sessions: Array) -> Session:
	var self_pid := OS.get_process_id()
	for session in sessions:
		if session.pid == self_pid:
			return session
	return null


## The directories a descriptor could be in, in the order the native code
## resolves them.
##
## This mirrors `resolveSessionDescriptorDirectory` in the C++ runtime. It is a
## deliberate duplication rather than a lookup, because there is nothing to ask:
## the extension does not publish where it published. Mirroring it as a list of
## candidates rather than a single answer is what keeps a divergence from
## becoming a blank panel -- every candidate is scanned, and a descriptor found
## anywhere is a descriptor found.
static func descriptor_directories() -> PackedStringArray:
	var directories := PackedStringArray()
	var configured := OS.get_environment("DIDI_SESSION_DIR")
	if not configured.is_empty():
		directories.append(configured.simplify_path())

	if OS.get_name() == "Windows":
		# GetTempPath, which is what std::filesystem::temp_directory_path calls,
		# reads these three in this order.
		for variable in ["TMP", "TEMP", "USERPROFILE"]:
			var root := OS.get_environment(variable)
			if not root.is_empty():
				_append_unique(directories, root.simplify_path().path_join(DESCRIPTOR_DIRECTORY_NAME))
	else:
		var runtime_dir := OS.get_environment("XDG_RUNTIME_DIR")
		if not runtime_dir.is_empty():
			_append_unique(directories, runtime_dir.simplify_path().path_join(DESCRIPTOR_DIRECTORY_NAME))
		# POSIX descriptor directories carry the effective uid in their name and
		# GDScript cannot ask for one, so every per-user directory under each
		# temporary root is a candidate. Directories belonging to other users are
		# unreadable, which removes them without a permission check here.
		for root in _posix_temporary_roots():
			for name in _subdirectories_named(root, DESCRIPTOR_DIRECTORY_NAME + "-"):
				_append_unique(directories, name)
	return directories


static func _posix_temporary_roots() -> PackedStringArray:
	var roots := PackedStringArray()
	for variable in ["TMPDIR", "TMP", "TEMP", "TEMPDIR"]:
		var value := OS.get_environment(variable)
		if not value.is_empty():
			_append_unique(roots, value.simplify_path())
	_append_unique(roots, "/tmp")
	return roots


static func _subdirectories_named(root: String, prefix: String) -> PackedStringArray:
	var matches := PackedStringArray()
	var handle := DirAccess.open(root)
	if handle == null:
		return matches
	for name in handle.get_directories():
		if name.begins_with(prefix):
			matches.append(root.path_join(name))
	return matches


static func _append_unique(list: PackedStringArray, value: String) -> void:
	if list.has(value):
		return
	list.append(value)


static func _read(path: String) -> Session:
	var file := FileAccess.open(path, FileAccess.READ)
	if file == null:
		return null
	# A descriptor is a few hundred bytes. Anything larger is not one, and
	# reading it would only be a way to spend memory on a file we will reject.
	var length := file.get_length()
	if length == 0 or length > 65536:
		file.close()
		return null
	var text := file.get_as_text()
	file.close()

	var parsed: Variant = JSON.parse_string(text)
	if typeof(parsed) != TYPE_DICTIONARY:
		return null
	return _adopt(parsed as Dictionary, path)


static func _adopt(raw: Dictionary, path: String) -> Session:
	if int(raw.get("schema_version", 0)) != 1:
		return null
	var session := Session.new()
	session.session_id = str(raw.get("session_id", ""))
	session.pid = int(raw.get("pid", 0))
	session.kind = str(raw.get("kind", ""))
	session.project_path = str(raw.get("project_path", ""))
	session.endpoint = str(raw.get("endpoint", ""))
	session.started_at_ms = int(raw.get("started_at_ms", 0))
	session.protocol_version = str(raw.get("protocol_version", ""))
	session.source_path = path
	if session.session_id.is_empty() or session.pid <= 0:
		return null
	if session.kind != "editor" and session.kind != "game":
		return null
	return session
