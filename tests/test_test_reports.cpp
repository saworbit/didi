#include "didi/offline/test_reports.hpp"
#include "didi/mcp/tool_registry.hpp"
#include "didi/runtime/scenario_runner.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#define ASSERT_TRUE(cond) if (!(cond)) throw std::runtime_error("Assertion failed: " #cond);
#define ASSERT_EQ(a, b) ASSERT_TRUE((a) == (b))

void registerTest(const std::string& name, std::function<void()> fn);

namespace {

namespace fs = std::filesystem;
using didi::json;
using didi::offline::InstalledTestFramework;
using didi::offline::TestFramework;
using didi::offline::TestRunFacts;

// What GUT 9.7.1 and GdUnit4 6.2.2 wrote for the same three tests, one of them
// wrong on purpose. The shape was the same on 4.5.1, 4.6.2 and 4.7.2 apart
// from the timings; these are 4.7.2's.
const char* kGutFailing = R"(<?xml version="1.0" encoding="UTF-8"?>
<testsuites name="GutTests" failures="1" tests="3" >
  <testsuite name="test/test_calculator.gd" tests="3" failures="1" skipped="0" time="0.000734" >
      <testcase name="test_adds" assertions="1" status="pass" classname="test/test_calculator.gd" time="0.000216" >
      </testcase>
      <testcase name="test_adds_negatives" assertions="1" status="pass" classname="test/test_calculator.gd" time="0.000151" >
      </testcase>
      <testcase name="test_is_wrong_on_purpose" assertions="1" status="fail" classname="test/test_calculator.gd" time="0.000367" >
      <failure message="failed"><![CDATA[[4] expected to equal [5]:  two and two
            at line 10]]></failure></testcase>
  </testsuite>
</testsuites>)";

const char* kGdUnitFailing = R"(<?xml version="1.0" encoding="UTF-8" ?>
<testsuites id="2026-10-09" name="report_1" tests="3" failures="1" skipped="0" flaky="0" time="0.000">
	<testsuite id="0" name="calculator_test" package="test" timestamp="2026-10-08T20:40:28" hostname="localhost" tests="3" failures="1" errors="0" skipped="0" flaky="0" time="0.017">
		<testcase name="test_adds" classname="calculator_test" time="0.003">
		</testcase>
		<testcase name="test_adds_negatives" classname="calculator_test" time="0.003">
		</testcase>
		<testcase name="test_is_wrong_on_purpose" classname="calculator_test" time="0.003">
			<failure message="FAILED: res://test/calculator_test.gd:10" type="FAILURE">
<![CDATA[
Expecting:
 5
 but was
 4
	at 'test_is_wrong_on_purpose' in res://test/calculator_test.gd:10
]]>
			</failure>
		</testcase>
	</testsuite>
</testsuites>)";

// A test that asserts nothing, a pending one, and a real one.
const char* kGutRisky = R"(<?xml version="1.0" encoding="UTF-8"?>
<testsuites name="GutTests" failures="0" tests="3" >
  <testsuite name="test/test_calculator.gd" tests="3" failures="0" skipped="1" time="0.00057" >
      <testcase name="test_asserts_nothing" assertions="0" status="no asserts" classname="test/test_calculator.gd" time="0.000252" >
      </testcase>
      <testcase name="test_later" assertions="0" status="pending" classname="test/test_calculator.gd" time="0.000182" >
      <skipped message="pending"><![CDATA[not written yet]]></skipped></testcase>
      <testcase name="test_real" assertions="1" status="pass" classname="test/test_calculator.gd" time="0.000136" >
      </testcase>
  </testsuite>
</testsuites>)";

const char* kGutEmpty = R"(<?xml version="1.0" encoding="UTF-8"?>
<testsuites name="GutTests" failures="0" tests="0" >
  </testsuites>)";

// The parse error both frameworks printed for a test file that does not parse.
const char* kParseErrorOutput =
    "SCRIPT ERROR: Parse Error: Unexpected \"Indent\" in class body.\n"
    "   at: GDScript::reload (res://test/test_broken.gd:4)\n"
    "   GDScript backtrace (most recent call first):\n";

std::string withoutFailure(const char* xml) {
    std::string text = xml;
    const auto start = text.find("<testcase name=\"test_is_wrong_on_purpose\"");
    const auto end = text.find("</testcase>", start) + std::string("</testcase>").size();
    return text.erase(start, end - start);
}

