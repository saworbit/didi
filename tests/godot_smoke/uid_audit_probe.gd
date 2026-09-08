extends Node

# A UID reference that no project file records, so project_audit_assets always
# has one unresolved uid:// finding for the live verification pass to check.
# It is well formed and deliberately unregistered: an attached editor must
# answer that it does not know it, which is what turns the offline guess into a
# confirmed finding rather than clearing it.
const DIDI_AUDIT_UID_PROBE := "uid://bogusbogusbogus"


func audit_uid_probe() -> String:
	return DIDI_AUDIT_UID_PROBE
