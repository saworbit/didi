#include "didi/gdextension/crash_capture.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <stdexcept>
#include <string>

#if defined(_WIN32)
#include <windows.h>
#include <intrin.h>
#include <stdlib.h>
#endif

#define ASSERT_TRUE(cond) if (!(cond)) throw std::runtime_error("Assertion failed: " #cond);
#define ASSERT_FALSE(cond) ASSERT_TRUE(!(cond))

void registerTest(const std::string& name, std::function<void()> fn);

namespace {

#if defined(_WIN32)

constexpr const char* kSelfTestVariable = "DIDI_CRASH_CAPTURE_SELF_TEST";
constexpr const char* kSelfTestStealVariable = "DIDI_CRASH_CAPTURE_SELF_TEST_STEAL";

LONG WINAPI terminateQuietly(EXCEPTION_POINTERS*) {
    // Stands in for the filter the engine already had. Returning this ends the
    // process instead of handing it to Windows Error Reporting, which keeps the
    // child quick and keeps a build machine from queueing a crash upload.
    return EXCEPTION_EXECUTE_HANDLER;
}

// The child half of the end to end check. Runs before main, so the suite never
// starts in this process. Nothing here is reachable unless the parent asks for
// it by name.
struct CrashCaptureSelfTest {
    CrashCaptureSelfTest() {
        const char* directory = std::getenv(kSelfTestVariable);
        if (!directory || !*directory) return;
        SetErrorMode(SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS);
        _putenv_s("DIDI_CRASH_CAPTURE_DIR", directory);
        SetUnhandledExceptionFilter(terminateQuietly);
        if (!didi::godot::armCrashCapture()) ExitProcess(2);
        const char* steal = std::getenv(kSelfTestStealVariable);
        if (steal && *steal) {
            // What the engine does: installs its own top level filter after the
            // extension has loaded, which silently replaces ours.
            SetUnhandledExceptionFilter(terminateQuietly);
            didi::godot::reassertCrashCapture();
        }
        // The fault the issue reports, raised for real rather than described.
        __ud2();
        ExitProcess(3);
    }
};

CrashCaptureSelfTest g_crash_capture_self_test;

std::string readFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    std::ostringstream contents;
    contents << file.rdbuf();
    return contents.str();
}

std::wstring thisExecutablePath() {
    wchar_t path[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, path, MAX_PATH) == 0) return {};
    return path;
}