InstalledTestFramework gut() { return {TestFramework::gut, "res://addons/gut/gut_cmdln.gd", "9.7.1"}; }
InstalledTestFramework gdunit() {
    return {TestFramework::gdunit4, "res://addons/gdUnit4/bin/GdUnitCmdTool.gd", "6.2.2"};
}

TestRunFacts facts(InstalledTestFramework framework, int exit_code, std::optional<std::string> xml,
                   std::string output = {}) {
    TestRunFacts run;
    run.framework = std::move(framework);
    run.paths = {"res://test"};
    run.exit_code = exit_code;
    run.report_xml = std::move(xml);
    run.output = std::move(output);
    return run;
}

void test_both_frameworks_reports_are_read_test_by_test() {
    const auto gut_report = didi::offline::parseJUnitReport(kGutFailing);
    ASSERT_TRUE(gut_report.isOk());
    const auto& gut_cases = gut_report.value().cases;
    ASSERT_EQ(gut_cases.size(), 3u);
    ASSERT_EQ(gut_cases[0].outcome, "passed");
    ASSERT_EQ(gut_cases[2].outcome, "failed");
    ASSERT_EQ(gut_cases[2].name, "test_is_wrong_on_purpose");
    ASSERT_TRUE(gut_cases[2].message.find("[4] expected to equal [5]:  two and two") != std::string::npos);
    ASSERT_EQ(gut_cases[2].file, "res://test/test_calculator.gd");
    ASSERT_EQ(gut_cases[2].line, 10);

    const auto gdunit_report = didi::offline::parseJUnitReport(kGdUnitFailing);
    ASSERT_TRUE(gdunit_report.isOk());
    const auto& gdunit_cases = gdunit_report.value().cases;
    ASSERT_EQ(gdunit_cases.size(), 3u);
    ASSERT_EQ(gdunit_cases[2].outcome, "failed");
    ASSERT_EQ(gdunit_cases[2].suite, "calculator_test");
    ASSERT_EQ(gdunit_cases[2].file, "res://test/calculator_test.gd");
    ASSERT_EQ(gdunit_cases[2].line, 10);
    ASSERT_TRUE(gdunit_cases[2].message.find("but was") != std::string::npos);

    const auto risky = didi::offline::parseJUnitReport(kGutRisky);
    ASSERT_TRUE(risky.isOk());
    ASSERT_EQ(risky.value().cases[0].outcome, "no_assertions");
    ASSERT_EQ(risky.value().cases[1].outcome, "skipped");
    ASSERT_EQ(risky.value().cases[1].message, "pending\nnot written yet");
    ASSERT_EQ(risky.value().cases[2].outcome, "passed");
}

void test_a_report_that_is_not_xml_is_refused_rather_than_guessed() {
    ASSERT_TRUE(didi::offline::parseJUnitReport("<testsuites><testcase name='x'>").isErr());
    ASSERT_TRUE(didi::offline::parseJUnitReport("<testsuites><testcase name=\"x\"></testsuite>").isErr());
    ASSERT_TRUE(didi::offline::parseJUnitReport("<testsuites><![CDATA[never ends").isErr());
    ASSERT_TRUE(didi::offline::parseJUnitReport("<testcase name=x/>").isErr());
    // Entities are decoded, in attributes and in text.
    const auto decoded = didi::offline::parseJUnitReport(
        "<testsuites><testcase name=\"a &lt;b&gt; &amp; &#65;&#x42;\"><failure message=\"m\">x &quot;y&quot;</failure></testcase></testsuites>");
    ASSERT_TRUE(decoded.isOk());
    ASSERT_EQ(decoded.value().cases[0].name, "a <b> & AB");
    ASSERT_EQ(decoded.value().cases[0].message, "m\nx \"y\"");
    std::string huge(didi::offline::kMaxReportBytes + 1, ' ');
    ASSERT_EQ(didi::offline::parseJUnitReport(huge).error().code, 413);
}

