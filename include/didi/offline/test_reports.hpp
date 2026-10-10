#pragma once

// The project's own tests, run headless (Q9 part 2 in docs/BUILD_QUEUE.md,
// principle P7).
//
// project_run_tests runs GUT or GdUnit4 from the command line, the path each
// documents for CI, and reads the JUnit XML both write. The verdict comes from
// that report and not from the exit code, because measured on 4.7.2 the exit
// code says 0 for runs that tested nothing: GUT with no tests, GUT on a project
// that was never imported (no report at all), GdUnit4 with no tests. A test
// file that does not parse is left out of GUT's report altogether, so a run
// whose output names a script that failed to load cannot pass either.
//
// Everything here is pure: what to run, how to read what it wrote, and what
// that means. src/tools/test_tools.cpp starts the process.

#include "didi/common/json.hpp"
#include "didi/common/types.hpp"
#include "didi/offline/deep_domain_support.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace didi::offline {

enum class TestFramework { gut, gdunit4 };
const char* testFrameworkName(TestFramework framework);

struct TestRunRequest {
    // Nothing means: whichever of the two the project has.
    std::optional<TestFramework> framework;
    // res:// directories or .gd files. Empty means the framework's own
    // configuration for GUT, and res://test for either when it is there.
    std::vector<std::string> paths;
    // The record's name under .didi/scenarios/.
    std::string name{"tests"};
    int timeout_seconds{120};
};

inline constexpr int kMinTestTimeoutSeconds = 5;
inline constexpr int kMaxTestTimeoutSeconds = 900;
inline constexpr size_t kMaxTestPaths = 32;
inline constexpr size_t kMaxReportedTests = 1000;
inline constexpr size_t kMaxReportBytes = 16u * 1024u * 1024u;

Result<TestRunRequest> parseTestRunRequest(const json& arguments);

struct InstalledTestFramework {
    TestFramework framework;
    // The entry script Godot is given with -s.
    std::string entry;
    // plugin.cfg's version, or empty when it cannot be read.
    std::string version;
};

// Which of the two frameworks the project's addons/ folder holds.
std::vector<InstalledTestFramework> detectTestFrameworks(const std::filesystem::path& project_root);

// The framework to run: the one asked for, if it is installed, or the only one
// installed. Refused when none is, or when both are and none was named.
Result<InstalledTestFramework> chooseTestFramework(const TestRunRequest& request,
                                                   const std::vector<InstalledTestFramework>& installed);

// The res:// paths the run covers, after defaults: what was asked for, or
// res://test when it exists, or nothing for GUT when .gutconfig.json says
// where its tests are. Refused when there is nothing to run.
Result<std::vector<std::string>> resolveTestPaths(const TestRunRequest& request,
                                                  const InstalledTestFramework& framework,
                                                  const std::filesystem::path& project_root);

// Each run writes its report under a directory of its own,
// .didi/tests/<name>/<run>, where <run> is the run's start in milliseconds and
// a random suffix. Two runs of one name, in one server or in two, never share
// a directory, so neither reads or clears the other's report (#1242).
Result<std::string> newTestRunId(int64_t started_at_ms);
std::filesystem::path testRunDirectory(const std::filesystem::path& project_root, const std::string& name,
                                       const std::string& run);
// The report a framework wrote under a run's directory: GUT's results.xml at
// its top, GdUnit4's under report_<n>/.
std::optional<std::filesystem::path> findTestReport(const std::filesystem::path& run_directory,
                                                    TestFramework framework);
// A run started this long ago has been stopped by its timeout, so its
// directory is no longer in use.
inline constexpr int64_t kTestRunRetentionMs = (kMaxTestTimeoutSeconds + 300) * int64_t{1000};
// Removes what earlier runs of a name left under .didi/tests/<name>/, except
// `keep` and any run that started less than kTestRunRetentionMs before
// `now_ms`, which may still be going.
void pruneTestRuns(const std::filesystem::path& name_directory, const std::string& keep, int64_t now_ms);

// Godot's arguments, after --headless and --path: the entry script and the
// framework's own options, with the report written to `report` (a res:// file
// for GUT, a res:// directory for GdUnit4).
std::vector<std::string> testCommandArguments(const InstalledTestFramework& framework,
                                              const std::vector<std::string>& paths,
                                              const std::string& report);

struct TestCaseResult {
    std::string suite;
    std::string name;
    // passed, failed, error, skipped, or no_assertions (GUT's "no asserts").
    std::string outcome;
    std::string message;
    // Where it failed, when the report says: a res:// path and a line.
    std::string file;
    int line{0};
    double seconds{0.0};
};

struct JUnitReport {
    std::vector<TestCaseResult> cases;
};

// Reads the JUnit XML both frameworks write. Bounded: a report over
// kMaxReportBytes, or nested deeper than any test report is, is refused.
Result<JUnitReport> parseJUnitReport(std::string_view xml);

struct TestRunFacts {
    InstalledTestFramework framework;
    std::vector<std::string> paths;
    int exit_code{0};
    bool timed_out{false};
    bool cancelled{false};
    // Nothing when the framework wrote no report.
    std::optional<std::string> report_xml;
    std::string output;
    bool output_truncated{false};
};

struct TestRunVerdict {
    // pass, fail or error, as runtime_run_scenario uses them.
    std::string verdict;
    // Empty on a pass; otherwise a short identifier: tests_failed, no_tests,
    // nothing_proved, no_report, report_unreadable, scripts_did_not_load,
    // exit_code_disagrees, output_truncated, timeout, cancelled.
    std::string reason;
    std::string summary;
    json report;
};

// What the run proved. Never a pass for a run with no report, no test that
// passed, a script that did not load, an exit code the report contradicts, or
// output cut short before every script could be seen to load.
TestRunVerdict judgeTestRun(const TestRunFacts& facts);

}  // namespace didi::offline
