#include "didi/runtime/session_client.hpp"
#include "didi/runtime/session_lock.hpp"
#include "didi/common/secure_random.hpp"
#include "didi/common/project_path.hpp"
#include "didi/common/logger.hpp"

#include <algorithm>
#include <unordered_map>
#include <array>
#include <cerrno>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <cmath>
#include <mutex>
#include <optional>
#include <limits>
#include <sstream>
#include <set>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#include <bcrypt.h>
#elif defined(__APPLE__)
#include <fcntl.h>
#include <libproc.h>
#include <signal.h>
#include <sys/attr.h>
#include <sys/stat.h>
#include <unistd.h>
#else
#include <fcntl.h>
#include <linux/fs.h>
#include <signal.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace didi::runtime {

namespace {

std::filesystem::path absoluteLexicalPath(const std::filesystem::path& path) {
    std::error_code error;
    const auto absolute = path.is_absolute() ? path : std::filesystem::absolute(path, error);
    return (error ? path : absolute).lexically_normal();
}

} // namespace

Result<std::filesystem::path> resolveSessionDescriptorDirectory() {
    const char* configured = std::getenv("DIDI_SESSION_DIR");
    if (configured && *configured) return absoluteLexicalPath(configured);

    std::error_code error;
#if !defined(_WIN32)
    const char* xdg_runtime = std::getenv("XDG_RUNTIME_DIR");
    if (xdg_runtime && *xdg_runtime) {
        const std::filesystem::path root(xdg_runtime);
        if (root.is_absolute()) return root.lexically_normal() / "didi-sessions";
    }
#endif
    const auto temporary = std::filesystem::temp_directory_path(error);
    if (error) {
        return Error::internal("Unable to resolve the system temporary directory: " + error.message());
    }
#if defined(_WIN32)
    return absoluteLexicalPath(temporary / "didi-sessions");
#else
    return absoluteLexicalPath(temporary / ("didi-sessions-" + std::to_string(geteuid())));
#endif
}

Result<ProcessIdentity> queryProcessIdentity(uint64_t pid) {
    if (pid == 0) return Error::invalidArgument("Process identity requires a non-zero PID");
#if defined(_WIN32)
    if (pid > std::numeric_limits<DWORD>::max()) {
        return Error::notFound("Process identity is unavailable");
    }
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE,
                                 static_cast<DWORD>(pid));
    if (!process) return Error::notFound("Process identity is unavailable");
    FILETIME created{}, exited{}, kernel{}, user{};
    // The wait handle is the only honest liveness test. GetExitCodeProcess
    // hands back 259 both for a process that is still running and for one that
    // exited with 259, and GetProcessTimes keeps answering for a corpse, so
    // reading the exit code reports a dead engine as alive.
    const bool running = WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
    const bool has_times = GetProcessTimes(process, &created, &exited, &kernel, &user) != 0;
    CloseHandle(process);
    if (!running || !has_times) return Error::notFound("Process identity is unavailable");
    ULARGE_INTEGER ticks{};
    ticks.LowPart = created.dwLowDateTime;
    ticks.HighPart = created.dwHighDateTime;
    constexpr uint64_t kWindowsEpochOffsetMs = 11644473600000ULL;
    return ProcessIdentity{
        static_cast<int64_t>(ticks.QuadPart / 10000ULL - kWindowsEpochOffsetMs), 1};
#elif defined(__linux__)
    if (pid > static_cast<uint64_t>(std::numeric_limits<pid_t>::max())) {
        return Error::notFound("Process identity is unavailable");
    }
    if (kill(static_cast<pid_t>(pid), 0) != 0 && errno != EPERM) {
        return Error::notFound("Process identity is unavailable");
    }
    std::ifstream stat("/proc/" + std::to_string(pid) + "/stat");
    std::ifstream system_stat("/proc/stat");
    std::string stat_line;
    if (!std::getline(stat, stat_line)) return Error::notFound("Process identity is unavailable");
    const auto command_end = stat_line.rfind(')');
    if (command_end == std::string::npos) return Error::internal("Malformed /proc process identity");
    std::istringstream fields(stat_line.substr(command_end + 2));
    std::string field;
    uint64_t start_ticks = 0;
    for (int index = 0; index <= 19; ++index) {
        if (!(fields >> field)) return Error::internal("Malformed /proc process identity");
        if (index == 19) {
            try { start_ticks = std::stoull(field); }
            catch (...) { return Error::internal("Malformed /proc process identity"); }
        }
    }
    int64_t boot_seconds = 0;
    std::string line;
    while (std::getline(system_stat, line)) {
        if (line.rfind("btime ", 0) == 0) {
            try { boot_seconds = std::stoll(line.substr(6)); }
            catch (...) { return Error::internal("Malformed /proc boot identity"); }
            break;
        }
    }
    const long ticks_per_second = sysconf(_SC_CLK_TCK);
    if (boot_seconds <= 0 || ticks_per_second <= 0) {
        return Error::internal("Process start identity resolution is unavailable");
    }
    const int64_t resolution_ms = std::max<int64_t>(
        1, (1000 + ticks_per_second - 1) / ticks_per_second);
    return ProcessIdentity{
        boot_seconds * 1000 + static_cast<int64_t>(start_ticks * 1000ULL /
                                                    static_cast<uint64_t>(ticks_per_second)),
        resolution_ms};
#elif defined(__APPLE__)
    if (pid > static_cast<uint64_t>(std::numeric_limits<pid_t>::max())) {
        return Error::notFound("Process identity is unavailable");
    }
    proc_bsdinfo info{};
    if (proc_pidinfo(static_cast<int>(pid), PROC_PIDTBSDINFO, 0,
                     &info, sizeof(info)) != sizeof(info)) {
        return Error::notFound("Process identity is unavailable");
    }
    return ProcessIdentity{static_cast<int64_t>(info.pbi_start_tvsec) * 1000 +
                               static_cast<int64_t>(info.pbi_start_tvusec) / 1000,
                           1};
#else
    return Error::internal("Process start identity is unsupported on this platform");
#endif
}

namespace {

constexpr uintmax_t kMaxDescriptorBytes = 64 * 1024;
constexpr int kSessionHandshakeTimeoutMs = 3000;

#if defined(_WIN32)
class ScopedNativeHandle {
public:
    explicit ScopedNativeHandle(HANDLE handle) : m_handle(handle) {}
    ~ScopedNativeHandle() {
        if (m_handle && m_handle != INVALID_HANDLE_VALUE) CloseHandle(m_handle);
    }
    ScopedNativeHandle(const ScopedNativeHandle&) = delete;
    ScopedNativeHandle& operator=(const ScopedNativeHandle&) = delete;
    HANDLE get() const { return m_handle; }

private:
    HANDLE m_handle{INVALID_HANDLE_VALUE};
};
#else
class ScopedNativeHandle {
public:
    explicit ScopedNativeHandle(int descriptor) : m_descriptor(descriptor) {}
    ~ScopedNativeHandle() {
        if (m_descriptor >= 0) close(m_descriptor);
    }
    ScopedNativeHandle(const ScopedNativeHandle&) = delete;
    ScopedNativeHandle& operator=(const ScopedNativeHandle&) = delete;
    int get() const { return m_descriptor; }

private:
    int m_descriptor{-1};
};
#endif

bool isLowerHex(const std::string& value, size_t length) {
    return value.size() == length && std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

// build_id is optional and every other field is required. It has to be
// optional: an extension built before the field existed publishes a descriptor
// without it, and refusing that descriptor would turn a stale bridge from a
// thing the dashboard can report into a session nothing can reach at all.
bool hasOnlyDescriptorFields(const json& value) {
    static const std::set<std::string> required = {
        "schema_version", "session_id", "token", "pid", "kind", "project_path",
        "endpoint", "started_at_ms", "protocol_version"
    };
    static const std::set<std::string> optional = {"build_id"};
    for (auto it = value.begin(); it != value.end(); ++it) {
        if (!required.count(it.key()) && !optional.count(it.key())) return false;
    }
    for (const auto& field : required) {
        if (!value.contains(field)) return false;
    }
    return true;
}

// A build identity is printed on a dashboard and compared against this
// server's, so it is bounded and kept to characters that cannot break either.
bool validBuildId(const std::string& value) {
    if (value.empty() || value.size() > 128) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
               (c >= 'A' && c <= 'Z') || c == '.' || c == '+' || c == '-' || c == '_';
    });
}