void test_the_verdict_comes_from_the_report() {
    const auto failing = didi::offline::judgeTestRun(facts(gut(), 1, std::string(kGutFailing)));
    ASSERT_EQ(failing.verdict, "fail");
    ASSERT_EQ(failing.reason, "tests_failed");
    ASSERT_EQ(failing.report["counts"]["failed"], 1);
    ASSERT_EQ(failing.report["counts"]["passed"], 2);
    ASSERT_EQ(failing.report["failed_tests"][0], "test/test_calculator.gd :: test_is_wrong_on_purpose");
    ASSERT_EQ(failing.report["tests"][2]["line"], 10);

    const auto passing = didi::offline::judgeTestRun(facts(gut(), 0, withoutFailure(kGutFailing)));
    ASSERT_EQ(passing.verdict, "pass");
    ASSERT_EQ(passing.report["counts"]["passed"], 2);
    ASSERT_TRUE(!passing.report.contains("output_tail"));

    const auto gdunit_failing = didi::offline::judgeTestRun(facts(gdunit(), 100, std::string(kGdUnitFailing)));
    ASSERT_EQ(gdunit_failing.verdict, "fail");
    // 101 is GdUnit4's exit for warnings, which is not a failure.
    const auto warned = didi::offline::judgeTestRun(facts(gdunit(), 101, withoutFailure(kGdUnitFailing)));
    ASSERT_TRUE(warned.verdict != "fail");
}

void test_a_run_that_tested_nothing_never_passes() {
    // GUT exits 0 with no tests; GdUnit4 exits 0 with no tests and no report.
    ASSERT_EQ(didi::offline::judgeTestRun(facts(gut(), 0, std::string(kGutEmpty))).reason, "no_tests");
    const auto none = didi::offline::judgeTestRun(
        facts(gdunit(), 0, std::nullopt, "No test cases found, abort test run!\nExit code: 0\n"));
    ASSERT_EQ(none.verdict, "error");
    ASSERT_EQ(none.reason, "no_tests");
    // GUT exits 0 and writes nothing on a project that was never imported.
    const auto not_imported = didi::offline::judgeTestRun(facts(
        gut(), 0, std::nullopt,
        "ERROR: Some GUT class_names have not been imported.  Please restart the Editor or run godot --headless --import\n"));
    ASSERT_EQ(not_imported.verdict, "error");
    ASSERT_EQ(not_imported.reason, "no_report");
    ASSERT_EQ(not_imported.report["not_imported"], true);
    // Every test skipped or asserting nothing.
    std::string only_risky = kGutRisky;
    only_risky.erase(only_risky.find("      <testcase name=\"test_real\""),
                     only_risky.find("</testcase>", only_risky.find("test_real")) + 11 -
                         only_risky.find("      <testcase name=\"test_real\""));
    const auto unproved = didi::offline::judgeTestRun(facts(gut(), 0, only_risky));
    ASSERT_EQ(unproved.reason, "nothing_proved");
    // One real pass beside them is a pass, and they are counted apart from it.
    const auto mixed = didi::offline::judgeTestRun(facts(gut(), 0, std::string(kGutRisky)));
    ASSERT_EQ(mixed.verdict, "pass");
    ASSERT_EQ(mixed.report["counts"]["no_assertions"], 1);
    ASSERT_EQ(mixed.report["counts"]["skipped"], 1);
    ASSERT_EQ(mixed.report["counts"]["passed"], 1);
}

void test_a_test_script_that_did_not_load_fails_the_run() {
    // GUT leaves the unparseable file out of its report and exits on what the
    // rest did: here everything else passed.
    const auto dropped = didi::offline::judgeTestRun(
        facts(gut(), 0, withoutFailure(kGutFailing), kParseErrorOutput));
    ASSERT_EQ(dropped.verdict, "error");
    ASSERT_EQ(dropped.reason, "scripts_did_not_load");
    ASSERT_EQ(dropped.report["scripts_did_not_load"][0]["file"], "res://test/test_broken.gd");
    ASSERT_EQ(dropped.report["scripts_did_not_load"][0]["line"], 4);
    // Beside a real failure it is still a fail.
    ASSERT_EQ(didi::offline::judgeTestRun(facts(gut(), 1, std::string(kGutFailing), kParseErrorOutput)).verdict,
              "fail");
    // An addon's own script that does not parse on this engine line, outside
    // the tested paths, is reported and does not stop a pass: GUT 9.7.1 on
    // 4.5.1 and 4.6.2.
    const auto addon = didi::offline::judgeTestRun(facts(
        gut(), 0, withoutFailure(kGutFailing),
        "SCRIPT ERROR: Parse Error: Identifier \"AccessibilityServer\" not declared in the current scope.\n"
        "   at: GDScript::reload (res://addons/gut/godot_singletons.gd:5)\n"));
    ASSERT_EQ(addon.verdict, "pass");
    ASSERT_EQ(addon.report["addon_scripts_did_not_load"][0]["file"], "res://addons/gut/godot_singletons.gd");
    ASSERT_TRUE(addon.report["scripts_did_not_load"].empty());
    // GdUnit4 stops on it (exit 105) and writes no report.
    const auto stopped = didi::offline::judgeTestRun(facts(
        gdunit(), 105, std::nullopt,
        std::string(kParseErrorOutput) +
            "ERROR: Failed to load script \"res://test/broken_test.gd\" with error \"Parse error\".\n"));
    ASSERT_EQ(stopped.reason, "scripts_did_not_load");
}

