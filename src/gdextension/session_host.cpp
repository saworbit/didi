#include "didi/gdextension/session_host.hpp"
#include "didi/common/project_path.hpp"

#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <system_error>

#if defined(_WIN32)
#include <windows.h>
#include <bcrypt.h>
#else
#include <cerrno>
#include <fcntl.h>
#if defined(__linux__)
#include <sys/random.h>
#endif
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace didi::godot {
namespace {

std::filesystem::path canonicalPath(const std::filesystem::path& path) {
    std::error_code ec;
    const auto resolved = std::filesystem::weakly_canonical(path, ec);
    return ec ? path.lexically_normal() : resolved;
}

Result<std::filesystem::path> sessionDirectory() {
    return runtime::resolveSessionDescriptorDirectory();
}

uint64_t processId() {
#if defined(_WIN32)
    return static_cast<uint64_t>(GetCurrentProcessId());
#else
    return static_cast<uint64_t>(getpid());
#endif
}

Result<std::vector<uint8_t>> secureRandom(size_t count) {
    std::vector<uint8_t> bytes(count);
#if defined(_WIN32)
    if (BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()),
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
        return Error::internal("BCryptGenRandom failed while creating the session secret");
    }
#else
    size_t offset = 0;
#if defined(__linux__)
    while (offset < bytes.size()) {
        const auto read = getrandom(bytes.data() + offset, bytes.size() - offset, 0);
        if (read > 0) {
            offset += static_cast<size_t>(read);
            continue;
        }
        if (read < 0 && errno == EINTR) continue;
        break;
    }
#endif
    if (offset < bytes.size()) {
        const int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
        if (fd < 0) return Error::internal("Unable to open /dev/urandom for session secret generation");
        while (offset < bytes.size()) {
            const auto read = ::read(fd, bytes.data() + offset, bytes.size() - offset);
            if (read > 0) {
                offset += static_cast<size_t>(read);
                continue;
            }
            if (read < 0 && errno == EINTR) continue;
            close(fd);
            return Error::internal("Unable to read /dev/urandom for session secret generation");
        }
        close(fd);
    }
#endif
    return bytes;
}

std::string lowerHex(const std::vector<uint8_t>& bytes) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(bytes.size() * 2);
    for (const auto byte : bytes) {
        result.push_back(digits[(byte >> 4) & 0x0f]);
        result.push_back(digits[byte & 0x0f]);
    }
    return result;
}

bool secureEquals(const std::string& left, const std::string& right) {
    unsigned char difference = 0;
    constexpr size_t kTokenBytes = 64;
    for (size_t index = 0; index < kTokenBytes; ++index) {
        const auto left_byte = index < left.size() ? static_cast<unsigned char>(left[index]) : 0;
        const auto right_byte = index < right.size() ? static_cast<unsigned char>(right[index]) : 0;
        difference |= left_byte ^ right_byte;
    }
    return difference == 0 && left.size() == kTokenBytes && right.size() == kTokenBytes;
}

Result<void> ensureDescriptorDirectory(const std::filesystem::path& directory) {
#if defined(_WIN32)
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    if (ec) return Error::internal("Unable to create session descriptor directory: " + ec.message());
#else
    struct stat status {};
    if (lstat(directory.c_str(), &status) != 0) {
        if (errno != ENOENT || mkdir(directory.c_str(), S_IRWXU) != 0) {
            return Error::internal("Unable to create secure session descriptor directory");
        }
    }
    const int directory_fd = open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (directory_fd < 0 || fstat(directory_fd, &status) != 0 || !S_ISDIR(status.st_mode)) {
        if (directory_fd >= 0) close(directory_fd);
        return Error::internal("Session descriptor directory is not a real directory");
    }
    if (status.st_uid != geteuid()) {
        close(directory_fd);
        return Error::internal("Session descriptor directory is not owned by the current user");
    }
    if (fchmod(directory_fd, S_IRWXU) != 0) {
        close(directory_fd);
        return Error::internal("Unable to restrict session descriptor directory permissions");
    }
    if (fstat(directory_fd, &status) != 0 || status.st_uid != geteuid() ||
        (status.st_mode & (S_IRWXG | S_IRWXO)) != 0) {
        close(directory_fd);
        return Error::internal("Session descriptor directory permissions are not owner-only");
    }
    close(directory_fd);
#endif
    return Result<void>::ok();
}