bool validEndpoint(const std::string& endpoint, uint64_t pid, const std::string& session_id,
                   const std::string& project_path) {
    if (endpoint.empty() || endpoint.find_first_of("\r\n") != std::string::npos) return false;
    const auto legacy_stem = "godot_didi_" + std::to_string(pid) + "_" + session_id;
    // Same conversion the publisher uses, or the two hash different bytes for a
    // non-ASCII project path and the endpoint never validates.
    const auto project_stem =
        "godot_didi_" + paths::projectEndpointKey(paths::projectPathFromUtf8(project_path)) + "_" +
                              std::to_string(pid) + "_";
#if defined(_WIN32)
    return endpoint == "\\\\.\\pipe\\" + legacy_stem ||
           endpoint == "\\\\.\\pipe\\" + project_stem + session_id;
#else
    const std::filesystem::path path(endpoint);
    if (!path.is_absolute() ||
        (path.filename() != legacy_stem + ".sock" &&
         path.filename() != project_stem + session_id.substr(0, 12) + ".sock")) return false;
    std::error_code endpoint_error;
    std::error_code temp_error;
    const auto endpoint_parent = std::filesystem::weakly_canonical(path.parent_path(), endpoint_error);
    const auto temp_directory = std::filesystem::temp_directory_path(temp_error);
    if (temp_error) return false;
    const auto expected_parent = std::filesystem::weakly_canonical(temp_directory, temp_error);
    return !endpoint_error && !temp_error && endpoint_parent == expected_parent;
#endif
}

std::filesystem::path canonicalPath(const std::filesystem::path& path) {
    std::error_code ec;
    const auto canonical = std::filesystem::weakly_canonical(path, ec);
    return ec ? path.lexically_normal() : canonical;
}

Result<void> validateDescriptorDirectory(const std::filesystem::path& directory) {
#if defined(_WIN32)
    std::error_code error;
    if (!std::filesystem::is_directory(directory, error) || error) {
        return Error::notFound("Session descriptor directory is not readable");
    }
#else
    ScopedNativeHandle directory_fd(open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (directory_fd.get() < 0) {
        return Error::notFound("Session descriptor directory is not readable");
    }
    struct stat info{};
    if (fstat(directory_fd.get(), &info) != 0 || !S_ISDIR(info.st_mode) ||
        info.st_uid != geteuid() || (info.st_mode & (S_IRWXG | S_IRWXO)) != 0) {
        return Error::invalidArgument("Session descriptor directory must be owner-only");
    }
#endif
    return Result<void>::ok();
}

struct DescriptorFileIdentity {
    uint64_t first{0};
    uint64_t second{0};
};

bool operator==(const DescriptorFileIdentity& left, const DescriptorFileIdentity& right) {
    return left.first == right.first && left.second == right.second;
}

struct OwnedDescriptorSnapshot {
    DescriptorFileIdentity identity;
};

bool descriptorMatchesOwner(const std::string& contents, const SessionDescriptor& expected) {
    try {
        const auto decoded = SessionDescriptor::fromJson(json::parse(contents));
        return decoded.isOk() && decoded.value().session_id == expected.session_id &&
               decoded.value().token == expected.token && decoded.value().pid == expected.pid &&
               decoded.value().started_at_ms == expected.started_at_ms;
    } catch (const std::exception&) {
        return false;
    }
}

#if defined(_WIN32)
Result<std::pair<std::string, DescriptorFileIdentity>> readSecureDescriptorHandle(HANDLE handle) {
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    BY_HANDLE_FILE_INFORMATION identity{};
    LARGE_INTEGER size{};
    if (!GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &attributes,
                                      sizeof(attributes)) ||
        (attributes.FileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) != 0 ||
        GetFileType(handle) != FILE_TYPE_DISK || !GetFileInformationByHandle(handle, &identity) ||
        !GetFileSizeEx(handle, &size) || size.QuadPart < 0 ||
        static_cast<uint64_t>(size.QuadPart) > kMaxDescriptorBytes) {
        return Error::invalidArgument("Descriptor handle is not a secure regular file");
    }
    LARGE_INTEGER beginning{};
    if (!SetFilePointerEx(handle, beginning, nullptr, FILE_BEGIN)) {
        return Error::internal("Descriptor seek failed");
    }
    std::string contents(static_cast<size_t>(size.QuadPart), '\0');
    size_t offset = 0;
    while (offset < contents.size()) {
        DWORD count = 0;
        const auto remaining = static_cast<DWORD>(std::min<size_t>(
            contents.size() - offset, std::numeric_limits<DWORD>::max()));
        if (!ReadFile(handle, contents.data() + offset, remaining, &count, nullptr) || count == 0) {
            return Error::internal("Descriptor read failed");
        }
        offset += count;
    }
    return std::make_pair(
        std::move(contents),
        DescriptorFileIdentity{identity.dwVolumeSerialNumber,
                               (static_cast<uint64_t>(identity.nFileIndexHigh) << 32) |
                                   identity.nFileIndexLow});
}
#else
Result<std::pair<std::string, DescriptorFileIdentity>> readSecureDescriptorFd(int descriptor_fd) {
    struct stat info{};
    if (fstat(descriptor_fd, &info) != 0 || !S_ISREG(info.st_mode) ||
        info.st_uid != geteuid() || (info.st_mode & (S_IRWXG | S_IRWXO)) != 0 ||
        info.st_size < 0 || static_cast<uint64_t>(info.st_size) > kMaxDescriptorBytes) {
        return Error::invalidArgument("Descriptor must be an owner-only regular file");
    }
    std::string contents(static_cast<size_t>(info.st_size), '\0');
    size_t offset = 0;
    while (offset < contents.size()) {
        const auto count = pread(descriptor_fd, contents.data() + offset,
                                 contents.size() - offset, static_cast<off_t>(offset));
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return Error::internal("Descriptor read failed");
        offset += static_cast<size_t>(count);
    }
    return std::make_pair(std::move(contents),
                          DescriptorFileIdentity{static_cast<uint64_t>(info.st_dev),
                                                 static_cast<uint64_t>(info.st_ino)});
}
#endif

