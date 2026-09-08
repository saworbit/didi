extends Node

# Two deliberately broken references, one of each kind, so the live
# verification pass in project_audit_assets always has something real to check
# against the running editor.
#
# The uid is well formed and unregistered: the engine must answer that it does
# not know it. The path is absent: the engine must answer that it cannot load
# it. Both turn an offline guess into a confirmed finding rather than clearing
# it. load() rather than preload() on purpose, so the script still compiles.
const DIDI_AUDIT_UID_PROBE := "uid://bogusbogusbogus"


func audit_uid_probe() -> String:
	return DIDI_AUDIT_UID_PROBE


func audit_missing_path_probe() -> Resource:
	return load("res://uid_audit_probe_missing.tres")