Result<void> writeDescriptorAtomically(const std::filesystem::path& destination, const json& descriptor) {
    const auto directory = destination.parent_path();
    auto secured = ensureDescriptorDirectory(directory);
    if (secured.isErr()) return secured.error();
    const auto contents = descriptor.dump();
#if defined(_WIN32)
    std::error_code ec;
    // Wide throughout. The narrow forms here go through the active code page,
    // so a Unicode %TEMP%, which is what a non-ASCII Windows user name gives
    // you, made publishing the descriptor fail before discovery could start.
    std::filesystem::path temporary = destination;
    temporary += ".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) return Error::internal("Unable to create temporary session descriptor");
        output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        output.flush();
        if (!output) {
            std::filesystem::remove(temporary, ec);
            return Error::internal("Unable to write temporary session descriptor");
        }
    }
    if (!MoveFileExW(temporary.c_str(), destination.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::filesystem::remove(temporary, ec);
        return Error::internal("Unable to atomically publish session descriptor");
    }
#else
    const int directory_fd = open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (directory_fd < 0) return Error::internal("Unable to open secure session descriptor directory");
    const auto temporary_name = destination.filename().string() + ".tmp";
    const int fd = openat(directory_fd, temporary_name.c_str(),
                          O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
                          S_IRUSR | S_IWUSR);
    if (fd < 0) {
        close(directory_fd);
        return Error::internal("Unable to create temporary session descriptor");
    }
    struct stat descriptor_status{};
    if (fstat(fd, &descriptor_status) != 0 || !S_ISREG(descriptor_status.st_mode) ||
        descriptor_status.st_uid != geteuid() ||
        (descriptor_status.st_mode & (S_IRWXG | S_IRWXO)) != 0) {
        close(fd);
        unlinkat(directory_fd, temporary_name.c_str(), 0);
        close(directory_fd);
        return Error::internal("Temporary session descriptor is not owner-only");
    }
    size_t offset = 0;
    while (offset < contents.size()) {
        const auto written = write(fd, contents.data() + offset, contents.size() - offset);
        if (written > 0) {
            offset += static_cast<size_t>(written);
            continue;
        }
        if (written < 0 && errno == EINTR) continue;
        close(fd);
        unlinkat(directory_fd, temporary_name.c_str(), 0);
        close(directory_fd);
        return Error::internal("Unable to write temporary session descriptor");
    }
    if (fsync(fd) != 0) {
        close(fd);
        unlinkat(directory_fd, temporary_name.c_str(), 0);
        close(directory_fd);
        return Error::internal("Unable to sync temporary session descriptor");
    }
    close(fd);
    if (renameat(directory_fd, temporary_name.c_str(), directory_fd,
                 destination.filename().c_str()) != 0) {
        unlinkat(directory_fd, temporary_name.c_str(), 0);
        close(directory_fd);
        return Error::internal("Unable to atomically publish session descriptor");
    }
    fsync(directory_fd);
    close(directory_fd);
#endif
    return Result<void>::ok();
}

} // namespace