Result<OwnedDescriptorSnapshot> inspectOwnedDescriptor(
    const std::filesystem::path& path, const SessionDescriptor& expected) {
#if defined(_WIN32)
    ScopedNativeHandle handle(CreateFileW(
        path.wstring().c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (handle.get() == INVALID_HANDLE_VALUE) return Error::notFound("Descriptor is not readable");
    auto snapshot = readSecureDescriptorHandle(handle.get());
#else
    ScopedNativeHandle directory_fd(open(path.parent_path().c_str(),
                                         O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (directory_fd.get() < 0) return Error::notFound("Descriptor directory is not readable");
    ScopedNativeHandle handle(openat(directory_fd.get(), path.filename().c_str(),
                                     O_RDONLY | O_NOFOLLOW | O_CLOEXEC));
    if (handle.get() < 0) return Error::notFound("Descriptor is not readable");
    auto snapshot = readSecureDescriptorFd(handle.get());
#endif
    if (snapshot.isErr()) return snapshot.error();
    if (!descriptorMatchesOwner(snapshot.value().first, expected)) {
        return Error(409, "Descriptor ownership changed");
    }
    return OwnedDescriptorSnapshot{snapshot.value().second};
}

enum class NoReplaceMoveResult {
    moved,
    destination_exists,
    failed,
};

NoReplaceMoveResult moveNoReplace(const std::filesystem::path& source,
                                  const std::filesystem::path& destination) {
#if defined(_WIN32)
    if (MoveFileExW(source.wstring().c_str(), destination.wstring().c_str(), MOVEFILE_WRITE_THROUGH)) {
        return NoReplaceMoveResult::moved;
    }
    const auto error = GetLastError();
    return (error == ERROR_ALREADY_EXISTS || error == ERROR_FILE_EXISTS)
               ? NoReplaceMoveResult::destination_exists
               : NoReplaceMoveResult::failed;
#elif defined(__linux__)
    if (syscall(SYS_renameat2, AT_FDCWD, source.c_str(), AT_FDCWD, destination.c_str(),
                RENAME_NOREPLACE) == 0) {
        return NoReplaceMoveResult::moved;
    }
    return errno == EEXIST ? NoReplaceMoveResult::destination_exists
                           : NoReplaceMoveResult::failed;
#elif defined(__APPLE__)
    if (renamex_np(source.c_str(), destination.c_str(), RENAME_EXCL) == 0) {
        return NoReplaceMoveResult::moved;
    }
    return errno == EEXIST ? NoReplaceMoveResult::destination_exists
                           : NoReplaceMoveResult::failed;
#else
    (void)source;
    (void)destination;
    return NoReplaceMoveResult::failed;
#endif
}

Result<std::string> retirementNonce() {
    std::array<uint8_t, 16> bytes{};
#if defined(_WIN32)
    if (BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()),
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
        return Error::internal("Unable to generate descriptor retirement nonce");
    }
#elif defined(__linux__)
    size_t offset = 0;
    while (offset < bytes.size()) {
        const auto count = getrandom(bytes.data() + offset, bytes.size() - offset, 0);
        if (count > 0) offset += static_cast<size_t>(count);
        else if (count < 0 && errno == EINTR) continue;
        else break;
    }
    if (offset != bytes.size()) return Error::internal("Unable to generate descriptor retirement nonce");
#else
    ScopedNativeHandle random_fd(open("/dev/urandom", O_RDONLY | O_CLOEXEC));
    if (random_fd.get() < 0) return Error::internal("Unable to generate descriptor retirement nonce");
    size_t offset = 0;
    while (offset < bytes.size()) {
        const auto count = read(random_fd.get(), bytes.data() + offset, bytes.size() - offset);
        if (count > 0) offset += static_cast<size_t>(count);
        else if (count < 0 && errno == EINTR) continue;
        else return Error::internal("Unable to generate descriptor retirement nonce");
    }
#endif
    static constexpr char digits[] = "0123456789abcdef";
    std::string encoded;
    encoded.reserve(bytes.size() * 2);
    for (const auto byte : bytes) {
        encoded.push_back(digits[(byte >> 4) & 0x0f]);
        encoded.push_back(digits[byte & 0x0f]);
    }
    return encoded;
}

enum class OwnedDescriptorDeleteResult {
    deleted,
    identity_race,
    unavailable,
};

OwnedDescriptorDeleteResult deleteOwnedDescriptorIfSame(
    const std::filesystem::path& path,
    const SessionDescriptor& descriptor,
    const DescriptorFileIdentity& expected_identity,
    const std::function<void(const std::filesystem::path&)>& before_final_delete) {
#if defined(_WIN32)
    ScopedNativeHandle handle(CreateFileW(
        path.wstring().c_str(), GENERIC_READ | DELETE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (handle.get() == INVALID_HANDLE_VALUE) return OwnedDescriptorDeleteResult::unavailable;
    const auto snapshot = readSecureDescriptorHandle(handle.get());
    if (snapshot.isErr() || !(snapshot.value().second == expected_identity) ||
        !descriptorMatchesOwner(snapshot.value().first, descriptor)) {
        return OwnedDescriptorDeleteResult::identity_race;
    }
    if (before_final_delete) before_final_delete(path);
    FILE_DISPOSITION_INFO disposition{};
    disposition.DeleteFile = TRUE;
    return SetFileInformationByHandle(handle.get(), FileDispositionInfo, &disposition,
                                      sizeof(disposition)) != 0
               ? OwnedDescriptorDeleteResult::deleted
               : OwnedDescriptorDeleteResult::unavailable;
#else
    ScopedNativeHandle directory_fd(open(path.parent_path().c_str(),
                                         O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (directory_fd.get() < 0) return OwnedDescriptorDeleteResult::unavailable;
    ScopedNativeHandle handle(openat(directory_fd.get(), path.filename().c_str(),
                                     O_RDONLY | O_NOFOLLOW | O_CLOEXEC));
    if (handle.get() < 0) return OwnedDescriptorDeleteResult::unavailable;
    const auto snapshot = readSecureDescriptorFd(handle.get());
    if (snapshot.isErr() || !(snapshot.value().second == expected_identity) ||
        !descriptorMatchesOwner(snapshot.value().first, descriptor)) {
        return OwnedDescriptorDeleteResult::identity_race;
    }
    struct stat path_info{};
    if (fstatat(directory_fd.get(), path.filename().c_str(), &path_info,
                AT_SYMLINK_NOFOLLOW) != 0 ||
        DescriptorFileIdentity{static_cast<uint64_t>(path_info.st_dev),
                               static_cast<uint64_t>(path_info.st_ino)} != expected_identity) {
        return OwnedDescriptorDeleteResult::identity_race;
    }
    if (before_final_delete) before_final_delete(path);
    // POSIX has no portable unlink primitive bound to this verified open file.
    // Re-checking a pathname and then calling unlinkat would still leave a final
    // substitution window, so retain the non-discoverable tombstone fail-safe.
    struct stat final_path_info{};
    if (fstatat(directory_fd.get(), path.filename().c_str(), &final_path_info,
                AT_SYMLINK_NOFOLLOW) != 0 ||
        DescriptorFileIdentity{static_cast<uint64_t>(final_path_info.st_dev),
                               static_cast<uint64_t>(final_path_info.st_ino)} != expected_identity) {
        return OwnedDescriptorDeleteResult::identity_race;
    }
    return OwnedDescriptorDeleteResult::unavailable;
#endif
}

Result<std::string> readDescriptorFromValidatedHandle(
    const std::filesystem::path& directory,
    const std::filesystem::path& path,
    const DescriptorOpenedHook& opened_hook) {
#if defined(_WIN32)
    ScopedNativeHandle handle(CreateFileW(
        path.wstring().c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    if (handle.get() == INVALID_HANDLE_VALUE) return Error::notFound("Descriptor is not readable");

    FILE_ATTRIBUTE_TAG_INFO attributes{};
    if (!GetFileInformationByHandleEx(handle.get(), FileAttributeTagInfo, &attributes,
                                      sizeof(attributes))) {
        return Error::notFound("Descriptor is not readable");
    }
    if ((attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        return Error::invalidArgument("Descriptor must not be a symlink or reparse point");
    }
    if ((attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ||
        GetFileType(handle.get()) != FILE_TYPE_DISK) {
        return Error::invalidArgument("Descriptor must be a regular file");
    }

    std::wstring final_path(32768, L'\0');
    const DWORD final_length = GetFinalPathNameByHandleW(
        handle.get(), final_path.data(), static_cast<DWORD>(final_path.size()), FILE_NAME_NORMALIZED);
    if (final_length == 0 || final_length >= final_path.size()) {
        return Error::invalidArgument("Descriptor final path is unverifiable");
    }
    final_path.resize(final_length);
    constexpr wchar_t kExtendedPrefix[] = L"\\\\?\\";
    if (final_path.rfind(kExtendedPrefix, 0) == 0) final_path.erase(0, 4);
    std::error_code equivalent_error;
    const bool same_parent = std::filesystem::equivalent(
        std::filesystem::path(final_path).parent_path(), directory, equivalent_error);
    if (equivalent_error || !same_parent) {
        return Error::invalidArgument("Descriptor escaped descriptor directory");
    }

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(handle.get(), &size) || size.QuadPart < 0 ||
        static_cast<uint64_t>(size.QuadPart) > kMaxDescriptorBytes) {
        return Error::invalidArgument("Descriptor exceeds 64 KiB limit");
    }
    if (opened_hook) opened_hook(path);

    std::string contents(static_cast<size_t>(size.QuadPart), '\0');
    DWORD bytes_read = 0;
    const bool read_ok = contents.empty() ||
                         ReadFile(handle.get(), contents.data(), static_cast<DWORD>(contents.size()),
                                  &bytes_read, nullptr);
    if (!read_ok || bytes_read != contents.size()) {
        return Error::internal("Descriptor read failed");
    }
    return contents;
#else
    ScopedNativeHandle directory_fd(open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (directory_fd.get() < 0) return Error::notFound("Session descriptor directory is not readable");
    struct stat directory_info{};
    if (fstat(directory_fd.get(), &directory_info) != 0 || !S_ISDIR(directory_info.st_mode) ||
        directory_info.st_uid != geteuid() ||
        (directory_info.st_mode & (S_IRWXG | S_IRWXO)) != 0) {
        return Error::invalidArgument("Session descriptor directory must be owner-only");
    }
    ScopedNativeHandle descriptor_fd(openat(directory_fd.get(), path.filename().c_str(),
                                            O_RDONLY | O_NOFOLLOW | O_CLOEXEC));
    const int open_error = errno;
    if (descriptor_fd.get() < 0) {
        if (open_error == ELOOP) {
            return Error::invalidArgument("Descriptor must not be a symlink or reparse point");
        }
        return Error::notFound("Descriptor is not readable");
    }

    struct stat info{};
    if (fstat(descriptor_fd.get(), &info) != 0) {
        return Error::notFound("Descriptor is not readable");
    }
    if (!S_ISREG(info.st_mode)) {
        return Error::invalidArgument("Descriptor must be a regular file");
    }
    if (info.st_uid != geteuid() || (info.st_mode & (S_IRWXG | S_IRWXO)) != 0) {
        return Error::invalidArgument("Descriptor must be owner-only");
    }
    if (info.st_size < 0 || static_cast<uint64_t>(info.st_size) > kMaxDescriptorBytes) {
        return Error::invalidArgument("Descriptor exceeds 64 KiB limit");
    }
    if (opened_hook) opened_hook(path);

    std::string contents(static_cast<size_t>(info.st_size), '\0');
    size_t offset = 0;
    while (offset < contents.size()) {
        const auto count = read(descriptor_fd.get(), contents.data() + offset, contents.size() - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) {
            return Error::internal("Descriptor read failed");
        }
        offset += static_cast<size_t>(count);
    }
    return contents;
#endif
}


} // namespace

ProcessInstanceReport describeProcessInstance(uint64_t pid, int64_t started_at_ms) {
    const auto identity = queryProcessIdentity(pid);
    if (identity.isOk()) {
        return {std::llabs(identity.value().started_at_ms - started_at_ms) <=
                        identity.value().resolution_ms
                    ? ProcessInstanceState::alive
                    : ProcessInstanceState::proven_stale,
                {}, 0};
    }
#if defined(_WIN32)
    if (pid > std::numeric_limits<DWORD>::max()) {
        return {ProcessInstanceState::proven_stale, {}, 0};
    }
    ScopedNativeHandle process(OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid)));
    if (process.get() == INVALID_HANDLE_VALUE || process.get() == nullptr) {
        const unsigned long error = GetLastError();
        // Windows answers a pid nothing owns with ERROR_INVALID_PARAMETER, so
        // that one is a fact about the process. Any other refusal is a fact
        // about this process's rights, and saying so is the difference between
        // "it died" and "we were not allowed to look".
        if (error == ERROR_INVALID_PARAMETER) {
            return {ProcessInstanceState::proven_stale, {}, 0};
        }
        return {ProcessInstanceState::unverifiable, "open_denied", error};
    }
    if (WaitForSingleObject(process.get(), 0) == WAIT_OBJECT_0) {
        return {ProcessInstanceState::proven_stale, {}, 0};
    }
    // Something with that pid is running. It could not be confirmed as the
    // process the session opened, so this is not "alive", but it is a long way
    // from a corpse and a reader deserves to know which.
    return {ProcessInstanceState::unverifiable, "running_but_unidentified", 0};
#else
    if (pid > static_cast<uint64_t>(std::numeric_limits<pid_t>::max())) {
        return {ProcessInstanceState::proven_stale, {}, 0};
    }
    errno = 0;
    if (kill(static_cast<pid_t>(pid), 0) != 0) {
        if (errno == ESRCH) return {ProcessInstanceState::proven_stale, {}, 0};
        return {ProcessInstanceState::unverifiable, "open_denied",
                static_cast<unsigned long>(errno)};
    }
    return {ProcessInstanceState::unverifiable, "running_but_unidentified", 0};
#endif
}

ProcessInstanceState processInstanceState(uint64_t pid, int64_t started_at_ms) {
    return describeProcessInstance(pid, started_at_ms).state;
}

namespace {

std::string valueAfter(const std::string& text, const std::string& key) {
    const auto at = text.find(key);
    if (at == std::string::npos) return {};
    const auto start = at + key.size();
    const auto end = text.find_first_of("\r\n", start);
    auto value = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.pop_back();
    return value;
}

} // namespace

EngineCrashReport findEngineCrashReport(const std::string& project_path, uint64_t pid) {
    EngineCrashReport report;
    if (pid == 0) return report;
    const std::string name = "godot_crash_" + std::to_string(pid) + ".log";

    std::vector<std::filesystem::path> candidates;
    if (const char* configured = std::getenv("DIDI_CRASH_CAPTURE_DIR");
        configured && *configured) {
        candidates.push_back(paths::projectPathFromUtf8(configured) / name);
    }
    if (!project_path.empty()) {
        candidates.push_back(paths::projectPathFromUtf8(project_path) / ".didi" / "crash" / name);
    }

    std::error_code error;
    for (const auto& candidate : candidates) {
        if (!std::filesystem::is_regular_file(candidate, error)) continue;
        std::ifstream file(candidate, std::ios::binary);
        if (!file) continue;
        std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        if (text.find("DIDI CRASH CAPTURE") == std::string::npos) continue;

        report.found = true;
        report.path = paths::nativePathToUtf8(candidate);
        report.exception = valueAfter(text, "exception: ");
        report.on_main_thread = valueAfter(text, "on main thread: ") == "yes";
        // Only the stack section counts. The module list below it names every
        // loaded binary, ours included, so searching the whole report would
        // call every crash ours.
        const auto stack = text.find("\nstack:");
        const auto modules = text.find("\nloaded modules:");
        if (stack != std::string::npos && modules != std::string::npos && modules > stack) {
            report.in_extension =
                text.substr(stack, modules - stack).find("didi_extension") != std::string::npos;
        }
        return report;
    }
    return report;
}

const char* engineIncidentKindName(EngineIncidentKind kind) {
    switch (kind) {
        case EngineIncidentKind::crashed: return "engine_crashed";
        case EngineIncidentKind::unreachable: return "engine_unreachable";
        case EngineIncidentKind::hung: return "engine_hung";
        case EngineIncidentKind::session_lost: return "session_lost";
        case EngineIncidentKind::none: break;
    }
    return "none";
}

namespace {

// Every recovery sentence ends by naming the one call that says what still
// works, because the useful next move is almost never to try the same tool
// again.
const char* kWhatStillWorks =
    " Call runtime_get_session for the tools that still work without an engine.";

} // namespace

EngineIncident classifyEngineIncident(ProcessInstanceState state, const EngineCrashReport& crash,
                                      bool transport_failed) {
    EngineIncident incident;
    switch (state) {
        case ProcessInstanceState::proven_stale:
            incident.kind = EngineIncidentKind::crashed;
            if (crash.found && crash.in_extension) {
                // Worth separating loudly. A frame of ours on the faulting
                // stack means the engine died doing something we asked, and
                // that is ours to fix rather than to report upstream.
                incident.cause = "The engine died with a Didi frame on the faulting stack: " +
                                 crash.exception;
                incident.recovery = "This is a fault in the extension, not the engine. Keep " +
                                    crash.path + " and report it." + kWhatStillWorks;
            } else if (crash.found) {
                incident.cause = "The engine died in its own code: " + crash.exception;
                incident.recovery = "Ask the user to restart the Godot editor, then call "
                                    "runtime_attach_session. The report is at " + crash.path + "." +
                                    kWhatStillWorks;
            } else {
                incident.cause = "The engine process is gone and left no crash report.";
                incident.recovery = "Ask the user to restart the Godot editor, then call "
                                    "runtime_attach_session." + std::string(kWhatStillWorks);
            }
            return incident;
        case ProcessInstanceState::unverifiable:
            incident.kind = EngineIncidentKind::unreachable;
            incident.cause = "The engine process cannot be verified.";
            incident.recovery = "Call runtime_list_sessions to see what is live. If nothing is, "
                                "ask the user to restart the Godot editor." +
                                std::string(kWhatStillWorks);
            return incident;
        case ProcessInstanceState::alive:
            if (!transport_failed) return incident;
            incident.kind = EngineIncidentKind::hung;
            incident.cause = "The engine is running but did not answer within the deadline.";
            // Deliberately not "retry". A mutation whose outcome is unknown must
            // not be repeated, which is the rule the dispatch layer already
            // enforces, and this is the one place an agent is most tempted.
            incident.recovery = "Do not repeat a mutation whose outcome is unknown. Call "
                                "runtime_get_session to check the engine, and retry reads only." +
                                std::string(kWhatStillWorks);
            return incident;
    }
    return incident;
}

namespace {

// One line per dead engine, not one per failed call. A tool loop can produce
// dozens of failures from a single crash, and a log that repeats itself is a
// log nobody reads.
std::mutex g_reported_crash_mutex;
std::set<uint64_t> g_reported_crash_pids;

void reportCrashOnce(uint64_t pid, const EngineIncident& incident, const EngineCrashReport& crash) {
    if (incident.kind != EngineIncidentKind::crashed) return;
    {
        std::lock_guard<std::mutex> lock(g_reported_crash_mutex);
        if (!g_reported_crash_pids.insert(pid).second) return;
    }
    // The only thing that reaches a person without the agent choosing to pass
    // it on. Everything else here is addressed to the agent.
    if (crash.found) {
        DIDI_LOG_ERROR("RUNTIME", "The Godot engine (pid ", pid, ") died: ", incident.cause,
                       " Report: ", crash.path);
    } else {
        DIDI_LOG_ERROR("RUNTIME", "The Godot engine (pid ", pid, ") died: ", incident.cause);
    }
}

} // namespace

void annotateEngineState(Error& error, const std::optional<SessionDescriptor>& session) {
    if (!session.has_value() || session->pid == 0) return;
    if (!error.data.is_object()) error.data = json::object();
    const auto report = describeProcessInstance(session->pid, session->started_at_ms);
    error.data["engine"] = processInstanceStateName(report.state);
    // Only when the answer is "unknown", because that is the only answer that
    // needs one. Alive and gone say everything they mean.
    if (!report.reason.empty()) {
        error.data["engine_reason"] = report.reason;
        if (report.os_error != 0) {
            error.data["engine_os_error"] = static_cast<uint64_t>(report.os_error);
        }
    }
    // An engine that is not alive may have left an account of why. Without it a
    // caller sees only that the connection went, which is the same thing it
    // would see for a network fault or a clean shutdown.
    const bool transport_failed = error.data.contains("outcome");
    const auto crash = report.state == ProcessInstanceState::alive
                           ? EngineCrashReport{}
                           : findEngineCrashReport(session->project_path, session->pid);
    if (crash.found) {
        error.data["engine_crash"] = {{"report", crash.path},
                                      {"exception", crash.exception},
                                      {"on_main_thread", crash.on_main_thread},
                                      {"in_extension", crash.in_extension}};
    }

    // The facts above say what happened. Without this the caller still has to
    // work out what to do about it, and the thing it does by default is call
    // the same tool again.
    const auto incident = classifyEngineIncident(report.state, crash, transport_failed);
    if (incident.kind == EngineIncidentKind::none) return;
    error.data["incident"] = engineIncidentKindName(incident.kind);
    error.data["cause"] = incident.cause;
    error.data["recovery"] = incident.recovery;
    reportCrashOnce(session->pid, incident, crash);
}

const char* processInstanceStateName(ProcessInstanceState state) {
    switch (state) {
        case ProcessInstanceState::alive: return "alive";
        case ProcessInstanceState::proven_stale: return "gone";
        case ProcessInstanceState::unverifiable: break;
    }
    return "unknown";
}

namespace {

struct DiscoveredSession {
    SessionDescriptor descriptor;
    bool alive{false};
};

Result<json> authenticateSession(const std::shared_ptr<ipc::IIpcClient>& client,
                                 const SessionDescriptor& descriptor) {
    if (!client) return Error::internal("Runtime IPC client factory returned no client");
    const json handshake_params = {{"_didi_session_token", descriptor.token},
                                   {"protocol_version", "1.3"}};
    auto handshake = client->sendRequest("session.handshake", handshake_params,
                                         kSessionHandshakeTimeoutMs);
    if (handshake.isErr()) return handshake.error();
    auto expected = descriptor.toJson();
    expected["status"] = "ok";
    if (!handshake.value().is_object() || handshake.value() != expected ||
        handshake.value().contains("token")) {
        return Error(409, "Runtime session handshake identity did not match the selected descriptor");
    }
    return handshake.value();
}

// Tombstone names are produced by retireOwnedSessionDescriptor as
// "<session-id>.json.didi-retired-<session-id>-<nonce>", where both the session
// id and the nonce are 32 lowercase hex characters. Returns the session id the
// name claims, or nullopt when the entry is not one of our tombstones.
std::optional<std::string> tombstoneSessionIdFromName(const std::string& name) {
    static const std::string marker = ".json.didi-retired-";
    const auto marker_offset = name.find(marker);
    if (marker_offset != 32) return std::nullopt;

    const std::string named_id = name.substr(0, 32);
    if (!isLowerHex(named_id, 32)) return std::nullopt;

    const std::string suffix = name.substr(marker_offset + marker.size());
    if (suffix.size() != 32 + 1 + 32) return std::nullopt;
    if (suffix.compare(0, 32, named_id) != 0 || suffix[32] != '-') return std::nullopt;
    if (!isLowerHex(suffix.substr(33), 32)) return std::nullopt;
    return named_id;
}

}  // namespace

TombstoneReapOutcome reapOrphanedDescriptorTombstone(
    const std::filesystem::path& directory, const std::filesystem::path& path) {
    const auto named_id = tombstoneSessionIdFromName(path.filename().string());
    if (!named_id.has_value()) return TombstoneReapOutcome::not_a_tombstone;

    // Everything below is decided from a single opened handle, so the object
    // that is verified is the object that is removed.
#if defined(_WIN32)
    ScopedNativeHandle handle(CreateFileW(
        path.wstring().c_str(), GENERIC_READ | DELETE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (handle.get() == INVALID_HANDLE_VALUE) return TombstoneReapOutcome::retained_unverifiable;
    const auto snapshot = readSecureDescriptorHandle(handle.get());
#else
    ScopedNativeHandle directory_fd(open(directory.c_str(),
                                         O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (directory_fd.get() < 0) return TombstoneReapOutcome::retained_unverifiable;
    ScopedNativeHandle handle(openat(directory_fd.get(), path.filename().c_str(),
                                     O_RDONLY | O_NOFOLLOW | O_CLOEXEC));
    if (handle.get() < 0) return TombstoneReapOutcome::retained_unverifiable;
    const auto snapshot = readSecureDescriptorFd(handle.get());
#endif
    if (snapshot.isErr()) return TombstoneReapOutcome::retained_unverifiable;

    SessionDescriptor descriptor;
    try {
        auto decoded = SessionDescriptor::fromJson(json::parse(snapshot.value().first));
        if (decoded.isErr()) return TombstoneReapOutcome::retained_unverifiable;
        descriptor = decoded.value();
    } catch (const std::exception&) {
        return TombstoneReapOutcome::retained_unverifiable;
    }

    // The name and the contents must agree. Without this, a descriptor placed
    // under an unrelated tombstone name could authorize removing that name.
    if (descriptor.session_id != named_id.value()) {
        return TombstoneReapOutcome::retained_unverifiable;
    }

    // Never delete on a guess. Only a provably finished owner qualifies; alive
    // and unverifiable both retain.
    if (processInstanceState(descriptor.pid, descriptor.started_at_ms) !=
        ProcessInstanceState::proven_stale) {
        return TombstoneReapOutcome::retained_owner_not_proven_gone;
    }

#if defined(_WIN32)
    FILE_DISPOSITION_INFO disposition{};
    disposition.DeleteFile = TRUE;
    return SetFileInformationByHandle(handle.get(), FileDispositionInfo, &disposition,
                                      sizeof(disposition)) != 0
               ? TombstoneReapOutcome::reaped
               : TombstoneReapOutcome::retained_unavailable;
#else
    // POSIX has no portable unlink primitive bound to this verified open file.
    // unlinkat() acts on the name, so removing it would reintroduce exactly the
    // substitution window that retirement refuses to accept. Retain instead.
    return TombstoneReapOutcome::retained_unavailable;
#endif
}

namespace {

std::vector<DiscoveredSession> discoverSessions(json& diagnostics,
                                                const DescriptorOpenedHook& opened_hook) {
    std::vector<DiscoveredSession> sessions;
    const auto resolved_directory = resolveSessionDescriptorDirectory();
    if (resolved_directory.isErr()) {
        diagnostics.push_back({{"path", ""}, {"error", resolved_directory.error().message}});
        return sessions;
    }
    const auto directory = resolved_directory.value();
    std::error_code ec;
    if (!std::filesystem::exists(directory, ec)) return sessions;
    const auto valid_directory = validateDescriptorDirectory(directory);
    if (ec || valid_directory.isErr()) {
        diagnostics.push_back({{"path", directory.string()},
                               {"error", valid_directory.isErr()
                                             ? valid_directory.error().message
                                             : "Session descriptor directory is not readable"}});
        return sessions;
    }

    for (std::filesystem::directory_iterator it(directory, ec), end; !ec && it != end; it.increment(ec)) {
        const auto& entry = *it;
        const auto path = entry.path();
        if (path.extension() != ".json") {
            // Opportunistic housekeeping. Retirement is move-then-delete, and an
            // owner that dies between the two steps leaves a tombstone nobody
            // will finish removing. Deliberately silent: a retained tombstone is
            // not discoverable and is not a fault the caller needs reported.
            (void)reapOrphanedDescriptorTombstone(directory, path);
            continue;
        }
        auto contents = readDescriptorFromValidatedHandle(directory, path, opened_hook);
        if (contents.isErr()) {
            diagnostics.push_back({{"path", path.string()}, {"error", contents.error().message}});
            continue;
        }
        try {
            auto decoded = SessionDescriptor::fromJson(json::parse(contents.value()));
            if (decoded.isErr()) {
                diagnostics.push_back({{"path", path.string()}, {"error", decoded.error().message}});
                continue;
            }
            auto descriptor = decoded.value();
            descriptor.project_path =
                paths::nativePathToUtf8(canonicalPath(paths::projectPathFromUtf8(descriptor.project_path)));
            const auto state = processInstanceState(descriptor.pid, descriptor.started_at_ms);
            if (state == ProcessInstanceState::proven_stale) {
                const auto retired = retireOwnedSessionDescriptor(path, descriptor);
                if (retired != DescriptorRetirementOutcome::deleted) {
                    diagnostics.push_back({{"path", path.string()},
                                           {"error", retired == DescriptorRetirementOutcome::retained_unavailable
                                                         ? "Proven-stale descriptor tombstone was retained because identity-bound deletion is unavailable"
                                                         : "Proven-stale descriptor was retained after a cleanup collision or race"}});
                }
                continue;
            }
            sessions.push_back({std::move(descriptor), state == ProcessInstanceState::alive});
        } catch (const std::exception& error) {
            diagnostics.push_back({{"path", path.string()}, {"error", std::string("Malformed descriptor: ") + error.what()}});
        }
    }
    if (ec) diagnostics.push_back({{"path", directory.string()}, {"error", "Failed while scanning descriptor directory"}});
    return sessions;
}

// How many sessions one process will hold routes to at once.
//
// Each route keeps a connection and an ownership lock, and a lock held here is
// a session refused to every other Didi process. Two or three is the realistic
// number; this is far above that and exists so the count cannot grow without
// one, not as a working limit anyone should meet.
constexpr size_t kMaxHeldRoutes = 8;

class RuntimeSessionClient final : public IRuntimeSessionClient {
public:
    RuntimeSessionClient(std::string project_root, ipc::IpcClientFactory factory,
                         DescriptorOpenedHook opened_hook)
        : m_projectRoot(paths::nativePathToUtf8(canonicalPath(paths::projectPathFromUtf8(project_root)))),
          m_factory(std::move(factory)),
          m_descriptorOpenedHook(std::move(opened_hook)) {
        auto client_id = security::secureRandomHex(16);
        if (client_id.isOk()) m_clientId = std::move(client_id.value());
    }

    bool connect(const std::string&, int) override { return isConnected(); }

    void disconnect() override {
        // Every route, not only the selected one. Each one holds an ownership
        // lock, and a lock this process keeps after it has stopped using the
        // session refuses that session to every other Didi process until this
        // one exits.
        std::vector<Route> released;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            released.reserve(m_routes.size());
            for (auto& entry : m_routes) released.push_back(std::move(entry.second));
            m_routes.clear();
            m_selectedSessionId.clear();
            m_autoAttachEnabled = false;
            ++m_releaseEpoch;
        }
        for (auto& route : released) {
            if (route.client) route.client->disconnect();
        }
    }

    bool isConnected() const override {
        std::shared_ptr<ipc::IIpcClient> selected;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (const Route* route = findRouteLocked(m_selectedSessionId)) selected = route->client;
        }
        if (selected) return selected->isConnected();
        return const_cast<RuntimeSessionClient*>(this)->tryAutoAttach();
    }

    Result<json> sendRequest(const std::string& method, const json& params, int timeout_ms) override {
        const auto lease = acquireRouteLease();
        if (!lease.has_value()) {
            return Error::notConnected("No runtime session is attached");
        }
        return lease->sendRequest(method, params, timeout_ms);
    }

    Result<json> listSessions(const std::optional<std::string>& project_path) override {
        json diagnostics = json::array();
        const auto filter = project_path.has_value()
                                ? paths::nativePathToUtf8(canonicalPath(paths::projectPathFromUtf8(*project_path)))
                                : std::string{};
        json listed = json::array();
        for (const auto& session : discoverSessions(diagnostics, m_descriptorOpenedHook)) {
            if (!filter.empty() && session.descriptor.project_path != filter) continue;
            auto item = session.descriptor.toJson();
            item["alive"] = session.alive;
            item["stale"] = !session.alive;
            listed.push_back(std::move(item));
        }
        return json{{"sessions", listed}, {"diagnostics", diagnostics}};
    }

    // Selects a session for the process, which is what the legacy lifecycle
    // means by attaching. Opening the route is the same work either way; only
    // whether the selection moves differs.
    Result<json> attachSession(const std::string& session_id) override {
        return openRoute(session_id, true);
    }

    // Holds a route without moving the selection, so a request that named its
    // own session cannot take the process out from under a legacy client.
    Result<json> openSessionRoute(const std::string& session_id) override {
        return openRoute(session_id, false);
    }

    Result<json> detachSession() override {
        std::optional<Route> released;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_autoAttachEnabled = false;
            if (m_selectedSessionId.empty() || !findRouteLocked(m_selectedSessionId)) {
                return Error::notConnected("No runtime session is attached");
            }
            released = takeRouteLocked(m_selectedSessionId);
        }
        if (released->client) released->client->disconnect();
        return json{{"session", released->descriptor.toJson()}};
    }

    Result<json> closeSessionRoute(const std::string& session_id) override {
        std::optional<Route> released;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            released = takeRouteLocked(session_id);
        }
        if (!released.has_value()) {
            return Error::notFound("No runtime route is open for session: " + session_id);
        }
        if (released->client) released->client->disconnect();
        return json{{"session", released->descriptor.toJson()}};
    }

    std::vector<SessionDescriptor> heldSessions() const override {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::vector<SessionDescriptor> held;
        held.reserve(m_routes.size());
        for (const auto& entry : m_routes) held.push_back(entry.second.descriptor);
        return held;
    }

    Result<json> refreshSession() override {
        (void)isConnected();
        std::shared_ptr<ipc::IIpcClient> active;
        std::optional<SessionDescriptor> descriptor;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (const Route* route = findRouteLocked(m_selectedSessionId)) {
                active = route->client;
                descriptor = route->descriptor;
            }
        }
        if (!active || !descriptor.has_value() || !active->isConnected()) {
            if (active) quarantineIfCurrent(active);
            return Error::notConnected("No runtime session is attached");
        }
        auto handshake = authenticateSession(active, *descriptor);
        if (handshake.isErr()) {
            quarantineIfCurrent(active);
            return handshake.error();
        }
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            const Route* route = findRouteLocked(m_selectedSessionId);
            if (!route || route->client != active ||
                route->descriptor.session_id != descriptor->session_id) {
                return Error(409, "Fresh runtime session handshake was superseded by a route change");
            }
        }
        return json{{"session", descriptor->toJson()}, {"handshake", handshake.value()},
                    {"connected", true}};
    }

    std::optional<SessionDescriptor> activeSession() const override {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (const Route* route = findRouteLocked(m_selectedSessionId)) return route->descriptor;
        return std::nullopt;
    }

    std::optional<RuntimeRouteLease> acquireRouteLease() override {
        (void)isConnected();
        std::string selected;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            selected = m_selectedSessionId;
        }
        if (selected.empty()) return std::nullopt;
        return leaseFor(selected);
    }

    // Deliberately does not go through isConnected. That would auto-attach the
    // one session matching this project, which is a guess, and a request that
    // named a session is not guessing.
    std::optional<RuntimeRouteLease> acquireRouteLeaseFor(const std::string& session_id) override {
        if (session_id.empty()) return std::nullopt;
        return leaseFor(session_id);
    }

    bool quarantineRoute(const RuntimeRouteLease& lease) override {
        if (!lease.descriptor.has_value()) return false;
        std::optional<Route> quarantined;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            const Route* route = findRouteLocked(lease.descriptor->session_id);
            // Generations are handed out once and never reused, so this both
            // finds the route and proves the lease is for this instance of it
            // rather than one that was already retired and reopened.
            if (!route || route->generation != lease.generation || route->client != lease.client) {
                return false;
            }
            quarantined = takeRouteLocked(lease.descriptor->session_id);
            m_autoAttachEnabled = false;
        }
        if (quarantined->client) quarantined->client->disconnect();
        return true;
    }

private:
    // One held route: the connection, the ownership lock that keeps other Didi
    // processes off this session, and the descriptor naming it. The map owns
    // the only reference to each, so removing the entry is what releases both.
    struct Route {
        std::shared_ptr<ipc::IIpcClient> client;
        std::shared_ptr<RuntimeSessionLock> lock;
        SessionDescriptor descriptor;
        uint64_t generation{0};
    };

    Result<json> attachDescriptor(const SessionDescriptor& descriptor, bool make_selected) {
        if (!m_factory) return Error::internal("Runtime IPC client factory is not configured");
        if (m_clientId.empty()) return Error::internal("Unable to establish MCP client identity");
        std::shared_ptr<RuntimeSessionLock> session_lock;
        // What this attach is racing against. Connecting and handshaking happen
        // outside the mutex, so the world can move underneath: a caller can
        // disconnect everything, or detach this very session, while the
        // handshake is still in flight. Committing anyway would resurrect a
        // route somebody deliberately released, and take its ownership lock
        // back with it.
        uint64_t release_epoch = 0;
        uint64_t teardowns = 0;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            release_epoch = m_releaseEpoch;
            teardowns = teardownsLocked(descriptor.session_id);
            // A route to this session that we already own carries the lock we
            // would otherwise queue for behind ourselves.
            if (const Route* route = findRouteLocked(descriptor.session_id)) {
                session_lock = route->lock;
            }
        }
        if (!session_lock) {
            auto session_directory = resolveSessionDescriptorDirectory();
            if (session_directory.isErr()) return session_directory.error();
            auto acquired = RuntimeSessionLock::acquire(
                session_directory.value() / (descriptor.session_id + ".lock"),
                {{"client_id", m_clientId}, {"session_id", descriptor.session_id},
                 {"project_path", descriptor.project_path}});
            if (acquired.isErr()) return acquired.error();
            session_lock = std::move(acquired.value());
        }
        auto candidate = std::shared_ptr<ipc::IIpcClient>(m_factory());
        if (!candidate || !candidate->connect(descriptor.endpoint, 2000)) {
            return Error::notConnected("Unable to connect to runtime session: " + descriptor.session_id);
        }
        auto handshake = authenticateSession(candidate, descriptor);
        if (handshake.isErr()) {
            candidate->disconnect();
            return handshake.error();
        }

        std::shared_ptr<ipc::IIpcClient> previous;
        bool kept_existing = false;
        bool superseded = false;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_releaseEpoch != release_epoch ||
                teardownsLocked(descriptor.session_id) != teardowns) {
                superseded = true;
            } else {
                Route* existing = findRouteLocked(descriptor.session_id);
                if (existing && existing->client && existing->client->isConnected()) {
                    // Another caller opened this same route while we were
                    // connecting. Theirs is live and leased, so ours is the
                    // one that goes; churning the lock would gain nothing.
                    previous = std::move(candidate);
                    kept_existing = true;
                } else {
                    if (existing && existing->client) previous = std::move(existing->client);
                    Route route;
                    route.client = std::move(candidate);
                    // Held as a shared_ptr, so reusing the lock we already
                    // owned keeps it held rather than dropping and retaking it.
                    route.lock = std::move(session_lock);
                    route.descriptor = descriptor;
                    route.generation = m_nextGeneration++;
                    m_routes[descriptor.session_id] = std::move(route);
                }
                if (make_selected) m_selectedSessionId = descriptor.session_id;
            }
        }
        if (superseded) {
            candidate->disconnect();
            return Error(409, "Runtime attach was superseded by a later route change");
        }
        if (previous) previous->disconnect();
        return json{{"session", descriptor.toJson()},
                    {"handshake", handshake.value()},
                    {"reused", kept_existing}};
    }

    // Opens a route to one session, or reuses the one already held for it.
    Result<json> openRoute(const std::string& session_id, bool make_selected) {
        if (session_id.empty()) return Error::invalidArgument("session_id is required");
        if (make_selected) {
            std::lock_guard<std::mutex> lock(m_mutex);
            // Naming a session is a decision, so stop guessing at one.
            m_autoAttachEnabled = false;
        } else if (auto held = leaseFor(session_id)) {
            // A request naming a session this process is already routed to uses
            // that route as it stands. Attach does not take this path: it
            // re-authenticates every time, which is how a session whose process
            // changed underneath it is caught, and callers read the handshake
            // it returns.
            return json{{"session", held->descriptor->toJson()}, {"reused", true}};
        }
        // A route whose engine has gone still holds a lock. Drop those before
        // deciding there is no room, so an editor that closed hours ago cannot
        // be what refuses a session now.
        releaseDeadRoutes();
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_routes.find(session_id) == m_routes.end() &&
                m_routes.size() >= kMaxHeldRoutes) {
                return Error(429,
                             "This server is already holding the most runtime sessions it will "
                             "hold at once. Close one with runtime_detach_session before opening "
                             "another.");
            }
        }
        json diagnostics = json::array();
        const auto sessions = discoverSessions(diagnostics, m_descriptorOpenedHook);
        auto found = std::find_if(sessions.begin(), sessions.end(),
                                  [&](const DiscoveredSession& item) {
                                      return item.descriptor.session_id == session_id;
                                  });
        if (found == sessions.end()) return Error::notFound("Runtime session not found: " + session_id);
        if (!found->alive) return Error::notConnected("Runtime session is stale: " + session_id);
        return attachDescriptor(found->descriptor, make_selected);
    }

    void releaseDeadRoutes() {
        std::vector<Route> released;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            for (auto it = m_routes.begin(); it != m_routes.end();) {
                if (it->second.client && it->second.client->isConnected()) {
                    ++it;
                    continue;
                }
                if (m_selectedSessionId == it->first) m_selectedSessionId.clear();
                released.push_back(std::move(it->second));
                it = m_routes.erase(it);
            }
        }
        for (auto& route : released) {
            if (route.client) route.client->disconnect();
        }
    }

    std::optional<RuntimeRouteLease> leaseFor(const std::string& session_id) {
        RuntimeRouteLease lease;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            const Route* route = findRouteLocked(session_id);
            if (!route || !route->client) return std::nullopt;
            lease = RuntimeRouteLease{route->client, route->descriptor, route->generation};
        }
        if (!lease.client->isConnected()) return std::nullopt;
        return lease;
    }

    // Both require m_mutex.
    Route* findRouteLocked(const std::string& session_id) {
        auto it = m_routes.find(session_id);
        return it == m_routes.end() ? nullptr : &it->second;
    }
    const Route* findRouteLocked(const std::string& session_id) const {
        auto it = m_routes.find(session_id);
        return it == m_routes.end() ? nullptr : &it->second;
    }

    // Deliberate teardown of one route: detach, close, quarantine. Reaping a
    // route whose engine has gone is not one of these and does not count, so a
    // dead route being cleaned up cannot abort an attach that is in the middle
    // of replacing it.
    std::optional<Route> takeRouteLocked(const std::string& session_id) {
        auto it = m_routes.find(session_id);
        if (it == m_routes.end()) return std::nullopt;
        Route route = std::move(it->second);
        m_routes.erase(it);
        if (m_selectedSessionId == session_id) m_selectedSessionId.clear();
        ++m_teardowns[session_id];
        return route;
    }

    uint64_t teardownsLocked(const std::string& session_id) const {
        auto it = m_teardowns.find(session_id);
        return it == m_teardowns.end() ? 0 : it->second;
    }

    bool tryAutoAttach() {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (const Route* route = findRouteLocked(m_selectedSessionId)) {
                return route->client && route->client->isConnected();
            }
            if (!m_autoAttachEnabled || m_autoAttachInProgress) return false;
            m_autoAttachInProgress = true;
        }

        bool attached = false;
        try {
            json diagnostics = json::array();
            const auto discovered = discoverSessions(diagnostics, m_descriptorOpenedHook);
            std::vector<SessionDescriptor> matching;
            for (const auto& session : discovered) {
                if (session.alive && session.descriptor.project_path == m_projectRoot) {
                    matching.push_back(session.descriptor);
                }
            }

            std::optional<SessionDescriptor> selected;
            if (matching.size() == 1) {
                selected = matching.front();
            } else if (matching.size() > 1) {
                const auto editor_count = std::count_if(
                    matching.begin(), matching.end(),
                    [](const SessionDescriptor& session) { return session.kind == "editor"; });
                if (editor_count == 1) {
                    selected = *std::find_if(
                        matching.begin(), matching.end(),
                        [](const SessionDescriptor& session) { return session.kind == "editor"; });
                }
            }
            if (selected.has_value()) {
                attached = attachDescriptor(*selected, true).isOk();
                if (attached) {
                    DIDI_LOG_INFO("RUNTIME", "Re-attached to runtime session ",
                                  selected->session_id, " (pid ", selected->pid,
                                  ") without being asked; no session was attached and exactly one "
                                  "live one matched this project.");
                }
            }
        } catch (const std::exception&) {
            attached = false;
        }

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_autoAttachInProgress = false;
        }
        return attached;
    }

    // The session lock goes with the route, exactly as it does in
    // quarantineRoute, detachSession and disconnect. Retiring the route but
    // keeping the lock left this process holding a session it no longer has,
    // so the next attach got 423 from a lock we ourselves still owned, and so
    // did any other MCP client, until the process exited. Taking the route out
    // of the map is what releases both, because the map owns the only
    // reference to each.
    void quarantineIfCurrent(const std::shared_ptr<ipc::IIpcClient>& client) {
        std::optional<Route> quarantined;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            std::string owner;
            for (const auto& entry : m_routes) {
                if (entry.second.client == client) {
                    owner = entry.first;
                    break;
                }
            }
            if (owner.empty()) return;
            quarantined = takeRouteLocked(owner);
            m_autoAttachEnabled = false;
        }
        if (quarantined->client) quarantined->client->disconnect();
    }

    std::string m_projectRoot;
    std::string m_clientId;
    ipc::IpcClientFactory m_factory;
    DescriptorOpenedHook m_descriptorOpenedHook;
    mutable std::mutex m_mutex;
    // Every session this process is holding a route to, by session id.
    std::unordered_map<std::string, Route> m_routes;
    // The process selection, empty when there is none. Legacy attach sets it;
    // a request that names its own session does not.
    std::string m_selectedSessionId;
    bool m_autoAttachEnabled{true};
    bool m_autoAttachInProgress{false};
    // Bumped by disconnect, which releases everything. An attach that began
    // before it must not commit after it.
    uint64_t m_releaseEpoch{0};
    // Deliberate teardowns per session, for the same reason at one route's
    // granularity, so detaching one session leaves an attach to another alone.
    std::unordered_map<std::string, uint64_t> m_teardowns;
    // Handed out once per installed route and never reused, so a lease can be
    // matched to the exact instance of the route it was taken from.
    uint64_t m_nextGeneration{1};
};

} // namespace

