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
void test_windows_exit_code_259_is_completed_not_timed_out() {
    ScopedEnvironmentVariable godot_bin("GODOT_BIN");
    godot_bin.set(commandShell());
    const auto result = didi::offline::TestRunner::runSession(
        "", 1, false, false, {"/d", "/c", "exit", "259"});
    ASSERT_EQ(result.exit_code, 259);
    ASSERT_TRUE(result.duration_seconds < 0.8);
}

void test_windows_completed_parent_does_not_wait_for_inherited_stdout() {
    // Break caught: blocking EOF drain waits for a descendant that inherited the output pipe.
    ScopedEnvironmentVariable godot_bin("GODOT_BIN");
    godot_bin.set(commandShell());
    const auto result = didi::offline::TestRunner::runSession(
        "", 5, false, false,
        {"/d", "/c", "start", "/b", "ping", "-n", "4", "127.0.0.1"});
    ASSERT_EQ(result.exit_code, 0);
    ASSERT_TRUE(result.duration_seconds < 1.2);
}
#endif

struct RegisterTestRunnerTests {
    RegisterTestRunnerTests() {
        registerTest("RuntimeLaunch.TimeoutSchema", test_runtime_launch_schema_bounds_timeout);
        registerTest("RuntimeLaunch.TimeoutValidation", test_runtime_launch_rejects_timeout_outside_public_range);
        registerTest("RuntimeLaunch.Godot451Discovery", test_resolver_finds_documented_godot_451_layout);
#if defined(_WIN32)
        registerTest("RuntimeLaunch.WindowsExit259", test_windows_exit_code_259_is_completed_not_timed_out);
        registerTest("RuntimeLaunch.WindowsBoundedOutputDrain", test_windows_completed_parent_does_not_wait_for_inherited_stdout);
#endif
    }
} g_register_test_runner_tests;

} // namespace