Result<void> SessionHost::prepare(const std::string& kind, const std::string& project_path) {
    if (kind != "editor" && kind != "game") return Error::invalidArgument("Session kind must be editor or game");
    if (project_path.empty()) return Error::invalidArgument("Session project path must not be empty");

    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_descriptor.has_value()) return Error(409, "Session host is already prepared");
    auto session_bytes = secureRandom(16);
    if (session_bytes.isErr()) return session_bytes.error();
    auto token_bytes = secureRandom(32);
    if (token_bytes.isErr()) return token_bytes.error();

    runtime::SessionDescriptor descriptor;
    descriptor.session_id = lowerHex(session_bytes.value());
    descriptor.token = lowerHex(token_bytes.value());
    descriptor.pid = processId();
    const auto process_identity = runtime::queryProcessIdentity(descriptor.pid);
    if (process_identity.isErr()) {
        return Error::internal("Unable to establish the runtime process identity: " +
                               process_identity.error().message);
    }
    descriptor.kind = kind;
    descriptor.project_path =
        paths::nativePathToUtf8(canonicalPath(paths::projectPathFromUtf8(project_path)));
    const auto project_key =
        paths::projectEndpointKey(paths::projectPathFromUtf8(descriptor.project_path));
#if defined(_WIN32)
    descriptor.endpoint = "\\\\.\\pipe\\godot_didi_" + project_key + "_" +
                          std::to_string(descriptor.pid) + "_" + descriptor.session_id;
#else
    std::error_code temp_error;
    const auto temp_directory = std::filesystem::temp_directory_path(temp_error);
    if (temp_error) return Error::internal("Unable to resolve the system temporary directory for the IPC endpoint: " + temp_error.message());
    descriptor.endpoint = (temp_directory /
                           ("godot_didi_" + project_key + "_" + std::to_string(descriptor.pid) +
                            "_" + descriptor.session_id.substr(0, 12) + ".sock")).string();
#endif
    descriptor.started_at_ms = process_identity.value().started_at_ms;
    descriptor.protocol_version = "1.3";
    auto directory = sessionDirectory();
    if (directory.isErr()) return directory.error();
    m_descriptorPath = directory.value() / (descriptor.session_id + ".json");
    m_descriptor = std::move(descriptor);
    m_published = false;
    return Result<void>::ok();
}

Result<void> SessionHost::startServer(ipc::IIpcServer& server) {
    const auto prepared = descriptor();
    if (!prepared.has_value()) return Error(409, "Session host is not prepared");
    if (!server.start(prepared->endpoint)) {
        server.stop();
        stop();
        return Error::notConnected("Unable to bind runtime IPC endpoint");
    }
    const auto published = publish();
    if (published.isErr()) {
        server.stop();
        stop();
        return published.error();
    }
    return Result<void>::ok();
}

Result<void> SessionHost::publish() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_descriptor.has_value()) return Error(409, "Session host is not prepared");
    if (m_published) return Result<void>::ok();
    auto published = writeDescriptorAtomically(m_descriptorPath, m_descriptor->toJson(true));
    if (published.isErr()) return published.error();
    m_published = true;
    return Result<void>::ok();
}

std::optional<runtime::SessionDescriptor> SessionHost::descriptor() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_descriptor;
}

Result<json> SessionHost::authorize(const json& request) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_descriptor.has_value()) return Error::notConnected("Runtime session host is not prepared");
    if (!request.is_object() || !request.value("params", json::object()).is_object()) {
        return Error::invalidArgument("IPC request must contain object params");
    }
    auto authorized = request;
    auto& params = authorized["params"];
    const auto token = params.value("_didi_session_token", std::string{});
    if (!secureEquals(token, m_descriptor->token)) return Error(401, "Runtime session token was rejected");
    params.erase("_didi_session_token");
    return authorized;
}

void SessionHost::stop() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_published && m_descriptor.has_value()) {
        (void)runtime::retireOwnedSessionDescriptor(
            m_descriptorPath, *m_descriptor, m_beforeRetirementHook,
            m_afterRetiredVerificationHook);
    }
    m_published = false;
    m_descriptor.reset();
    m_descriptorPath.clear();
}

void SessionHost::setBeforeRetirementHookForTesting(std::function<void(const std::filesystem::path&)> hook) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_beforeRetirementHook = std::move(hook);
}

void SessionHost::setAfterRetiredVerificationHookForTesting(std::function<void(const std::filesystem::path&)> hook) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_afterRetiredVerificationHook = std::move(hook);
}

} // namespace didi::godot