DescriptorRetirementOutcome retireOwnedSessionDescriptor(
    const std::filesystem::path& path,
    const SessionDescriptor& descriptor,
    const std::function<void(const std::filesystem::path&)>& before_move,
    const std::function<void(const std::filesystem::path&)>& after_verification,
    const std::function<void(const std::filesystem::path&)>& before_final_delete) {
    const auto active_snapshot = inspectOwnedDescriptor(path, descriptor);
    if (active_snapshot.isErr()) {
        return DescriptorRetirementOutcome::retained_collision_or_race;
    }
    constexpr size_t kRetirementAttempts = 8;
    for (size_t attempt = 0; attempt < kRetirementAttempts; ++attempt) {
        const auto nonce = retirementNonce();
        if (nonce.isErr()) return DescriptorRetirementOutcome::retained_unavailable;
        const auto retired_path = std::filesystem::path(
            path.string() + ".didi-retired-" + descriptor.session_id + "-" + nonce.value());
        if (before_move) before_move(retired_path);
        const auto moved = moveNoReplace(path, retired_path);
        if (moved == NoReplaceMoveResult::destination_exists) continue;
        if (moved != NoReplaceMoveResult::moved) {
            return DescriptorRetirementOutcome::retained_unavailable;
        }

        const auto retired_snapshot = inspectOwnedDescriptor(retired_path, descriptor);
        if (retired_snapshot.isErr() ||
            !(retired_snapshot.value().identity == active_snapshot.value().identity)) {
            (void)moveNoReplace(retired_path, path);
            return DescriptorRetirementOutcome::retained_collision_or_race;
        }
        if (after_verification) after_verification(retired_path);
        const auto deleted = deleteOwnedDescriptorIfSame(
            retired_path, descriptor, active_snapshot.value().identity, before_final_delete);
        if (deleted == OwnedDescriptorDeleteResult::deleted) {
            return DescriptorRetirementOutcome::deleted;
        }
        return deleted == OwnedDescriptorDeleteResult::identity_race
                   ? DescriptorRetirementOutcome::retained_collision_or_race
                   : DescriptorRetirementOutcome::retained_unavailable;
    }
    return DescriptorRetirementOutcome::retained_collision_or_race;
}

