#include "didi/runtime/managed_process.hpp"
#include "didi/offline/process_runner.hpp"
#include "didi/offline/test_runner.hpp"
#include <filesystem>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <thread>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <crt_externs.h>
#include <csignal>
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#else
#include <csignal>
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
        // Long enough that outliving the reap deadline is the only way this
        // ends. A grandchild that merely finished its own sleep would make the
        // reaping test pass without anything having reaped it.
        if (args[2] == "hold_long")
            std::this_thread::sleep_for(std::chrono::seconds(120));
#if !defined(_WIN32)
        if (args[2] == "check_descriptor" && args.size() == 4) {
            const int descriptor = std::stoi(args[3]);
            std::_Exit(fcntl(descriptor, F_GETFD) == -1 && errno == EBADF ? 37 : 38);
        }
#endif
        // Stands in for a host that dies without running any of its own code.
        // It starts a managed child, publishes that child's pid, and then never
        // returns, so no destructor and no stop() can be what reaps it.
        if (args[2] == "orphan" && args.size() == 6) {
            static ManagedProcess grandchild;
            if (grandchild.start(args[0], {"--didi-managed-child", "hold_long"}, args[3], args[4])
                    .isOk()) {
                const fs::path published(args[5]);
                const fs::path partial(published.string() + ".partial");
                std::ofstream(partial) << grandchild.pid();
                std::error_code ec;
                fs::rename(partial, published, ec);
            }
            std::this_thread::sleep_for(std::chrono::seconds(60));
            std::_Exit(38);
        }
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

// Alive means running. A killed child on Linux is briefly a zombie, which is
// still a pid but is not a process doing anything, so it does not count.
bool processAlive(uint64_t pid) {
#if defined(_WIN32)
    HANDLE handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE,
                                static_cast<DWORD>(pid));
    if (!handle)
        return false;
    const bool alive = WaitForSingleObject(handle, 0) == WAIT_TIMEOUT;
    CloseHandle(handle);
    return alive;
#else
    if (kill(static_cast<pid_t>(pid), 0) != 0)
        return false;
#if defined(__linux__)
    std::ifstream input("/proc/" + std::to_string(pid) + "/stat");
    std::string line;
    if (std::getline(input, line)) {
        // The command field is parenthesised and may itself contain spaces and
        // brackets, so the state is read relative to the last close bracket.
        const auto command_end = line.rfind(')');
        if (command_end != std::string::npos && command_end + 2 < line.size())
            return line[command_end + 2] != 'Z';
    }
#endif
    return true;
#endif
}

// Kills without giving the target any chance to clean up, which is the point:
// a supervisor, a SIGKILL, or a second Ctrl+C taking the default action.
void hardKill(uint64_t pid) {
#if defined(_WIN32)
    HANDLE handle = OpenProcess(PROCESS_TERMINATE, FALSE, static_cast<DWORD>(pid));
    CHECK_PROCESS(handle != nullptr);
    CHECK_PROCESS(TerminateProcess(handle, 1) != 0);
    CloseHandle(handle);
#else
    CHECK_PROCESS(kill(static_cast<pid_t>(pid), SIGKILL) == 0);
#endif
}

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

