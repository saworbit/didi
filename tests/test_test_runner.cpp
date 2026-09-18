#include "didi/mcp/tool_registry.hpp"
#include "didi/offline/test_runner.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>

#define ASSERT_TRUE(cond) if (!(cond)) throw std::runtime_error("Assertion failed: " #cond)
#define ASSERT_EQ(a, b) ASSERT_TRUE((a) == (b))

void registerTest(const std::string& name, std::function<void()> fn);

namespace {

class ScopedEnvironmentVariable {
public:
    explicit ScopedEnvironmentVariable(std::string name) : m_name(std::move(name)) {
        if (const char* value = std::getenv(m_name.c_str())) m_original = value;
    }
    ~ScopedEnvironmentVariable() { set(m_original); }

    void set(const std::optional<std::string>& value) const {
#if defined(_WIN32)
        _putenv_s(m_name.c_str(), value ? value->c_str() : "");
#else
        if (value) setenv(m_name.c_str(), value->c_str(), 1);
        else unsetenv(m_name.c_str());
#endif
    }

private:
    std::string m_name;
    std::optional<std::string> m_original;
};

std::string commandShell() {
#if defined(_WIN32)
    if (const char* shell = std::getenv("ComSpec")) return shell;
    return "C:\\Windows\\System32\\cmd.exe";
#else
    return "/bin/sh";
#endif
}

std::vector<std::string> successfulShellArguments() {
#if defined(_WIN32)
    return {"/d", "/c", "exit", "0"};
#else
    return {"-c", "exit 0"};
#endif
}

void test_runtime_launch_schema_bounds_timeout() {
    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();
    const auto* definition = registry.getTool("runtime_launch");
    ASSERT_TRUE(definition != nullptr);
    const auto& timeout = definition->inputSchema["properties"]["timeout_seconds"];
    ASSERT_EQ(timeout["minimum"], 1);
    ASSERT_EQ(timeout["maximum"], 120);
}

void test_runtime_launch_rejects_timeout_outside_public_range() {
    ScopedEnvironmentVariable godot_bin("GODOT_BIN");
    godot_bin.set(commandShell());
    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();
    const auto result = registry.callTool(
        "runtime_launch",
        {{"timeout_seconds", 0}, {"headless", false},
         {"extra_args", successfulShellArguments()}});
    ASSERT_TRUE(result.isError);
}

void test_resolver_finds_documented_godot_451_layout() {
    ScopedEnvironmentVariable godot_bin("GODOT_BIN");
    ScopedEnvironmentVariable godot_path("GODOT_PATH");
    godot_bin.set(std::nullopt);
    const auto root = std::filesystem::temp_directory_path() /
                      ("didi-godot-discovery-" + std::to_string(
                          std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);
#if defined(_WIN32)
    const auto candidate = root / "Godot_v4.5.1-stable_win64_console.exe";
#else
    const auto candidate = root / "Godot_v4.5.1-stable_linux.x86_64";
#endif
    std::ofstream(candidate).put('\n');
    godot_path.set(root.string());
    const auto resolved = didi::offline::resolveGodotExecutable();
    std::filesystem::remove_all(root);
    ASSERT_EQ(std::filesystem::path(resolved), candidate);
}

#if defined(_WIN32)
class ScopedWideEnvironmentVariable {
public:
    explicit ScopedWideEnvironmentVariable(std::wstring name) : m_name(std::move(name)) {
        if (const wchar_t* value = _wgetenv(m_name.c_str())) m_original = value;
    }
    ~ScopedWideEnvironmentVariable() { set(m_original); }

    void set(const std::optional<std::wstring>& value) const {
        _wputenv_s(m_name.c_str(), value ? value->c_str() : L"");
    }

private:
    std::wstring m_name;
    std::optional<std::wstring> m_original;
};

void test_windows_exit_code_259_is_completed_not_timed_out() {
    ScopedEnvironmentVariable godot_bin("GODOT_BIN");
    godot_bin.set(commandShell());
    // The timeout is generous on purpose, and the bound is derived from it
    // rather than from how long this took on one machine.
    //
    // 259 is STILL_ACTIVE. What this proves is that it is read as a process
    // that finished rather than one still going, and one still going would be
    // waited out to the timeout. So the two answers are "returns as soon as the
    // process exits" and "takes the whole timeout", and the bound only has to
    // separate them.
    //
    // A short timeout made that separation two tenths of a second, which is not
    // a property of the code under test, it is a bet on how quickly a loaded
    // runner can spawn a process. Five seconds against a three second bound
    // leaves two seconds of margin above a correct run and two below a wrong
    // one. A passing run still returns immediately, so none of this is paid for
    // unless the test is failing.
    const auto result = didi::offline::TestRunner::runSession(
        "", 5, false, false, {"/d", "/c", "exit", "259"});
    ASSERT_EQ(result.exit_code, 259);
    ASSERT_TRUE(result.duration_seconds < 3.0);
}

void test_windows_completed_parent_does_not_wait_for_inherited_stdout() {
    // Break caught: blocking EOF drain waits for a descendant that inherited the output pipe.
    ScopedEnvironmentVariable godot_bin("GODOT_BIN");
    godot_bin.set(commandShell());
    // `ping -n 8` sends eight probes a second apart, so the descendant that
    // inherits the pipe lives for about seven seconds, measured at 7.12, while
    // the shell that started it returns at once. Those seven seconds are the
    // floor for the bug: a drain that blocks on EOF cannot come back before the
    // descendant closes the pipe.
    //
    // The bound is read off that floor rather than off a stopwatch, and the
    // descendant is deliberately long so the floor and a correct run are far
    // apart. Four seconds sits three below the floor, which is what makes the
    // test discriminate, and nearly four above a correct run, which is what
    // stops a loaded runner failing it.
    //
    // The previous shape was `ping -n 4` against a 1.2 second bound. Its floor
    // was 3.07 seconds and CI was observed taking more than 1.2 for a correct
    // run, so the margin above a pass was smaller than the noise, and it failed
    // a release on a diff of one version digit and some markdown. A passing run
    // returns as soon as the shell exits and never waits for the descendant, so
    // the longer ping costs a passing run nothing.
    const auto result = didi::offline::TestRunner::runSession(
        "", 12, false, false,
        {"/d", "/c", "start", "/b", "ping", "-n", "8", "127.0.0.1"});
    ASSERT_EQ(result.exit_code, 0);
    ASSERT_TRUE(result.duration_seconds < 4.0);
}

void test_windows_batch_wrapper_is_launched_through_command_shell() {
    const std::filesystem::path interpreter(
        didi::offline::detail::trustedWindowsCommandInterpreter());
    ASSERT_TRUE(interpreter.is_absolute());
    ASSERT_EQ(interpreter.filename().wstring(), L"cmd.exe");

    ScopedEnvironmentVariable godot_bin("GODOT_BIN");
    const auto root = std::filesystem::temp_directory_path() /
                      ("didi-godot-wrapper-" + std::to_string(
                          std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);
    const auto wrapper = root / "godot.cmd";
    std::ofstream(wrapper) << "@echo off\nexit /b 0\n";
    godot_bin.set(wrapper.string());

    const auto result = didi::offline::TestRunner::runSession("", 2, false, false);
    std::filesystem::remove_all(root);
    ASSERT_TRUE(result.success);
    ASSERT_EQ(result.exit_code, 0);
}

void test_windows_batch_wrapper_supports_non_ascii_path() {
    ScopedWideEnvironmentVariable godot_bin(L"GODOT_BIN");
    const auto root = std::filesystem::temp_directory_path() /
                      (L"didi-godot-测试-" + std::to_wstring(
                          std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);
    const auto wrapper = root / L"godot.cmd";
    std::ofstream(wrapper) << "@echo off\nexit /b 0\n";
    godot_bin.set(wrapper.wstring());

    const auto resolved = didi::offline::resolveGodotExecutable();
    const auto result = didi::offline::TestRunner::runSession("", 2, false, false);
    std::filesystem::remove_all(root);
    const auto expected_utf8 = wrapper.u8string();
    ASSERT_EQ(resolved, std::string(reinterpret_cast<const char*>(expected_utf8.data()),
                                    expected_utf8.size()));
    ASSERT_TRUE(result.success);
    ASSERT_EQ(result.exit_code, 0);
}

void test_windows_arguments_are_quoted_rather_than_dropped() {
    // Break caught: the old is_safe_arg filter silently skipped any argument
    // containing a quote, comma or semicolon on Windows, while POSIX passed the
    // same argument through untouched.
    const std::vector<std::string> arguments = {
        "--headless", "res://scenes/level one.tscn", "--custom=a,b;c", "--label=\"quoted\""
    };
    const auto command = didi::offline::detail::makeWindowsProcessCommand(
        "C:/Program Files/Godot/godot.exe", arguments);
    ASSERT_TRUE(command.has_value());

    const auto& line = command->command_line;
    ASSERT_TRUE(line.find(L"\"C:/Program Files/Godot/godot.exe\"") != std::wstring::npos);
    ASSERT_TRUE(line.find(L"--headless") != std::wstring::npos);
    ASSERT_TRUE(line.find(L"\"res://scenes/level one.tscn\"") != std::wstring::npos);
    // Present, not dropped, even though it carries a comma and a semicolon.
    ASSERT_TRUE(line.find(L"--custom=a,b;c") != std::wstring::npos);
    // The embedded quotes survive, escaped the way CreateProcessW expects.
    ASSERT_TRUE(line.find(L"\\\"quoted\\\"") != std::wstring::npos);
}

void test_batch_wrapper_refuses_shell_metacharacters() {
    // Break caught: a .cmd wrapper really does go through cmd.exe, so an
    // unescaped metacharacter would run something other than what was asked.
    // Refusing is the answer, not quietly discarding the argument.
    const auto safe = didi::offline::detail::makeWindowsProcessCommand(
        "C:/tools/godot.cmd", {"--headless"});
    ASSERT_TRUE(safe.has_value());

    for (const auto& hostile : {"--scene=a&calc", "--scene=a|b", "--scene=a>out"}) {
        const auto refused = didi::offline::detail::makeWindowsProcessCommand(
            "C:/tools/godot.cmd", {std::string(hostile)});
        ASSERT_TRUE(!refused.has_value());
    }

    // The direct executable path has no shell, so the same argument is fine.
    const auto direct = didi::offline::detail::makeWindowsProcessCommand(
        "C:/tools/godot.exe", {"--scene=a&calc"});
    ASSERT_TRUE(direct.has_value());
}
#endif

// A script that prints a Godot-shaped crash and then outlives the timeout, so
// the run ends the way a real crashing game does. Written per platform because
// a shell script and a batch file are the two things GODOT_BIN can name here.
class ScopedCrashingEngine {
public:
    ScopedCrashingEngine() {
        m_root = std::filesystem::temp_directory_path() /
                 ("didi-crash-runner-" +
                  std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(m_root);
#if defined(_WIN32)
        m_path = m_root / "fake_engine.bat";
        std::ofstream out(m_path, std::ios::binary);
        out << "@echo off\r\n"
               "echo Godot Engine v4.7.2.stable.official - https://godotengine.org\r\n"
               "echo about to fail\r\n"
               "echo SCRIPT ERROR: Invalid access to property or key 'name' on a base object of type 'Nil'.\r\n"
               "echo    at: _ready (res://crasher.gd:6)\r\n"
               "echo GDScript backtrace (most recent call first):\r\n"
               "echo    [0] _ready (res://crasher.gd:6)\r\n"
               "echo    [1] _run (res://level.gd:12)\r\n"
               "ping -n 30 127.0.0.1 >nul\r\n";
#else
        m_path = m_root / "fake_engine.sh";
        std::ofstream out(m_path, std::ios::binary);
        out << "#!/bin/sh\n"
               "echo 'Godot Engine v4.7.2.stable.official - https://godotengine.org'\n"
               "echo 'about to fail'\n"
               "echo \"SCRIPT ERROR: Invalid access to property or key 'name' on a base object of type 'Nil'.\"\n"
               "echo '   at: _ready (res://crasher.gd:6)'\n"
               "echo 'GDScript backtrace (most recent call first):'\n"
               "echo '   [0] _ready (res://crasher.gd:6)'\n"
               "echo '   [1] _run (res://level.gd:12)'\n"
               "sleep 30\n";
        out.close();
        std::filesystem::permissions(m_path, std::filesystem::perms::owner_all,
                                     std::filesystem::perm_options::add);
#endif
    }

    ~ScopedCrashingEngine() {
        std::error_code ignored;
        std::filesystem::remove_all(m_root, ignored);
    }

    std::string path() const { return m_path.string(); }

private:
    std::filesystem::path m_root;
    std::filesystem::path m_path;
};

void test_a_crash_keeps_its_location_and_names_itself_in_the_summary() {
    // Break caught: Godot prints an error across several lines -- the message,
    // then `at: _ready (res://crasher.gd:6)`, then the GDScript backtrace --
    // and each line was classified on its own text. Every frame of a crash
    // landed under INFO, the level print() gets, so a caller filtering logs on
    // ERROR kept the message and dropped the whole stack. errors[] held a bare
    // string with no file or line, and summary named the timeout while the
    // cause sat one key away (#744).
    ScopedCrashingEngine engine;
    ScopedEnvironmentVariable godot_bin("GODOT_BIN");
    godot_bin.set(engine.path());

    const auto result = didi::offline::TestRunner::runSession("res://crash.tscn", 1, true, true, {});
    ASSERT_TRUE(result.timed_out);
    ASSERT_EQ(result.exit_code, 124);
    ASSERT_TRUE(!result.success);

    // The summary names the crash, not only the timeout that followed it.
    ASSERT_TRUE(result.summary.find("timeout") != std::string::npos);
    ASSERT_TRUE(result.summary.find("Invalid access to property") != std::string::npos);

    // Every line of the error carries the error's level, and says it is a
    // continuation rather than an error of its own.
    size_t error_lines = 0;
    size_t continuations = 0;
    for (const auto& entry : result.logs) {
        if (entry.level == "ERROR") ++error_lines;
        if (entry.continuation) ++continuations;
        // The game's own print() is not swept up with them.
        if (entry.message.find("about to fail") != std::string::npos) {
            ASSERT_EQ(entry.level, std::string("INFO"));
            ASSERT_TRUE(!entry.continuation);
        }
    }
    // The message, the at: line, the backtrace header and two frames.
    ASSERT_EQ(error_lines, static_cast<size_t>(5));
    ASSERT_EQ(continuations, static_cast<size_t>(4));

    // errors[] is unchanged: one message line, no continuations folded in.
    ASSERT_EQ(result.errors.size(), static_cast<size_t>(1));

    // And the structured half has somewhere to go.
    ASSERT_EQ(result.diagnostics.size(), static_cast<size_t>(1));
    const auto& diagnostic = result.diagnostics.front();
    ASSERT_EQ(diagnostic.file, std::string("res://crasher.gd"));
    ASSERT_EQ(diagnostic.line, 6);
    ASSERT_EQ(diagnostic.function, std::string("_ready"));
    ASSERT_EQ(diagnostic.frames.size(), static_cast<size_t>(2));
    ASSERT_EQ(diagnostic.frames[1].file, std::string("res://level.gd"));
    ASSERT_EQ(diagnostic.frames[1].line, 12);
    ASSERT_EQ(diagnostic.frames[1].function, std::string("_run"));
}

struct RegisterTestRunnerTests {
    RegisterTestRunnerTests() {
        registerTest("RuntimeLaunch.CrashKeepsItsLocation",
                     test_a_crash_keeps_its_location_and_names_itself_in_the_summary);
        registerTest("RuntimeLaunch.TimeoutSchema", test_runtime_launch_schema_bounds_timeout);
        registerTest("RuntimeLaunch.TimeoutValidation", test_runtime_launch_rejects_timeout_outside_public_range);
        registerTest("RuntimeLaunch.Godot451Discovery", test_resolver_finds_documented_godot_451_layout);
#if defined(_WIN32)
        registerTest("RuntimeLaunch.WindowsExit259", test_windows_exit_code_259_is_completed_not_timed_out);
        registerTest("RuntimeLaunch.WindowsBoundedOutputDrain", test_windows_completed_parent_does_not_wait_for_inherited_stdout);
        registerTest("RuntimeLaunch.WindowsBatchWrapper", test_windows_batch_wrapper_is_launched_through_command_shell);
        registerTest("RuntimeLaunch.WindowsUnicodeBatchWrapper", test_windows_batch_wrapper_supports_non_ascii_path);
        registerTest("RuntimeLaunch.WindowsArgumentsAreQuoted", test_windows_arguments_are_quoted_rather_than_dropped);
        registerTest("RuntimeLaunch.BatchWrapperRefusesMetacharacters", test_batch_wrapper_refuses_shell_metacharacters);
#endif
    }
} g_register_test_runner_tests;

} // namespace