json SessionDescriptor::toProvenanceJson() const {
    // Derived from toJson so a field added to the descriptor is carried here
    // too, and only the ones deliberately withheld have to be named.
    json value = toJson(false);
    value.erase("endpoint");
    return value;
}

json SessionDescriptor::toJson(bool include_token) const {
    json value = {
        {"schema_version", schema_version}, {"session_id", session_id}, {"pid", pid},
        {"kind", kind}, {"project_path", project_path}, {"endpoint", endpoint},
        {"started_at_ms", started_at_ms}, {"protocol_version", protocol_version}
    };
    if (!build_id.empty()) value["build_id"] = build_id;
    if (include_token) value["token"] = token;
    return value;
}

Result<SessionDescriptor> SessionDescriptor::fromJson(const json& value) {
    if (!value.is_object() || !hasOnlyDescriptorFields(value)) return Error::invalidArgument("Invalid session descriptor fields");
    try {
        if (!value.at("schema_version").is_number_integer() || value.at("schema_version").get<int>() != 1 ||
            !value.at("session_id").is_string() || !isLowerHex(value.at("session_id").get<std::string>(), 32) ||
            !value.at("token").is_string() || !isLowerHex(value.at("token").get<std::string>(), 64) ||
            !value.at("pid").is_number_unsigned() || value.at("pid").get<uint64_t>() == 0 ||
            !value.at("kind").is_string() || (value.at("kind") != "editor" && value.at("kind") != "game") ||
            !value.at("project_path").is_string() || value.at("project_path").get<std::string>().empty() ||
            !value.at("endpoint").is_string() ||
            !validEndpoint(value.at("endpoint").get<std::string>(), value.at("pid").get<uint64_t>(),
                           value.at("session_id").get<std::string>(),
                           value.at("project_path").get<std::string>()) ||
            !value.at("started_at_ms").is_number_integer() || value.at("started_at_ms").get<int64_t>() <= 0 ||
            !value.at("protocol_version").is_string() || value.at("protocol_version") != "1.3") {
            return Error::invalidArgument("Invalid session descriptor values");
        }
        if (value.contains("build_id") &&
            (!value.at("build_id").is_string() ||
             !validBuildId(value.at("build_id").get<std::string>()))) {
            return Error::invalidArgument("Invalid session descriptor values");
        }
        SessionDescriptor descriptor;
        descriptor.schema_version = value.at("schema_version").get<int>();
        descriptor.session_id = value.at("session_id").get<std::string>();
        descriptor.token = value.at("token").get<std::string>();
        descriptor.pid = value.at("pid").get<uint64_t>();
        descriptor.kind = value.at("kind").get<std::string>();
        descriptor.project_path = value.at("project_path").get<std::string>();
        descriptor.endpoint = value.at("endpoint").get<std::string>();
        descriptor.started_at_ms = value.at("started_at_ms").get<int64_t>();
        descriptor.protocol_version = value.at("protocol_version").get<std::string>();
        if (value.contains("build_id")) {
            descriptor.build_id = value.at("build_id").get<std::string>();
        }
        return descriptor;
    } catch (const std::exception&) {
        return Error::invalidArgument("Invalid session descriptor values");
    }
}

