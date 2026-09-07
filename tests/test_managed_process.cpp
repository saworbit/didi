#include "didi/runtime/managed_process.hpp"
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <thread>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <crt_externs.h>
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#else
#include <fcntl.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

void registerTest(const std::string&, std::function<void()>);
#define CHECK_PROCESS(x)                                                                           \
    do {                                                                                           \
        if (!(x))                                                                                  \
            throw std::runtime_error("Assertion failed: " #x);                                     \
    } while (false)

namespace {
namespace fs = std::filesystem;
using didi::runtime::ManagedProcess;

std::vector<std::string> processArguments() {
    std::vector<std::string> args;
#if defined(_WIN32)
    for (int i = 0; i < __argc; ++i)
        args.emplace_back(__argv[i]);
#elif defined(__APPLE__)
    for (int i = 0; i < *_NSGetArgc(); ++i)
        args.emplace_back((*_NSGetArgv())[i]);
#else
    std::ifstream input("/proc/self/cmdline", std::ios::binary);
    std::string arg;
    while (std::getline(input, arg, '\0'))
        args.push_back(arg);
#endif
    return args;
}

// The real test executable doubles as a child fixture, before the normal runner.
struct ChildFixture {
    ChildFixture() {
        auto args = processArguments();
        if (args.size() < 3 || args[1] != "--didi-managed-child")
            return;
        if (args[2] == "hold")
            std::this_thread::sleep_for(std::chrono::seconds(30));
#if !defined(_WIN32)
        if (args[2] == "check_descriptor" && args.size() == 4) {
            const int descriptor = std::stoi(args[3]);
            std::_Exit(fcntl(descriptor, F_GETFD) == -1 && errno == EBADF ? 37 : 38);
        }
#endif
        if (args[2] == "report") {
            std::cout << didi::json(
                             {{"arguments", std::vector<std::string>(args.begin() + 3, args.end())},
                              {"cwd", fs::current_path().string()},
                              {"stdin_eof", std::cin.get() == EOF}})
                             .dump()
                      << std::endl;
            std::cerr << "child stderr marker" << std::endl;
        }
        std::_Exit(37);
    }
} child_fixture;

fs::path selfPath() {
#if defined(_WIN32)
    std::wstring value(32768, L'\0');
    const DWORD length =
        GetModuleFileNameW(nullptr, value.data(), static_cast<DWORD>(value.size()));
    CHECK_PROCESS(length > 0 && length < value.size());
    value.resize(length);
    return value;
#elif defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string value(size, '\0');
    CHECK_PROCESS(_NSGetExecutablePath(value.data(), &size) == 0);
    return fs::canonical(value.c_str());
#else
    return fs::read_symlink("/proc/self/exe");
#endif
}

struct Temp {
    fs::path path = fs::temp_directory_path() /
                    ("didi-managed-process-" +
                     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Temp() { fs::create_directories(path); }
    ~Temp() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

void waitForExit(ManagedProcess& process) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (process.running() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK_PROCESS(!process.running());
}

void reportsArgumentsAndExit() {
    Temp temp;
    ManagedProcess process;
    const std::vector<std::string> values = {"",       "two words", "quote\"inside", "end slash\\",
                                             "a\\\"b", "a&b|c;$()"};
    std::vector<std::string> args = {"--didi-managed-child", "report"};
    args.insert(args.end(), values.begin(), values.end());
    CHECK_PROCESS(
        process.start(selfPath().string(), args, temp.path, temp.path / "child.log").isOk());
    CHECK_PROCESS(process.pid() != 0);
    waitForExit(process);
    CHECK_PROCESS(process.exitCode() == 37);
    std::ifstream log(temp.path / "child.log");
    std::string line;
    CHECK_PROCESS(static_cast<bool>(std::getline(log, line)));
    const auto report = didi::json::parse(line);
    CHECK_PROCESS(report["arguments"] == values);
    CHECK_PROCESS(fs::equivalent(report["cwd"].get<std::string>(), temp.path));
    CHECK_PROCESS(report["stdin_eof"] == true);
    CHECK_PROCESS(static_cast<bool>(std::getline(log, line)));
    CHECK_PROCESS(line == "child stderr marker");
    process.stop();
    CHECK_PROCESS(process.exitCode() == 37);
}

void stopsOnlyOwnedChild() {
    Temp temp;
    ManagedProcess survivor;
    CHECK_PROCESS(survivor
                      .start(selfPath().string(), {"--didi-managed-child", "hold"}, temp.path,
                             temp.path / "survivor.log")
                      .isOk());
    {
        ManagedProcess child;
        CHECK_PROCESS(child
                          .start(selfPath().string(), {"--didi-managed-child", "hold"}, temp.path,
                                 temp.path / "owned.log")
                          .isOk());
        CHECK_PROCESS(child.running());
        CHECK_PROCESS(!child.exitCode());
        const auto original_pid = child.pid();
        CHECK_PROCESS(
            child.start(selfPath().string(), {}, temp.path, temp.path / "second.log").isErr());
        CHECK_PROCESS(child.pid() == original_pid);
#if defined(_WIN32)
        HANDLE owned = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(original_pid));
        CHECK_PROCESS(owned != nullptr);
        child.stop();
        CHECK_PROCESS(WaitForSingleObject(owned, 0) == WAIT_OBJECT_0);
        CloseHandle(owned);
#else
        child.stop();
        int status;
        CHECK_PROCESS(waitpid(static_cast<pid_t>(original_pid), &status, WNOHANG) == -1 &&
                      errno == ECHILD);
#endif
        CHECK_PROCESS(!child.running());
        CHECK_PROCESS(child.exitCode().has_value());
        CHECK_PROCESS(child
                          .start(selfPath().string(), {"--didi-managed-child", "report"}, temp.path,
                                 temp.path / "restart.log")
                          .isOk());
        waitForExit(child);
    }
    CHECK_PROCESS(survivor.running());
    survivor.stop();
}

void rejectsInvalidLaunchInput() {
    Temp temp;
    ManagedProcess child;
    const auto executable = selfPath().string();
    CHECK_PROCESS(child.start("relative", {}, temp.path, temp.path / "log").isErr());
    CHECK_PROCESS(child.start(temp.path.string(), {}, temp.path, temp.path / "log").isErr());
    CHECK_PROCESS(
        child.start(executable + std::string("\0bad", 4), {}, temp.path, temp.path / "log")
            .isErr());
    CHECK_PROCESS(
        child.start(executable, {std::string("a\0b", 3)}, temp.path, temp.path / "log").isErr());
    CHECK_PROCESS(child.start(executable, {}, temp.path / "missing", temp.path / "log").isErr());
    CHECK_PROCESS(child.start(executable, {}, temp.path, temp.path).isErr());
    CHECK_PROCESS(child.start(executable, {}, temp.path, temp.path / "missing" / "log").isErr());
    CHECK_PROCESS(!child.running());
    CHECK_PROCESS(child.pid() == 0);
    CHECK_PROCESS(!child.exitCode());
}

void destructorReapsOwnedChild() {
    Temp temp;
    uint64_t child_pid = 0;
#if defined(_WIN32)
    HANDLE observed = nullptr;
#endif
    {
        ManagedProcess child;
        CHECK_PROCESS(child
                          .start(selfPath().string(), {"--didi-managed-child", "hold"}, temp.path,
                                 temp.path / "owned.log")
                          .isOk());
        child_pid = child.pid();
#if defined(_WIN32)
        observed = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(child_pid));
        CHECK_PROCESS(observed != nullptr);
#endif
    }
#if defined(_WIN32)
    const auto result = WaitForSingleObject(observed, 0);
    CloseHandle(observed);
    CHECK_PROCESS(result == WAIT_OBJECT_0);
#else
    int status;
    CHECK_PROCESS(waitpid(static_cast<pid_t>(child_pid), &status, WNOHANG) == -1 &&
                  errno == ECHILD);
#endif
}

void failedNativeLaunchLeavesObjectReusable() {
    Temp temp;
    const auto invalid = temp.path / "not-executable.bin";
    std::ofstream(invalid) << "this is not an executable";
    ManagedProcess child;
    CHECK_PROCESS(child.start(invalid.string(), {}, temp.path, temp.path / "failure.log").isErr());
    CHECK_PROCESS(!child.running());
    CHECK_PROCESS(child.pid() == 0);
    CHECK_PROCESS(!child.exitCode());
    CHECK_PROCESS(child
                      .start(selfPath().string(), {"--didi-managed-child", "report"}, temp.path,
                             temp.path / "success.log")
                      .isOk());
    waitForExit(child);
    CHECK_PROCESS(child.exitCode() == 37);
}

#if !defined(_WIN32)
void launchClosesDescriptorsAboveSoftLimit() {
    Temp temp;
    struct LimitGuard {
        struct rlimit previous {};
        ~LimitGuard() { setrlimit(RLIMIT_NOFILE, &previous); }
    } limit;
    CHECK_PROCESS(getrlimit(RLIMIT_NOFILE, &limit.previous) == 0);
    struct DescriptorGuard {
        int value = -1;
        ~DescriptorGuard() { if (value >= 0) close(value); }
    } low{open("/dev/null", O_RDONLY)};
    CHECK_PROCESS(low.value >= 0);
    DescriptorGuard high{fcntl(low.value, F_DUPFD, 128)};
    CHECK_PROCESS(high.value >= 128);
    auto reduced = limit.previous;
    reduced.rlim_cur = 64;
    CHECK_PROCESS(setrlimit(RLIMIT_NOFILE, &reduced) == 0);
    ManagedProcess child;
    auto started = child.start(selfPath().string(),
                              {"--didi-managed-child", "check_descriptor", std::to_string(high.value)},
                              temp.path, temp.path / "isolated.log");
    if (started.isErr())
        throw std::runtime_error(started.error().message);
    waitForExit(child);
    CHECK_PROCESS(child.exitCode() == 37);
    CHECK_PROCESS(fcntl(high.value, F_GETFD) >= 0);
}
#endif

struct RegisterManagedProcess {
    RegisterManagedProcess() {
        registerTest("ManagedProcess.ReportsArgumentsAndExit", reportsArgumentsAndExit);
        registerTest("ManagedProcess.StopsOnlyOwnedChild", stopsOnlyOwnedChild);
        registerTest("ManagedProcess.RejectsInvalidLaunchInput", rejectsInvalidLaunchInput);
        registerTest("ManagedProcess.DestructorReapsOwnedChild", destructorReapsOwnedChild);
        registerTest("ManagedProcess.FailedNativeLaunchLeavesObjectReusable",
                     failedNativeLaunchLeavesObjectReusable);
#if !defined(_WIN32)
        registerTest("ManagedProcess.LaunchClosesDescriptorsAboveSoftLimit",
                     launchClosesDescriptorsAboveSoftLimit);
#endif
    }
} register_managed_process;
} // namespace
