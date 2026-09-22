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
#include <cerrno>
#include <crt_externs.h>
#include <csignal>
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#else
#include <cerrno>
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
        // Publishes its own pid and then holds. Used as the grandchild in the
        // orphan test: the wrapper starts this in the background, so the pid
        // file appears within milliseconds of launch rather than after an
        // interpreter has finished starting up.
        if (args[2] == "publish_self_and_hold" && args.size() == 4) {
            const fs::path published(args[3]);
            const fs::path partial(published.string() + ".partial");
            {
#if defined(_WIN32)
                std::ofstream(partial) << static_cast<uint64_t>(GetCurrentProcessId());
#else
                std::ofstream(partial) << static_cast<uint64_t>(getpid());
#endif
            }
            std::error_code ec;
            fs::rename(partial, published, ec);
            std::this_thread::sleep_for(std::chrono::seconds(120));
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

static void offlineRunnerTimeoutKillsTheWholeProcessTree() {
    // The README states the Phase 5 contract as fact: these tools "terminate
    // the child process group on timeout". process_runner.cpp is what backs
    // that for csharp_check_build, shader_check_compile, project_export,
    // gridmap_export_mesh_library and speculative verification's git and engine
    // runs, and it had neither guard the sibling spawner in test_runner.cpp
    // grew in #351 (#758). Nothing
    // covered the contract at all, so a timeout could leave a dotnet build's
    // MSBuild worker nodes running and answer 124 as though it had not.
    //
    // A wrapper, because without one the spawned process is the target and
    // killing it directly is enough. cmd.exe or /bin/sh starts a background
    // grandchild that publishes its own pid and holds; the grandchild is what
    // has to be gone.
    //
    // What this proves, and what it does not. It proves the timeout reaches a
    // descendant rather than the one process this call started, so removing the
    // job object or the group signal fails it. It does not reproduce the
    // assignment race itself: the window is between CreateProcessW returning
    // and AssignProcessToJobObject, which is a few microseconds of parent work
    // against a child that has not finished loading, so the parent wins on any
    // machine. CREATE_SUSPENDED closes that window by construction rather than
    // by timing, which is why the fix is the guard and not a retry.
    Temp temp;
    const auto published = temp.path / "runner-grandchild.pid";
    const auto published_text = published.generic_string();
    const auto self = selfPath().string();

    didi::offline::ProcessRequest request;
    request.working_directory = temp.path;
    request.timeout = std::chrono::milliseconds(6000);

#if defined(_WIN32)
    const auto wrapper = temp.path / "spawner.cmd";
    {
        std::ofstream script(wrapper);
        script << "@echo off\n"
               << "start \"\" /b \"" << self << "\" --didi-managed-child "
               << "publish_self_and_hold \"" << published_text << "\"\n"
               << "\"" << self << "\" --didi-managed-child hold_long\n";
    }
    request.executable = "cmd.exe";
    request.arguments = {"/c", wrapper.string()};
#else
    const auto wrapper = temp.path / "spawner.sh";
    {
        std::ofstream script(wrapper);
        script << "#!/bin/sh\n"
               << "\"" << self << "\" --didi-managed-child publish_self_and_hold \""
               << published_text << "\" &\n"
               << "\"" << self << "\" --didi-managed-child hold_long\n";
    }
    fs::permissions(wrapper, fs::perms::owner_all | fs::perms::group_read |
                                 fs::perms::group_exec);
    request.executable = "/bin/sh";
    request.arguments = {wrapper.string()};
#endif

    const auto ran = didi::offline::runProcess(request);
    CHECK_PROCESS(ran.isOk());
    CHECK_PROCESS(ran.value().timed_out);
    CHECK_PROCESS(ran.value().exit_code == 124);

    uint64_t grandchild = 0;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (!grandchild && std::chrono::steady_clock::now() < deadline) {
        std::ifstream(published) >> grandchild;
        if (!grandchild) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    // Nothing to assert about if the wrapper never got far enough to publish,
    // and passing on that would be pretending. The grandchild holds for two
    // minutes, so a missing file means it never started.
    CHECK_PROCESS(grandchild != 0);

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

#if !defined(_WIN32)
    // The parent's own setpgid ran, or returned EACCES because the child had
    // already exec'd, which means the child's call had succeeded. Either way
    // the group the timeout signals exists. This is the half of the fix that
    // can be observed directly.
    CHECK_PROCESS(ran.value().contained);
#endif

    // Asserted whatever `contained` says, and not skipped when it is false.
    // A host that runs this process inside a job with breakaway restricted can
    // refuse the assignment, and on that host nothing can reach a grandchild
    // and this fails. Letting it pass instead would let a removed job object
    // pass with it, which is the thing being guarded, and a test that cannot
    // fail on the break it is named after is worth nothing. The sibling test
    // below takes the same position.
    //
    // Bounded rather than immediate. runProcess makes no promise that the tree
    // has finished going by the time it answers -- TerminateJobObject and
    // SIGKILL both start a termination rather than complete one -- so the
    // honest assertion is that the tree goes, well inside the two minutes the
    // grandchild would otherwise hold for.
    deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (processAlive(grandchild) && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK_PROCESS(!processAlive(grandchild));
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

    // The wrapper drives this same test binary rather than an interpreter.
    // The first version used PowerShell for the grandchild and failed on CI
    // with "grandchild != 0": the session timeout killed the tree before
    // PowerShell had finished starting, so the pid file never appeared. That
    // guard doing its job is the only reason it was not a silent pass.
    const auto self = selfPath().string();

#if defined(_WIN32)
    const auto wrapper = temp.path / "godot.cmd";
    {
        std::ofstream script(wrapper);
        script << "@echo off\n"
               << "start \"\" /b \"" << self << "\" --didi-managed-child "
               << "publish_self_and_hold \"" << published_text << "\"\n"
               << "\"" << self << "\" --didi-managed-child hold_long\n";
    }
    _putenv_s("GODOT_BIN", wrapper.string().c_str());
#else
    const auto wrapper = temp.path / "godot.sh";
    {
        std::ofstream script(wrapper);
        script << "#!/bin/sh\n"
               << "\"" << self << "\" --didi-managed-child publish_self_and_hold \""
               << published_text << "\" &\n"
               << "\"" << self << "\" --didi-managed-child hold_long\n";
    }
    fs::permissions(wrapper, fs::perms::owner_all | fs::perms::group_read |
                                 fs::perms::group_exec);
    setenv("GODOT_BIN", wrapper.c_str(), 1);
#endif

    // Short, because the whole point is what survives the timeout.
    const auto result = didi::offline::TestRunner::runSession("res://none.tscn", 6, true, true, {});
    CHECK_PROCESS(result.exit_code == 124);
    CHECK_PROCESS(result.timed_out);

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

    // The tree, not just the interpreter.
#if defined(_WIN32)
    // And gone by the time runSession returns, with no wait here at all.
    //
    // The generous wait this used to do is the thing that hid #732: a job with
    // KILL_ON_JOB_CLOSE terminates asynchronously when the last handle closes,
    // so the tree did die -- a moment after the tool had already answered. The
    // documented flow is launch, then runtime_list_sessions, then
    // runtime_attach_session, and that sequence lands inside the window every
    // time: the list reported the game alive and not stale, truthfully, and the
    // attach a call later could not connect to it. A caller reading `alive` is
    // entitled to assume the tool has finished what it started, so the timeout
    // path terminates the job and waits for it to empty, and this asserts that
    // rather than waiting for it.
    //
    // The job is what makes that reachable, and a host that runs this process
    // inside a job of its own with breakaway restricted can refuse the
    // assignment. Nothing can reach a grandchild then, so asserting it would be
    // asserting something the code cannot do rather than something it failed to
    // do. The bounded wait below is what is left to check there, and `contained`
    // says which of the two ran.
    //
    // The wait is bounded at five seconds, and a loaded runner reaches that
    // bound where an idle one does not: this assertion failed on a pull request
    // about tilemap coordinate shapes, passed on the branch that introduced it,
    // on main afterwards and on a local build of the same tree (#755). An
    // assertion that turns CI load into a red branch on somebody else's work
    // stops being read. So the strong form is asserted when the runner says the
    // job emptied, which is the claim #732 is about, and when it says it gave
    // up the generous poll below still requires the tree to be gone -- a kill
    // that does not work at all fails either way. A query that failed is not
    // load and is not excused: nothing is known about the tree then, and that
    // is a broken handle rather than a busy machine.
    CHECK_PROCESS(result.kill_wait != didi::offline::TestSessionResult::KillWait::QueryFailed);
    if (result.contained &&
        result.kill_wait == didi::offline::TestSessionResult::KillWait::TreeExited) {
        CHECK_PROCESS(!processAlive(grandchild));
    } else {
        deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        while (processAlive(grandchild) && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        CHECK_PROCESS(!processAlive(grandchild));
    }

    // Whichever branch ran, the run that timed out inside a job must have
    // waited on the kill and said so. NotAttempted here would mean the wait was
    // skipped entirely, which is the state this field exists to make visible.
    if (result.contained) {
        CHECK_PROCESS(result.kill_wait !=
                      didi::offline::TestSessionResult::KillWait::NotAttempted);
    }
#else
    // POSIX kills the process group in the timeout path, and the group members
    // are reaped by init rather than by the runner, so a moment of slack is the
    // honest bound here. The Windows assertion above is the one about #732.
    deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (processAlive(grandchild) && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK_PROCESS(!processAlive(grandchild));
#endif

#if defined(_WIN32)
    _putenv_s("GODOT_BIN", "");
#else
    unsetenv("GODOT_BIN");
#endif
#endif
}

#if defined(_WIN32)
static void detachedSessionDoesNotInheritServerHandles() {
    // Break found running the live harness on #760: a detached launch spawns
    // with bInheritHandles=TRUE, which hands the child every inheritable handle
    // this process holds and not merely the ones STARTUPINFO names. A blocking
    // run hides it, because the parent waits and the child's copies die first.
    // A detached game outlives the call, so it keeps the server's own MCP
    // stdout open and the client never sees end of input: the harness sat for
    // nine minutes reading a pipe whose server had already exited cleanly, on
    // all three engines.
    //
    // A pipe of our own stands in for that stdout. Any inheritable handle will
    // do, because what is wrong is that the inheritance is unrestricted, and
    // observing the handle directly is the only way to tell the fix from a
    // child that happened to exit.
    Temp temp;
    const auto self = selfPath().string();
    const auto wrapper = temp.path / "godot.cmd";
    {
        std::ofstream script(wrapper);
        script << "@echo off\n"
               << "\"" << self << "\" --didi-managed-child hold_long\n";
    }
    _putenv_s("GODOT_BIN", wrapper.string().c_str());

    SECURITY_ATTRIBUTES sa;
    ZeroMemory(&sa, sizeof(sa));
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE read_end = INVALID_HANDLE_VALUE;
    HANDLE write_end = INVALID_HANDLE_VALUE;
    CHECK_PROCESS(CreatePipe(&read_end, &write_end, &sa, 0) != 0);

    const auto result =
        didi::offline::TestRunner::runSession("res://none.tscn", 6, true, true, {}, true);
    CHECK_PROCESS(result.detached);
    CHECK_PROCESS(result.pid != 0);

    // After this the only copy that can still exist is one the spawn leaked.
    CloseHandle(write_end);

    // PeekNamedPipe rather than a read, because a read on a pipe nobody closed
    // is exactly the hang this is about.
    bool broken = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!broken && std::chrono::steady_clock::now() < deadline) {
        DWORD available = 0;
        if (!PeekNamedPipe(read_end, nullptr, 0, nullptr, &available, nullptr)) {
            broken = GetLastError() == ERROR_BROKEN_PIPE;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    CloseHandle(read_end);

    // The game is detached by design, so nothing else will end it.
    if (HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, static_cast<DWORD>(result.pid))) {
        TerminateProcess(h, 1);
        CloseHandle(h);
    }
    _putenv_s("GODOT_BIN", "");

    CHECK_PROCESS(broken);
}
#endif

#if !defined(_WIN32)
static void detachedSessionIsNotLeftAsAChildOfTheServer() {
    // Break reported in #786: the POSIX detached launch forked once and
    // returned, so the game stayed a child of a server that by definition
    // never waits for it. The engine exits in half a second and the pid then
    // sits in the process table as a zombie until the server itself exits.
    // An agent loop that launches a game per change does that a few hundred
    // times in one session, and Windows has no equivalent because a process
    // that exits there leaves nothing behind once its handles are closed.
    //
    // The assertion is ownership rather than the zombie. Ownership is what was
    // wrong, it is readable while the game is still running, and it does not
    // depend on how promptly whoever adopts the orphan gets round to reaping
    // it, which is a property of the host rather than of this fix. A process
    // this one did not fork cannot be waited for: before the fix waitpid
    // answers 0 for a live child of ours, after it ECHILD.
    Temp temp;
    const auto self = selfPath().string();
    const auto wrapper = temp.path / "godot.sh";
    {
        std::ofstream script(wrapper);
        script << "#!/bin/sh\n"
               << "exec \"" << self << "\" --didi-managed-child hold_long\n";
    }
    fs::permissions(wrapper, fs::perms::owner_all | fs::perms::group_read |
                                 fs::perms::group_exec);
    setenv("GODOT_BIN", wrapper.c_str(), 1);

    const auto result =
        didi::offline::TestRunner::runSession("res://none.tscn", 6, true, true, {}, true);
    unsetenv("GODOT_BIN");
    CHECK_PROCESS(result.detached);
    CHECK_PROCESS(result.success);
    CHECK_PROCESS(result.pid != 0);

    // It has to be running for the question to mean anything: waitpid answers
    // ECHILD both for a pid that was never ours and for one already reaped,
    // and only the first of those is the fix.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!processAlive(result.pid) && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK_PROCESS(processAlive(result.pid));

    errno = 0;
    const pid_t observed = waitpid(static_cast<pid_t>(result.pid), nullptr, WNOHANG);
    const int failure = errno;

    // The game is detached by design, so nothing else will end it.
    hardKill(result.pid);

    CHECK_PROCESS(observed == -1);
    CHECK_PROCESS(failure == ECHILD);
}
#endif

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
        registerTest("ProcessRunner.TimeoutKillsTheWholeProcessTree",
                     offlineRunnerTimeoutKillsTheWholeProcessTree);
        registerTest("TestRunner.TimeoutKillsTheWholeProcessTree",
                     testSessionTimeoutKillsTheWholeProcessTree);
#if defined(_WIN32)
        registerTest("TestRunner.DetachedSessionDoesNotInheritServerHandles",
                     detachedSessionDoesNotInheritServerHandles);
#endif
#if !defined(_WIN32)
        registerTest("ManagedProcess.LaunchClosesDescriptorsAboveSoftLimit",
                     launchClosesDescriptorsAboveSoftLimit);
        registerTest("TestRunner.DetachedSessionIsNotLeftAsAChildOfTheServer",
                     detachedSessionIsNotLeftAsAChildOfTheServer);
#endif
    }
} register_managed_process;
} // namespace