Result<json> RuntimeRouteLease::sendRequest(const std::string& method, const json& params,
                                            int timeout_ms) const {
    if (!client || !client->isConnected()) {
        return Error::notConnected("Runtime route lease is disconnected");
    }
    json routed_params = params.is_object() ? params : json::object();
    if (descriptor.has_value()) routed_params["_didi_session_token"] = descriptor->token;
    const int bounded_timeout_ms = timeout_ms < 0
                                       ? kMaxPublicLiveRequestMs
                                       : std::min(timeout_ms, kMaxPublicLiveRequestMs);
    return client->sendRequest(method, routed_params, bounded_timeout_ms);
}

std::optional<RuntimeRouteLease> acquireRuntimeRouteLease(
    const std::shared_ptr<ipc::IIpcClient>& router) {
    if (!router || !router->isConnected()) return std::nullopt;
    const auto sessions = std::dynamic_pointer_cast<IRuntimeSessionClient>(router);
    if (sessions) {
        auto lease = sessions->acquireRouteLease();
        if (!lease.has_value() || !lease->client || !lease->descriptor.has_value()) {
            return std::nullopt;
        }
        if (SessionDescriptor::fromJson(lease->descriptor->toJson(true)).isErr()) {
            return std::nullopt;
        }
        return lease;
    }
    const auto provider = std::dynamic_pointer_cast<IRuntimeRouteLeaseProvider>(router);
    if (provider) {
        auto lease = provider->acquireRouteLease();
        if (!lease.has_value() || !lease->client || !lease->descriptor.has_value()) {
            return std::nullopt;
        }
        if (SessionDescriptor::fromJson(lease->descriptor->toJson(true)).isErr()) {
            return std::nullopt;
        }
        return lease;
    }
    return RuntimeRouteLease{router, std::nullopt, 0};
}