void reapsOwnedChildWhenTheHostDiesAbnormally() {
    // Break caught: the child was reaped only by a destructor, so a host killed
    // rather than stopped left a headless editor running with nothing left that
    // knew about it.
#if defined(__APPLE__)
    // Darwin has no equivalent of a job object or PR_SET_PDEATHSIG, and the
    // spawn extension this platform needs rules out running code in the child.
    // The gap is recorded in docs/MANAGED_RECOVERY.md rather than pretended away.
    return;
#else
    Temp temp;
    const auto self = selfPath().string();
    const auto published = temp.path / "grandchild.pid";

    ManagedProcess host;
    CHECK_PROCESS(host
                      .start(self,
                             {"--didi-managed-child", "orphan", temp.path.string(),
                              (temp.path / "grandchild.log").string(), published.string()},
                             temp.path, temp.path / "host.log")
                      .isOk());

    uint64_t grandchild = 0;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (!grandchild && std::chrono::steady_clock::now() < deadline) {
        std::ifstream(published) >> grandchild;
        if (!grandchild)
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK_PROCESS(grandchild != 0);
    CHECK_PROCESS(processAlive(grandchild));

    // If nothing reaps it, do not leave it sleeping for two minutes.
    struct Cleanup {
        uint64_t pid;
        ~Cleanup() {
            if (processAlive(pid)) {
#if defined(_WIN32)
                if (HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, static_cast<DWORD>(pid))) {
                    TerminateProcess(h, 1);
                    CloseHandle(h);
                }
#else
                kill(static_cast<pid_t>(pid), SIGKILL);
#endif
            }
        }
    } cleanup{grandchild};

    // The host object stays alive here, so our own job handle stays open.
    // Anything that reaps the grandchild now is the host's own arrangement.
    hardKill(host.pid());

    // Well inside the grandchild's own lifetime, so surviving this window means
    // it was not reaped rather than that it had not got round to exiting.
    deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (processAlive(grandchild) && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK_PROCESS(!processAlive(grandchild));
#endif
}

static void offlineRunnerDoesNotHandChildrenTheServerStdin() {
    // The offline runner used to hand children this process's standard input.
    // Didi speaks JSON-RPC on stdio, so a child that reads stdin reads the
    // server's own requests, and two readers on one stream race whether or not
    // the child ever wants input.
    //
    // What this proves, and what it does not. A command that reads until end of
    // input now returns promptly instead of waiting for one that never comes.
    // On a machine whose own stdin is already at end of file the child would
    // return promptly either way, so this can pass without exercising the
    // redirect. It cannot pass while the bug is present on a machine with an
    // open stdin, which is the case that hangs a real session, and it never
    // fails spuriously. Distinguishing the mechanism would mean observing the
    // child's handle, which is not portable.
    didi::offline::ProcessRequest request;
    request.working_directory = std::filesystem::temp_directory_path();
    request.timeout = std::chrono::milliseconds(5000);
#if defined(_WIN32)
    request.executable = "cmd.exe";
    request.arguments = {"/c", "more"};
#else
    request.executable = "/bin/sh";
    request.arguments = {"-c", "cat"};
#endif

    const auto started = std::chrono::steady_clock::now();
    const auto result = didi::offline::runProcess(request);
    const auto elapsed = std::chrono::steady_clock::now() - started;

    CHECK_PROCESS(result.isOk());
    CHECK_PROCESS(!result.value().timed_out);
    CHECK_PROCESS(
        std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() < 4000);
}

static void testSessionTimeoutKillsTheWholeProcessTree() {
    // Break reported in #351: runSession spawned Godot with no job object on
    // Windows and no process group on POSIX, then killed a single process on
    // timeout. When Godot resolves to a godot.cmd wrapper the launch goes
    // through cmd.exe, so the thing being killed is the interpreter and the
    // engine keeps running detached.
    //
    // The wrapper is what makes this reproducible: without one, the spawned
    // process is the target and killing it directly is enough. So the test
    // points GODOT_BIN at a wrapper that starts a background grandchild which
    // publishes its own pid, and then checks the grandchild is gone once the
    // session has timed out.
#if defined(__APPLE__)
    // Same exemption the sibling test above takes, for the same reason.
    return;
#else
    Temp temp;
    const auto published = temp.path / "grandchild.pid";
    const auto published_text = published.generic_string();

#if defined(_WIN32)
    const auto wrapper = temp.path / "godot.cmd";
    {
        std::ofstream script(wrapper);
        script << "@echo off\n"
               << "start \"\" /b powershell -NoProfile -Command \"$PID | Out-File -Encoding ascii '"
               << published_text << "'; Start-Sleep -Seconds 90\"\n"
               << "powershell -NoProfile -Command \"Start-Sleep -Seconds 90\"\n";
    }
    _putenv_s("GODOT_BIN", wrapper.string().c_str());
#else
    const auto wrapper = temp.path / "godot.sh";
    {
        std::ofstream script(wrapper);
        script << "#!/bin/sh\n"
               << "sh -c 'echo $$ > \"" << published_text << "\"; sleep 90' &\n"
               << "sleep 90\n";
    }
    fs::permissions(wrapper, fs::perms::owner_all | fs::perms::group_read |
                                 fs::perms::group_exec);
    setenv("GODOT_BIN", wrapper.c_str(), 1);
#endif

    // Short, because the whole point is what survives the timeout.
    const auto result = didi::offline::TestRunner::runSession("res://none.tscn", 3, true, true, {});
    CHECK_PROCESS(result.exit_code == 124);

    uint64_t grandchild = 0;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (!grandchild && std::chrono::steady_clock::now() < deadline) {
        std::ifstream(published) >> grandchild;
        if (!grandchild) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    // If the wrapper never got far enough to publish a pid there is nothing to
    // assert about, and passing on that would be pretending. The wrapper has
    // ninety seconds of sleep ahead of it, so the only way the file is missing
    // is that the grandchild never started.
    CHECK_PROCESS(grandchild != 0);

    // The tree, not just the interpreter. Give the kernel a moment: a job
    // closing kills asynchronously.
    deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (processAlive(grandchild) && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK_PROCESS(!processAlive(grandchild));

#if defined(_WIN32)
    _putenv_s("GODOT_BIN", "");
#else
    unsetenv("GODOT_BIN");
#endif
#endif
}

struct RegisterManagedProcess {
    RegisterManagedProcess() {
        registerTest("ManagedProcess.ReportsArgumentsAndExit", reportsArgumentsAndExit);
        registerTest("ManagedProcess.StopsOnlyOwnedChild", stopsOnlyOwnedChild);
        registerTest("ManagedProcess.RejectsInvalidLaunchInput", rejectsInvalidLaunchInput);
        registerTest("ManagedProcess.DestructorReapsOwnedChild", destructorReapsOwnedChild);
        registerTest("ManagedProcess.ReapsOwnedChildWhenHostDiesAbnormally",
                     reapsOwnedChildWhenTheHostDiesAbnormally);
        registerTest("ManagedProcess.FailedNativeLaunchLeavesObjectReusable",
                     failedNativeLaunchLeavesObjectReusable);
        registerTest("ProcessRunner.ChildDoesNotInheritServerStdin",
                     offlineRunnerDoesNotHandChildrenTheServerStdin);
        registerTest("TestRunner.TimeoutKillsTheWholeProcessTree",
                     testSessionTimeoutKillsTheWholeProcessTree);
#if !defined(_WIN32)
        registerTest("ManagedProcess.LaunchClosesDescriptorsAboveSoftLimit",
                     launchClosesDescriptorsAboveSoftLimit);
#endif
    }
} register_managed_process;
} // namespace