void test_an_exit_code_the_report_contradicts_is_neither_answer() {
    ASSERT_EQ(didi::offline::judgeTestRun(facts(gut(), 0, std::string(kGutFailing))).reason, "exit_code_disagrees");
    ASSERT_EQ(didi::offline::judgeTestRun(facts(gut(), 1, withoutFailure(kGutFailing))).reason,
              "exit_code_disagrees");
    // 103 to 105 are GdUnit4 saying it could not run as asked.
    ASSERT_EQ(didi::offline::judgeTestRun(facts(gdunit(), 103, withoutFailure(kGdUnitFailing))).verdict, "error");
    auto timed_out = facts(gut(), 1, std::nullopt);
    timed_out.timed_out = true;
    ASSERT_EQ(didi::offline::judgeTestRun(timed_out).reason, "timeout");
    auto cancelled = facts(gut(), 1, std::nullopt);
    cancelled.cancelled = true;
    ASSERT_EQ(didi::offline::judgeTestRun(cancelled).reason, "cancelled");
}

void test_each_framework_is_given_its_own_options() {
    const auto gut_arguments = didi::offline::testCommandArguments(
        gut(), {"res://test/unit", "res://test/test_one.gd", "res://test/it"}, "res://.didi/tests/t/results.xml");
    const std::vector<std::string> gut_expected = {
        "-s", "res://addons/gut/gut_cmdln.gd", "-gexit", "-gdir=res://test/unit,res://test/it",
        "-ginclude_subdirs", "-gtest=res://test/test_one.gd", "-gjunit_xml_file=res://.didi/tests/t/results.xml"};
    ASSERT_EQ(gut_arguments, gut_expected);
    // No paths: GUT reads .gutconfig.json for them.
    const std::vector<std::string> gut_configured = {"-s", "res://addons/gut/gut_cmdln.gd", "-gexit",
                                                     "-gjunit_xml_file=r.xml"};
    ASSERT_EQ(didi::offline::testCommandArguments(gut(), {}, "r.xml"), gut_configured);

    const auto gdunit_arguments =
        didi::offline::testCommandArguments(gdunit(), {"res://test", "res://more"}, "res://.didi/tests/t");
    const std::vector<std::string> gdunit_expected = {
        "-s", "res://addons/gdUnit4/bin/GdUnitCmdTool.gd", "--ignoreHeadlessMode", "-a", "res://test", "-a",
        "res://more", "-rd", "res://.didi/tests/t", "-rc", "1"};
    ASSERT_EQ(gdunit_arguments, gdunit_expected);
}

void test_arguments_are_refused_before_a_godot_starts() {
    using didi::offline::parseTestRunRequest;
    ASSERT_EQ(parseTestRunRequest({{"framework", "jest"}}).error().data["field"], "framework");
    ASSERT_EQ(parseTestRunRequest({{"paths", json::array({"test"})}}).error().data["field"], "paths");
    ASSERT_EQ(parseTestRunRequest({{"name", "../x"}}).error().data["field"], "name");
    ASSERT_EQ(parseTestRunRequest({{"timeout_seconds", 901}}).error().data["field"], "timeout_seconds");
    ASSERT_EQ(parseTestRunRequest({{"scene_path", "res://x.tscn"}}).error().data["field"], "scene_path");
    const auto defaults = parseTestRunRequest(json::object());
    ASSERT_TRUE(defaults.isOk());
    ASSERT_TRUE(!defaults.value().framework.has_value());
    ASSERT_EQ(defaults.value().name, "tests");
    ASSERT_EQ(defaults.value().timeout_seconds, 120);
}