std::optional<RuntimeRouteLease> acquireRuntimeRouteLeaseFor(
    const std::shared_ptr<ipc::IIpcClient>& router, const std::string& session_id) {
    if (!router || session_id.empty()) return std::nullopt;
    // No isConnected gate here, unlike the selection-based form. That asks
    // whether the selected route is up, and the whole point of naming a
    // session is to reach one that is not the selection.
    const auto provider = std::dynamic_pointer_cast<IRuntimeRouteLeaseProvider>(router);
    if (!provider) return std::nullopt;
    auto lease = provider->acquireRouteLeaseFor(session_id);
    if (!lease.has_value() || !lease->client || !lease->descriptor.has_value()) {
        return std::nullopt;
    }
    if (lease->descriptor->session_id != session_id) return std::nullopt;
    if (SessionDescriptor::fromJson(lease->descriptor->toJson(true)).isErr()) {
        return std::nullopt;
    }
    return lease;
}

bool reconnectRuntimeRoute(const RuntimeRouteLease& lease, int timeout_ms) {
    if (!lease.client || !lease.descriptor.has_value()) return false;
    if (lease.client->isConnected()) return true;
    if (lease.descriptor->endpoint.empty()) return false;
    return lease.client->connect(lease.descriptor->endpoint, timeout_ms);
}

