#include "didi/runtime/managed_process.hpp"
#include <cerrno>
#include <limits>
#if defined(_WIN32)
#include <windows.h>
#else
#include <csignal>
#include <fcntl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/syscall.h>
#elif defined(__APPLE__)
#include <crt_externs.h>
#include <spawn.h>
#endif
#endif

namespace didi::runtime {
namespace {
template <class String> bool hasNul(const String& value) {
    return value.find(typename String::value_type{}) != String::npos;
}
#if defined(_WIN32)
struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    ~Handle() {
        if (value && value != INVALID_HANDLE_VALUE)
            CloseHandle(value);
    }
};
Result<std::wstring> wide(const std::string& value) {
    if (value.empty())
        return std::wstring{};
    if (value.size() > static_cast<size_t>(std::numeric_limits<int>::max()))
        return Error::invalidArgument("Process argument too large");
    int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                   static_cast<int>(value.size()), nullptr, 0);
    if (!size)
        return Error::invalidArgument("Process argument must be valid UTF-8");
    std::wstring result(size, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
                        result.data(), size);
    return result;
}
std::wstring quote(const std::wstring& value) {
    std::wstring result(1, L'"');
    size_t slashes = 0;
    for (wchar_t c : value) {
        if (c == L'\\') {
            ++slashes;
            continue;
        }
        result.append(c == L'"' ? slashes * 2 + 1 : slashes, L'\\');
        result += c;
        slashes = 0;
    }
    result.append(slashes * 2, L'\\');
    result += L'"';
    return result;
}
#else
struct Descriptor {
    int value = -1;
    ~Descriptor() {
        if (value >= 0)
            close(value);
    }
};
int aboveStandardStreams(int fd) {
    if (fd < 0 || fd > STDERR_FILENO)
        return fd;
    int moved = fcntl(fd, F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
    close(fd);
    return moved;
}
int exitStatus(int status) {
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}
#endif
} // namespace

struct ManagedProcess::Impl {
    uint64_t pid = 0;
    std::optional<int> exit;
#if defined(_WIN32)
    Handle process;
#else
    pid_t owned_pid = 0;
    bool uncertain = false;
#endif
};
ManagedProcess::ManagedProcess() : impl_(std::make_unique<Impl>()) {}
ManagedProcess::~ManagedProcess() { stop(); }

Result<void> ManagedProcess::start(const std::string& executable,
                                   const std::vector<std::string>& arguments,
                                   const std::filesystem::path& cwd,
                                   const std::filesystem::path& log_path) {
    if (running())
        return Error::invalidArgument(
            "Managed child is already running or its state cannot be verified");
    if (executable.empty() || hasNul(executable) || hasNul(cwd.native()) ||
        hasNul(log_path.native()))
        return Error::invalidArgument("Managed process paths must be nonempty and contain no NUL");
    for (const auto& arg : arguments)
        if (hasNul(arg))
            return Error::invalidArgument("Managed process arguments cannot contain NUL");
#if defined(_WIN32)
    auto executable_wide = wide(executable);
    if (executable_wide.isErr())
        return executable_wide.error();
    const std::filesystem::path executable_path(executable_wide.value());
#else
    const std::filesystem::path executable_path(executable);
#endif
    std::error_code ec;
    if (!executable_path.is_absolute() || !std::filesystem::is_regular_file(executable_path, ec) ||
        ec)
        return Error::invalidArgument("Managed executable must be an absolute regular file");
    if (!cwd.is_absolute() || !std::filesystem::is_directory(cwd, ec) || ec)
        return Error::invalidArgument(
            "Managed working directory must be an existing absolute directory");
    if (!log_path.is_absolute() || log_path.filename().empty() ||
        !std::filesystem::is_directory(log_path.parent_path(), ec) || ec)
        return Error::invalidArgument(
            "Managed log must have an existing absolute parent directory");
    auto log_status = std::filesystem::symlink_status(log_path, ec);
    if (ec && ec != std::errc::no_such_file_or_directory)
        return Error::invalidArgument("Cannot inspect managed log path");
    if (std::filesystem::exists(log_status) && !std::filesystem::is_regular_file(log_status))
        return Error::invalidArgument(
            "Managed log must be a regular file, not a link or directory");
    ec.clear();
    if (std::filesystem::equivalent(executable_path, log_path, ec))
        return Error::invalidArgument("Managed log cannot be the executable");
#if defined(_WIN32)
    std::wstring command = quote(executable_wide.value());
    for (const auto& argument : arguments) {
        auto converted = wide(argument);
        if (converted.isErr())
            return converted.error();
        command += L' ';
        command += quote(converted.value());
    }
    if (command.size() >= 32767)
        return Error::invalidArgument("Managed command line exceeds Windows limit");
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    Handle log{CreateFileW(log_path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           &security, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
    if (log.value == INVALID_HANDLE_VALUE)
        return Error::internal("Cannot open managed process log");
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(log.value, &info) ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)))
        return Error::invalidArgument("Managed process log is not a plain file");
    Handle input{CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &security,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
    if (input.value == INVALID_HANDLE_VALUE)
        return Error::internal("Cannot open managed process input");

    SIZE_T size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    std::vector<unsigned char> storage(size);
    auto attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    if (!InitializeProcThreadAttributeList(attributes, 1, 0, &size))
        return Error::internal("Cannot initialize managed process handle isolation");
    struct AttributeGuard {
        LPPROC_THREAD_ATTRIBUTE_LIST value;
        ~AttributeGuard() { DeleteProcThreadAttributeList(value); }
    } attribute_guard{attributes};
    HANDLE inherited[] = {input.value, log.value};
    if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited,
                                   sizeof(inherited), nullptr, nullptr))
        return Error::internal("Cannot isolate managed process handles");
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    startup.StartupInfo.wShowWindow = SW_HIDE;
    startup.StartupInfo.hStdInput = input.value;
    startup.StartupInfo.hStdOutput = log.value;
    startup.StartupInfo.hStdError = log.value;
    startup.lpAttributeList = attributes;
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable_wide.value().c_str(), command.data(), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr, cwd.c_str(),
                        &startup.StartupInfo, &process))
        return Error::internal("Cannot start managed process (Windows error " +
                               std::to_string(GetLastError()) + ")");
    CloseHandle(process.hThread);
    if (impl_->process.value != INVALID_HANDLE_VALUE)
        CloseHandle(impl_->process.value);
    impl_->process.value = process.hProcess;
    impl_->pid = process.dwProcessId;