std::filesystem::path makeScratchDirectory(const char* label) {
    const auto directory =
        std::filesystem::temp_directory_path() /
        (std::string("didi-crash-capture-") + label + "-" + std::to_string(GetCurrentProcessId()) +
         "-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(directory);
    return directory;
}

// #285 leaves a bare exit code and no engine side trace, so the report this
// writes is the whole point of arming. Waiting for a real fault to find out
// whether it is readable is how the last occurrences were lost.
void test_crash_report_names_the_fault_the_thread_and_the_stack() {
    const auto directory = makeScratchDirectory("report");

    // Nothing is armed until it is asked for, so an editor a user is running
    // never pays for this.
    _putenv_s("DIDI_CRASH_CAPTURE_DIR", "");
    ASSERT_FALSE(didi::godot::armCrashCapture());
    ASSERT_TRUE(didi::godot::crashReportPath().empty());

    _putenv_s("DIDI_CRASH_CAPTURE_DIR", directory.string().c_str());
    ASSERT_TRUE(didi::godot::armCrashCapture());
    const std::filesystem::path report_path(didi::godot::crashReportPath());
    ASSERT_FALSE(report_path.empty());
    ASSERT_TRUE(report_path.parent_path() == directory);

    CONTEXT context{};
    RtlCaptureContext(&context);
    EXCEPTION_RECORD record{};
    record.ExceptionCode = EXCEPTION_ILLEGAL_INSTRUCTION;
    record.ExceptionFlags = 0;
    record.ExceptionAddress = reinterpret_cast<PVOID>(context.Rip);
    EXCEPTION_POINTERS pointers{&record, &context};
    const bool written = didi::godot::writeCrashReport(&pointers);

    const std::string report = readFile(report_path);
    const std::filesystem::path executable(thisExecutablePath());
    const std::string executable_name = executable.filename().string();
    ASSERT_TRUE(didi::godot::disarmCrashCapture());
    _putenv_s("DIDI_CRASH_CAPTURE_DIR", "");
    std::error_code cleanup;
    std::filesystem::remove_all(directory, cleanup);

    ASSERT_TRUE(written);
    ASSERT_TRUE(report.find("DIDI CRASH CAPTURE") != std::string::npos);
    // The exit code the issue reports, named rather than left as a number.
    ASSERT_TRUE(report.find("0xc000001d") != std::string::npos);
    ASSERT_TRUE(report.find("ILLEGAL_INSTRUCTION") != std::string::npos);
    // Which thread died is the question the engine log cannot answer, and this
    // record was raised on the thread the test runs on.
    ASSERT_TRUE(report.find("thread: " + std::to_string(GetCurrentThreadId())) !=
                std::string::npos);
    // A stack that resolves no module is a list of numbers. This one has to
    // name the binary it was taken in.
    ASSERT_FALSE(executable_name.empty());
    ASSERT_TRUE(report.find(executable_name + "+0x") != std::string::npos);
    ASSERT_TRUE(report.find("loaded modules:") != std::string::npos);
    ASSERT_TRUE(report.find("END DIDI CRASH CAPTURE") != std::string::npos);

    // Disarming has to leave nothing behind, or the next process to ask would
    // write into the directory the last one chose.
    ASSERT_TRUE(didi::godot::crashReportPath().empty());
    ASSERT_FALSE(didi::godot::writeCrashReport(&pointers));
}

// The report writer being right is not the same as the handler running. The
// first attempt at this used a vectored handler, which does fire, and also
// fired on exceptions the engine raises and handles normally, and hung the
// editor's shutdown. So the thing worth testing is the whole path: a real
// illegal instruction, in a real process, reaching a real filter.
void runFaultingChild(const char* label, bool steal) {
    const auto directory = makeScratchDirectory(label);
    const std::wstring executable = thisExecutablePath();
    ASSERT_FALSE(executable.empty());

    SetEnvironmentVariableA(kSelfTestVariable, directory.string().c_str());
    if (steal) SetEnvironmentVariableA(kSelfTestStealVariable, "1");
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION child{};
    std::wstring command = L"\"" + executable + L"\"";
    const BOOL started = CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE,
                                        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &child);
    SetEnvironmentVariableA(kSelfTestVariable, nullptr);
    SetEnvironmentVariableA(kSelfTestStealVariable, nullptr);
    if (!started) {
        std::error_code cleanup;
        std::filesystem::remove_all(directory, cleanup);
        throw std::runtime_error("Failed to start the faulting child process");
    }
    CloseHandle(child.hThread);

    const DWORD waited = WaitForSingleObject(child.hProcess, 60000);
    DWORD exit_code = 0;
    const bool read_exit_code = GetExitCodeProcess(child.hProcess, &exit_code) != 0;
    if (waited != WAIT_OBJECT_0) TerminateProcess(child.hProcess, 1);
    CloseHandle(child.hProcess);

    const auto report_path =
        directory / ("godot_crash_" + std::to_string(child.dwProcessId) + ".log");
    const bool report_exists = std::filesystem::exists(report_path);
    const std::string report = report_exists ? readFile(report_path) : std::string();
    std::error_code cleanup;
    std::filesystem::remove_all(directory, cleanup);

    ASSERT_TRUE(waited == WAIT_OBJECT_0);
    ASSERT_TRUE(read_exit_code);
    // Exit code 2 means arming refused, 3 means the fault did not happen. Both
    // would leave the report missing for reasons worth telling apart.
    ASSERT_TRUE(exit_code != 2);
    ASSERT_TRUE(exit_code != 3);
    ASSERT_TRUE(report_exists);
    ASSERT_TRUE(report.find("ILLEGAL_INSTRUCTION") != std::string::npos);
    ASSERT_TRUE(report.find("on main thread: yes") != std::string::npos);
    ASSERT_TRUE(report.find("END DIDI CRASH CAPTURE") != std::string::npos);
    // Chaining matters as much as reporting. The filter the child installed
    // first is what ends the process, and it is still the thing that ran.
    ASSERT_TRUE(exit_code == static_cast<DWORD>(EXCEPTION_ILLEGAL_INSTRUCTION));
}

void test_a_real_illegal_instruction_leaves_a_report() { runFaultingChild("child", false); }

// The engine installs its own top level filter after the extension has loaded.
// That replaced this one in the field, so a real crash was reported by the
// engine's handler, which on an official build has no symbols and printed
// thirty lines of "no debug info", and this handler never ran. Reasserting is
// what puts it back, and this is the case that proves it.
void test_a_stolen_filter_is_taken_back() { runFaultingChild("stolen", true); }

#endif

} // namespace

struct RegisterCrashCaptureTests {
    RegisterCrashCaptureTests() {
#if defined(_WIN32)
        registerTest("CrashCapture.ReportIsReadable",
                     test_crash_report_names_the_fault_the_thread_and_the_stack);
        registerTest("CrashCapture.RealFaultIsCaptured",
                     test_a_real_illegal_instruction_leaves_a_report);
        registerTest("CrashCapture.StolenFilterIsTakenBack",
                     test_a_stolen_filter_is_taken_back);
#endif
    }
};

static RegisterCrashCaptureTests g_register_crash_capture_tests;