struct TempProject {
    fs::path root;
    TempProject() {
        static std::atomic<int> counter{0};
        root = fs::temp_directory_path() /
               ("didi_tests_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "_" +
                std::to_string(counter++));
        fs::create_directories(root);
        write("project.godot", "config_version=5\n");
    }
    ~TempProject() {
        std::error_code error;
        fs::remove_all(root, error);
    }
    void write(const std::string& relative, const std::string& text) const {
        const auto path = root / relative;
        fs::create_directories(path.parent_path());
        std::ofstream(path, std::ios::binary) << text;
    }
};

void test_the_framework_is_the_one_the_project_has() {
    using didi::offline::chooseTestFramework;
    using didi::offline::detectTestFrameworks;
    using didi::offline::resolveTestPaths;
    TempProject project;
    didi::offline::TestRunRequest any;
    ASSERT_EQ(chooseTestFramework(any, detectTestFrameworks(project.root)).error().code, 404);

    project.write("addons/gut/gut_cmdln.gd", "extends SceneTree\n");
    project.write("addons/gut/plugin.cfg", "[plugin]\n\nname=\"Gut\"\nversion=\"9.7.1\"\n");
    const auto only_gut = chooseTestFramework(any, detectTestFrameworks(project.root));
    ASSERT_TRUE(only_gut.isOk());
    ASSERT_EQ(only_gut.value().version, "9.7.1");
    didi::offline::TestRunRequest wants_gdunit;
    wants_gdunit.framework = TestFramework::gdunit4;
    ASSERT_EQ(chooseTestFramework(wants_gdunit, detectTestFrameworks(project.root)).error().code, 404);

    // Paths: res://test when there is one, GUT's own configuration when there
    // is not, and a refusal when there is neither.
    ASSERT_EQ(resolveTestPaths(any, only_gut.value(), project.root).error().data["field"], "paths");
    project.write(".gutconfig.json", "{\"dirs\": [\"res://tests/unit\"]}");
    ASSERT_TRUE(resolveTestPaths(any, only_gut.value(), project.root).value().empty());
    project.write("test/test_a.gd", "extends GutTest\n");
    ASSERT_EQ(resolveTestPaths(any, only_gut.value(), project.root).value(), std::vector<std::string>{"res://test"});
    didi::offline::TestRunRequest named;
    named.paths = {"res://test/test_a.gd", "res://nowhere"};
    ASSERT_EQ(resolveTestPaths(named, only_gut.value(), project.root).error().code, 404);

    project.write("addons/gdUnit4/bin/GdUnitCmdTool.gd", "extends SceneTree\n");
    const auto both = chooseTestFramework(any, detectTestFrameworks(project.root));
    ASSERT_EQ(both.error().code, 409);
    ASSERT_EQ(both.error().data["field"], "framework");
    ASSERT_TRUE(chooseTestFramework(wants_gdunit, detectTestFrameworks(project.root)).isOk());
}

void test_a_test_records_the_classes_it_uses_and_not_the_framework() {
    TempProject project;
    project.write("src/calculator.gd", "class_name Calculator\nextends RefCounted\n");
    project.write("src/unused.gd", "class_name Unused\nextends RefCounted\n");
    project.write("addons/gut/test.gd", "class_name GutTest\nextends Node\n");
    project.write("test/test_calculator.gd",
                  "extends GutTest\n\nfunc test_adds():\n\tassert_eq(Calculator.new().add(2, 3), 5)\n");
    const auto files = didi::runtime::collectProofFiles(project.root, {"res://test/test_calculator.gd"});
    std::vector<std::string> listed;
    for (const auto& file : files.files) listed.push_back(file.path);
    const std::vector<std::string> expected = {"project.godot", "res://src/calculator.gd",
                                               "res://test/test_calculator.gd"};
    ASSERT_EQ(listed, expected);
}

void test_the_tool_runs_project_code_and_says_so() {
    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();
    const auto* tool = registry.getTool("project_run_tests");
    ASSERT_TRUE(tool != nullptr);
    ASSERT_TRUE(tool->capability.implemented);
    ASSERT_TRUE(tool->annotations.open_world);
    // The tests run from this repository's root, which is not a Godot project,
    // so the call is refused before any Godot starts, as an envelope.
    const auto refused = registry.callTool("project_run_tests", json::object());
    ASSERT_TRUE(refused.isError);
    const auto error = json::parse(refused.content[0].text)["error"];
    ASSERT_EQ(error["code"], 400);
    ASSERT_EQ(error["data"]["canonical_tool"], "project_run_tests");
}