RouteRequestResult sendLiveRouteRequest(const RuntimeRouteLease& lease,
                                        const std::string& method, const json& params,
                                        int timeout_ms, bool repeatable) {
    auto response = lease.sendRequest(method, params, timeout_ms);
    if (!response.isErr() || !repeatable ||
        !ipc::transportFailureState(response.error()).has_value() ||
        !reconnectRuntimeRoute(lease)) {
        return RouteRequestResult{std::move(response), false, false};
    }
    DIDI_LOG_WARN("RUNTIME_ROUTE", "Repeating ", method,
                  " on a new connection after a transport failure: ",
                  response.error().message);
    auto repeat = lease.sendRequest(method, params, timeout_ms);
    const bool answered = !repeat.isErr();
    return RouteRequestResult{std::move(repeat), true, answered};
}

bool quarantineRuntimeRoute(const std::shared_ptr<ipc::IIpcClient>& router,
                            const RuntimeRouteLease& lease) {
    const auto provider = std::dynamic_pointer_cast<IRuntimeRouteLeaseProvider>(router);
    if (provider) return provider->quarantineRoute(lease);
    if (!lease.client) return false;
    lease.client->disconnect();
    return true;
}

std::shared_ptr<IRuntimeSessionClient> createRuntimeSessionClient(const std::string& project_root,
                                                                   ipc::IpcClientFactory ipc_client_factory,
                                                                   DescriptorOpenedHook descriptor_opened_hook) {
    return std::make_shared<RuntimeSessionClient>(project_root, std::move(ipc_client_factory),
                                                   std::move(descriptor_opened_hook));
}

} // namespace didi::runtime
