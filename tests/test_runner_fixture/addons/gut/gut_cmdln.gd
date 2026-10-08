extends SceneTree

# A stand-in for GUT's command-line runner, for the Q9 block in
# tests/test_runner.ps1. It follows the contract project_run_tests relies on:
# -gdir, -ginclude_subdirs, -gtest and -gjunit_xml_file in; GUT's JUnit shape
# out; exit 1 when a test failed and 0 otherwise; a test file that does not
# parse left out of the report, with Godot's own SCRIPT ERROR printed for it.
# CI has no GUT, so this is what proves Didi's side of that contract on every
# engine line. The real GUT and GdUnit4 are run the same way by hand.
#
# It needs no import: nothing here or in the tests declares a class_name.

var _dirs := []
var _files := []
var _junit := ""
var _include_subdirs := false


func _initialize() -> void:
	for argument in OS.get_cmdline_args():
		if argument.begins_with("-gdir="):
			_dirs.append_array(Array(argument.substr(6).split(",")))
		elif argument.begins_with("-gtest="):
			_files.append_array(Array(argument.substr(7).split(",")))
		elif argument.begins_with("-gjunit_xml_file="):
			_junit = argument.substr(17)
		elif argument == "-ginclude_subdirs":
			_include_subdirs = true
	for dir in _dirs:
		_collect(dir)
	var suites := []
	var failures := 0
	var total := 0
	for path in _files:
		# A file that does not parse prints its SCRIPT ERROR here and comes
		# back null, and GUT leaves it out of the report the same way.
		var script = load(path)
		if script == null:
			continue
		var cases := []
		for method in script.get_script_method_list():
			var test_name: String = method["name"]
			if not test_name.begins_with("test_"):
				continue
			var test = script.new()
			test.call(test_name)
			cases.append({"name": test_name, "failure": test.failure})
			total += 1
			if test.failure != "":
				failures += 1
		suites.append({"path": path, "cases": cases})
	if not _write(suites, failures, total):
		quit(1)
		return
	quit(1 if failures > 0 else 0)


func _collect(dir: String) -> void:
	var listing := DirAccess.open(dir)
	if listing == null:
		return
	for file_name in listing.get_files():
		if file_name.begins_with("test_") and file_name.ends_with(".gd"):
			_files.append(dir.path_join(file_name))
	if _include_subdirs:
		for sub in listing.get_directories():
			_collect(dir.path_join(sub))


func _write(suites: Array, failures: int, total: int) -> bool:
	var xml := "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
	xml += "<testsuites name=\"GutTests\" failures=\"%d\" tests=\"%d\" >\n" % [failures, total]
	for suite in suites:
		var name: String = suite["path"].trim_prefix("res://")
		var suite_failures := 0
		for test in suite["cases"]:
			if test["failure"] != "":
				suite_failures += 1
		xml += "  <testsuite name=\"%s\" tests=\"%d\" failures=\"%d\" skipped=\"0\" time=\"0\" >\n" % [
			name, suite["cases"].size(), suite_failures]
		for test in suite["cases"]:
			var status := "fail" if test["failure"] != "" else "pass"
			xml += "      <testcase name=\"%s\" assertions=\"1\" status=\"%s\" classname=\"%s\" time=\"0\" >\n" % [
				test["name"], status, name]
			if test["failure"] != "":
				xml += "      <failure message=\"failed\"><![CDATA[%s]]></failure>" % test["failure"]
			xml += "</testcase>\n"
		xml += "  </testsuite>\n"
	xml += "</testsuites>\n"
	var file := FileAccess.open(_junit, FileAccess.WRITE)
	if file == null:
		push_error("Cannot write the report to " + _junit)
		return false
	file.store_string(xml)
	return true