#else
    // Allocate everything before spawning; the fork child uses only safe calls.
    std::vector<char*> argv;
    argv.reserve(arguments.size() + 2);
    argv.push_back(const_cast<char*>(executable.c_str()));
    for (const auto& arg : arguments)
        argv.push_back(const_cast<char*>(arg.c_str()));
    argv.push_back(nullptr);
    const std::string directory = cwd.native();
    Descriptor log{aboveStandardStreams(
        open(log_path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK,
             0600))};
    if (log.value < 0)
        return Error::internal("Cannot open managed process log");
    struct stat log_info {};
    if (fstat(log.value, &log_info) != 0 || !S_ISREG(log_info.st_mode))
        return Error::invalidArgument("Managed process log is not a plain file");
    Descriptor input{aboveStandardStreams(open("/dev/null", O_RDONLY | O_CLOEXEC))};
    if (input.value < 0)
        return Error::internal("Cannot open managed process input");
#if defined(__APPLE__)
    // Darwin normally has an infinite hard RLIMIT_NOFILE. Its spawn extension
    // closes unlisted descriptors atomically, including descriptors above a
    // subsequently lowered soft limit, without guessing a close-loop bound.
    posix_spawn_file_actions_t actions;
    int spawn_error = posix_spawn_file_actions_init(&actions);
    if (spawn_error != 0)
        return Error::internal("Cannot initialize managed spawn file actions");
    struct ActionsGuard {
        posix_spawn_file_actions_t* value;
        ~ActionsGuard() { posix_spawn_file_actions_destroy(value); }
    } actions_guard{&actions};
    posix_spawnattr_t attributes;
    spawn_error = posix_spawnattr_init(&attributes);
    if (spawn_error != 0)
        return Error::internal("Cannot initialize managed spawn attributes");
    struct SpawnAttributesGuard {
        posix_spawnattr_t* value;
        ~SpawnAttributesGuard() { posix_spawnattr_destroy(value); }
    } attributes_guard{&attributes};
    if ((spawn_error = posix_spawnattr_setflags(&attributes, POSIX_SPAWN_CLOEXEC_DEFAULT)) != 0 ||
        (spawn_error = posix_spawn_file_actions_adddup2(&actions, input.value, STDIN_FILENO)) != 0 ||
        (spawn_error = posix_spawn_file_actions_adddup2(&actions, log.value, STDOUT_FILENO)) != 0 ||
        (spawn_error = posix_spawn_file_actions_adddup2(&actions, log.value, STDERR_FILENO)) != 0 ||
        (spawn_error = posix_spawn_file_actions_addchdir_np(&actions, directory.c_str())) != 0)
        return Error::internal("Cannot configure managed spawn (errno " +
                               std::to_string(spawn_error) + ")");
    pid_t child = 0;
    spawn_error = posix_spawn(&child, executable.c_str(), &actions, &attributes,
                             argv.data(), *_NSGetEnviron());
    if (spawn_error != 0)
        return Error::internal("Cannot execute managed process (errno " +
                               std::to_string(spawn_error) + ")");