std::string readAll(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// Two runs of one name at once, in the order #1242 caught them in: A writes
// its report, B starts and clears what earlier runs left, B writes its own,
// and then each reads. Each has to read its own.
void test_two_runs_of_one_name_keep_their_own_reports() {
    using didi::offline::findTestReport;
    using didi::offline::newTestRunId;
    using didi::offline::pruneTestRuns;
    using didi::offline::testRunDirectory;
    TempProject project;
    const int64_t now = 1'800'000'000'000;
    const auto names = project.root / ".didi" / "tests" / "tests";

    // What an older run left: one from long ago, and the layout before this.
    const auto old_run = std::to_string(now - didi::offline::kTestRunRetentionMs - 1) + "-0a0a0a";
    project.write(".didi/tests/tests/" + old_run + "/results.xml", "old");
    project.write(".didi/tests/tests/results.xml", "older layout");

    const auto run_a = newTestRunId(now);
    ASSERT_TRUE(run_a.isOk());
    const auto dir_a = testRunDirectory(project.root, "tests", run_a.value());
    project.write(".didi/tests/tests/" + run_a.value() + "/results.xml", "A");

    const auto run_b = newTestRunId(now);
    ASSERT_TRUE(run_b.isOk());
    ASSERT_TRUE(run_a.value() != run_b.value());
    const auto dir_b = testRunDirectory(project.root, "tests", run_b.value());
    ASSERT_TRUE(dir_a != dir_b);
    ASSERT_EQ(dir_b.parent_path(), names);
    pruneTestRuns(names, run_b.value(), now);
    project.write(".didi/tests/tests/" + run_b.value() + "/results.xml", "B");

    const auto report_a = findTestReport(dir_a, TestFramework::gut);
    const auto report_b = findTestReport(dir_b, TestFramework::gut);
    ASSERT_TRUE(report_a.has_value() && report_b.has_value());
    ASSERT_EQ(readAll(*report_a), "A");
    ASSERT_EQ(readAll(*report_b), "B");
    // What was too old to be running, and the older layout, are gone.
    ASSERT_TRUE(!fs::exists(names / old_run));
    ASSERT_TRUE(!fs::exists(names / "results.xml"));

    // GdUnit4 writes report_<n>/results.xml under the directory it is given.
    project.write(".didi/tests/tests/" + run_b.value() + "/report_1/results.xml", "B4");
    fs::remove(dir_b / "results.xml");
    const auto gdunit_report = findTestReport(dir_b, TestFramework::gdunit4);
    ASSERT_TRUE(gdunit_report.has_value());
    ASSERT_EQ(readAll(*gdunit_report), "B4");
    ASSERT_TRUE(!findTestReport(dir_b, TestFramework::gut).has_value());
}

struct RegisterTestReportTests {
    RegisterTestReportTests() {
        registerTest("TestReports.ReadTestByTest", test_both_frameworks_reports_are_read_test_by_test);
        registerTest("TestReports.MalformedReportsAreRefused",
                     test_a_report_that_is_not_xml_is_refused_rather_than_guessed);
        registerTest("TestReports.VerdictComesFromTheReport", test_the_verdict_comes_from_the_report);
        registerTest("TestReports.NothingTestedNeverPasses", test_a_run_that_tested_nothing_never_passes);
        registerTest("TestReports.ScriptThatDidNotLoadFailsTheRun",
                     test_a_test_script_that_did_not_load_fails_the_run);
        registerTest("TestReports.ContradictedExitCodeIsNeither",
                     test_an_exit_code_the_report_contradicts_is_neither_answer);
        registerTest("TestReports.EachFrameworkGetsItsOptions", test_each_framework_is_given_its_own_options);
        registerTest("TestReports.ArgumentsAreRefusedFirst", test_arguments_are_refused_before_a_godot_starts);
        registerTest("TestReports.FrameworkIsTheProjects", test_the_framework_is_the_one_the_project_has);
        registerTest("TestReports.RecordFollowsClassNames",
                     test_a_test_records_the_classes_it_uses_and_not_the_framework);
        registerTest("TestReports.ToolRunsProjectCode", test_the_tool_runs_project_code_and_says_so);
        registerTest("TestReports.RunsOfOneNameKeepTheirOwnReports",
                     test_two_runs_of_one_name_keep_their_own_reports);
    }
} g_register_test_report_tests;

}  // namespace
