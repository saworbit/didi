#include "didi/gdextension/session_host.hpp"

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

std::filesystem::path sessionDirectory() {
    const char* configured = std::getenv("DIDI_SESSION_DIR");
    if (configured && *configured) return canonicalPath(configured);
    std::error_code ec;
    const auto temp = std::filesystem::temp_directory_path(ec);
    return canonicalPath((ec ? std::filesystem::current_path() : temp) / "didi-sessions");
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
    if (left.size() != right.size()) return false;
    unsigned char difference = 0;
    for (size_t index = 0; index < left.size(); ++index) {
        difference |= static_cast<unsigned char>(left[index]) ^ static_cast<unsigned char>(right[index]);
    }
    return difference == 0;
}

Result<void> writeDescriptorAtomically(const std::filesystem::path& destination, const json& descriptor) {
    const auto directory = destination.parent_path();
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    if (ec) return Error::internal("Unable to create session descriptor directory: " + ec.message());
#if !defined(_WIN32)
    chmod(directory.c_str(), S_IRWXU);
#endif
    const auto temporary = destination.string() + ".tmp";
    const auto contents = descriptor.dump();
#if defined(_WIN32)
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
    if (!MoveFileExA(temporary.c_str(), destination.string().c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::filesystem::remove(temporary, ec);
        return Error::internal("Unable to atomically publish session descriptor");
    }
#else
    const int fd = open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, S_IRUSR | S_IWUSR);
    if (fd < 0) return Error::internal("Unable to create temporary session descriptor");
    size_t offset = 0;
    while (offset < contents.size()) {
        const auto written = write(fd, contents.data() + offset, contents.size() - offset);
        if (written > 0) {
            offset += static_cast<size_t>(written);
            continue;
        }
        if (written < 0 && errno == EINTR) continue;
        close(fd);
        unlink(temporary.c_str());
        return Error::internal("Unable to write temporary session descriptor");
    }
    if (fsync(fd) != 0) {
        close(fd);
        unlink(temporary.c_str());
        return Error::internal("Unable to sync temporary session descriptor");
    }
    close(fd);
    if (rename(temporary.c_str(), destination.c_str()) != 0) {
        unlink(temporary.c_str());
        return Error::internal("Unable to atomically publish session descriptor");
    }
    const int directory_fd = open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory_fd >= 0) {
        fsync(directory_fd);
        close(directory_fd);
    }
#endif
    return Result<void>::ok();
}

bool isOwnedDescriptor(const std::filesystem::path& path, const runtime::SessionDescriptor& descriptor) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec) || ec) return false;
    std::ifstream input(path, std::ios::binary);
    if (!input) return false;
    try {
        auto decoded = runtime::SessionDescriptor::fromJson(json::parse(input));
        return decoded.isOk() && decoded.value().session_id == descriptor.session_id &&
               secureEquals(decoded.value().token, descriptor.token);
    } catch (const std::exception&) {
        return false;
    }
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
    descriptor.kind = kind;
    descriptor.project_path = canonicalPath(project_path).string();
#if defined(_WIN32)
    descriptor.endpoint = "\\\\.\\pipe\\godot_didi_" + std::to_string(descriptor.pid) + "_" + descriptor.session_id;
#else
    descriptor.endpoint = (std::filesystem::temp_directory_path() /
                           ("godot_didi_" + std::to_string(descriptor.pid) + "_" + descriptor.session_id + ".sock")).string();
#endif
    descriptor.started_at_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    descriptor.protocol_version = "1.3";
    m_descriptorPath = sessionDirectory() / (descriptor.session_id + ".json");
    m_descriptor = std::move(descriptor);
    m_published = false;
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
    if (m_published && m_descriptor.has_value() && isOwnedDescriptor(m_descriptorPath, *m_descriptor)) {
        std::error_code ec;
        std::filesystem::remove(m_descriptorPath, ec);
    }
    m_published = false;
    m_descriptor.reset();
    m_descriptorPath.clear();
}

} // namespace didi::godot