#else
    int pipe_fds[2];
    if (pipe(pipe_fds) != 0)
        return Error::internal("Cannot create managed exec status pipe");
    Descriptor read_error{aboveStandardStreams(pipe_fds[0])};
    Descriptor write_error{aboveStandardStreams(pipe_fds[1])};
    if (read_error.value < 0 || write_error.value < 0 ||
        fcntl(read_error.value, F_SETFD, FD_CLOEXEC) < 0 ||
        fcntl(write_error.value, F_SETFD, FD_CLOEXEC) < 0)
        return Error::internal("Cannot isolate managed exec status pipe");
    struct rlimit limit {};
    if (getrlimit(RLIMIT_NOFILE, &limit) != 0 || limit.rlim_max == RLIM_INFINITY)
        return Error::internal("Cannot determine safe descriptor closing limit");
    const int descriptor_limit =
        static_cast<int>(std::min<rlim_t>(limit.rlim_max, std::numeric_limits<int>::max()));
    const pid_t child = fork();
    if (child < 0)
        return Error::internal("Cannot fork managed process");
    if (child == 0) {
        int failure = 0;
        if (chdir(directory.c_str()) != 0 || dup2(input.value, STDIN_FILENO) < 0 ||
            dup2(log.value, STDOUT_FILENO) < 0 || dup2(log.value, STDERR_FILENO) < 0)
            failure = errno;
        // Preserve only the exec error descriptor and standard streams.
        bool closed = false;
#if defined(__linux__) && defined(SYS_close_range)
        const bool lower_closed = write_error.value == STDERR_FILENO + 1 ||
                                  syscall(SYS_close_range, static_cast<unsigned>(STDERR_FILENO + 1),
                                          static_cast<unsigned>(write_error.value - 1), 0) == 0;
        const bool upper_closed =
            syscall(SYS_close_range, static_cast<unsigned>(write_error.value + 1),
                    std::numeric_limits<unsigned>::max(), 0) == 0;
        closed = lower_closed && upper_closed;
#endif
        if (!closed)
            for (int fd = STDERR_FILENO + 1; fd < descriptor_limit; ++fd)
                if (fd != write_error.value)
                    close(fd);
        if (!failure) {
            execv(executable.c_str(), argv.data());
            failure = errno;
        }
        while (write(write_error.value, &failure, sizeof(failure)) < 0 && errno == EINTR) {
        }
        _exit(127);
    }
    close(write_error.value);
    write_error.value = -1;
    int failure = 0;
    ssize_t bytes;
    do {
        bytes = read(read_error.value, &failure, sizeof(failure));
    } while (bytes < 0 && errno == EINTR);
    if (bytes != 0) {
        kill(child, SIGKILL);
        while (waitpid(child, nullptr, 0) < 0 && errno == EINTR) {
        }
        return Error::internal("Cannot execute managed process (errno " + std::to_string(failure) +
                               ")");
    }
#endif
    impl_->owned_pid = child;
    impl_->pid = static_cast<uint64_t>(child);
    impl_->uncertain = false;
#endif
    impl_->exit.reset();
    return Result<void>::ok();
}

bool ManagedProcess::running() {
    if (impl_->exit)
        return false;
#if defined(_WIN32)
    if (impl_->process.value == INVALID_HANDLE_VALUE)
        return false;
    if (WaitForSingleObject(impl_->process.value, 0) != WAIT_OBJECT_0)
        return true;
    DWORD code;
    if (!GetExitCodeProcess(impl_->process.value, &code))
        return true;
    impl_->exit = static_cast<int>(code);
#else
    if (!impl_->owned_pid)
        return impl_->uncertain;
    int status = 0;
    pid_t observed;
    do {
        observed = waitpid(impl_->owned_pid, &status, WNOHANG);
    } while (observed < 0 && errno == EINTR);
    if (observed == 0)
        return true;
    if (observed < 0) {
        // An external reaper may have released the PID; never signal it again.
        if (errno == ECHILD) {
            impl_->owned_pid = 0;
            impl_->uncertain = true;
        }
        return true;
    }
    impl_->exit = exitStatus(status);
    impl_->owned_pid = 0;
#endif
    return false;
}

uint64_t ManagedProcess::pid() const { return impl_->pid; }
std::optional<int> ManagedProcess::exitCode() {
    running();
    return impl_->exit;
}

void ManagedProcess::stop() {
    if (!running())
        return;
#if defined(_WIN32)
    if (TerminateProcess(impl_->process.value, 1)) {
        WaitForSingleObject(impl_->process.value, INFINITE);
        running();
    }
#else
    if (!impl_->owned_pid)
        return;
    if (kill(impl_->owned_pid, SIGKILL) == 0 || errno == ESRCH) {
        int status = 0;
        pid_t observed;
        do {
            observed = waitpid(impl_->owned_pid, &status, 0);
        } while (observed < 0 && errno == EINTR);
        if (observed == impl_->owned_pid)
            impl_->exit = exitStatus(status);
        else
            impl_->uncertain = true;
        impl_->owned_pid = 0;
    }
#endif
}
} // namespace didi::runtime
