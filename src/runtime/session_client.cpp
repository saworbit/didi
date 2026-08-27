#include "didi/runtime/session_client.hpp"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <cmath>
#include <mutex>
#include <limits>
#include <sstream>
#include <set>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <fcntl.h>
#include <libproc.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace didi::runtime {

Result<ProcessIdentity> queryProcessIdentity(uint64_t pid) {
    if (pid == 0) return Error::invalidArgument("Process identity requires a non-zero PID");
#if defined(_WIN32)
    if (pid > std::numeric_limits<DWORD>::max()) {
        return Error::notFound("Process identity is unavailable");
    }
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (!process) return Error::notFound("Process identity is unavailable");
    DWORD exit_code = 0;
    FILETIME created{}, exited{}, kernel{}, user{};
    const bool running = GetExitCodeProcess(process, &exit_code) && exit_code == STILL_ACTIVE;
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

bool isLowerHex(const std::string& value, size_t length) {
    return value.size() == length && std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

bool hasOnlyDescriptorFields(const json& value) {
    static const std::set<std::string> fields = {
        "schema_version", "session_id", "token", "pid", "kind", "project_path",
        "endpoint", "started_at_ms", "protocol_version"
    };
    if (value.size() != fields.size()) return false;
    for (auto it = value.begin(); it != value.end(); ++it) {
        if (!fields.count(it.key())) return false;
    }
    return true;
}

bool validEndpoint(const std::string& endpoint, uint64_t pid, const std::string& session_id) {
    if (endpoint.empty() || endpoint.find_first_of("\r\n") != std::string::npos) return false;
    const auto stem = "godot_didi_" + std::to_string(pid) + "_" + session_id;
#if defined(_WIN32)
    return endpoint == "\\\\.\\pipe\\" + stem;
#else
    const std::filesystem::path path(endpoint);
    if (!path.is_absolute() || path.filename() != stem + ".sock") return false;
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

std::filesystem::path descriptorDirectory() {
    const char* configured = std::getenv("DIDI_SESSION_DIR");
    if (configured && *configured) return canonicalPath(configured);
    std::error_code ec;
    const auto temp = std::filesystem::temp_directory_path(ec);
    return canonicalPath((ec ? std::filesystem::current_path() : temp) / "didi-sessions");
}

Result<std::string> readDescriptorFromValidatedHandle(
    const std::filesystem::path& directory,
    const std::filesystem::path& path,
    const DescriptorOpenedHook& opened_hook) {
#if defined(_WIN32)
    HANDLE handle = CreateFileW(
        path.wstring().c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return Error::notFound("Descriptor is not readable");

    FILE_ATTRIBUTE_TAG_INFO attributes{};
    if (!GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &attributes,
                                      sizeof(attributes))) {
        CloseHandle(handle);
        return Error::notFound("Descriptor is not readable");
    }
    if ((attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        CloseHandle(handle);
        return Error::invalidArgument("Descriptor must not be a symlink or reparse point");
    }
    if ((attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ||
        GetFileType(handle) != FILE_TYPE_DISK) {
        CloseHandle(handle);
        return Error::invalidArgument("Descriptor must be a regular file");
    }

    std::wstring final_path(32768, L'\0');
    const DWORD final_length = GetFinalPathNameByHandleW(
        handle, final_path.data(), static_cast<DWORD>(final_path.size()), FILE_NAME_NORMALIZED);
    if (final_length == 0 || final_length >= final_path.size()) {
        CloseHandle(handle);
        return Error::invalidArgument("Descriptor final path is unverifiable");
    }
    final_path.resize(final_length);
    constexpr wchar_t kExtendedPrefix[] = L"\\\\?\\";
    if (final_path.rfind(kExtendedPrefix, 0) == 0) final_path.erase(0, 4);
    std::error_code equivalent_error;
    const bool same_parent = std::filesystem::equivalent(
        std::filesystem::path(final_path).parent_path(), directory, equivalent_error);
    if (equivalent_error || !same_parent) {
        CloseHandle(handle);
        return Error::invalidArgument("Descriptor escaped descriptor directory");
    }

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(handle, &size) || size.QuadPart < 0 ||
        static_cast<uint64_t>(size.QuadPart) > kMaxDescriptorBytes) {
        CloseHandle(handle);
        return Error::invalidArgument("Descriptor exceeds 64 KiB limit");
    }
    if (opened_hook) opened_hook(path);

    std::string contents(static_cast<size_t>(size.QuadPart), '\0');
    DWORD bytes_read = 0;
    const bool read_ok = contents.empty() ||
                         ReadFile(handle, contents.data(), static_cast<DWORD>(contents.size()),
                                  &bytes_read, nullptr);
    CloseHandle(handle);
    if (!read_ok || bytes_read != contents.size()) {
        return Error::internal("Descriptor read failed");
    }
    return contents;
#else
    const int directory_fd = open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory_fd < 0) return Error::notFound("Session descriptor directory is not readable");
    const int descriptor_fd = openat(directory_fd, path.filename().c_str(),
                                     O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    const int open_error = errno;
    close(directory_fd);
    if (descriptor_fd < 0) {
        if (open_error == ELOOP) {
            return Error::invalidArgument("Descriptor must not be a symlink or reparse point");
        }
        return Error::notFound("Descriptor is not readable");
    }

    struct stat info{};
    if (fstat(descriptor_fd, &info) != 0) {
        close(descriptor_fd);
        return Error::notFound("Descriptor is not readable");
    }
    if (!S_ISREG(info.st_mode)) {
        close(descriptor_fd);
        return Error::invalidArgument("Descriptor must be a regular file");
    }
    if (info.st_size < 0 || static_cast<uint64_t>(info.st_size) > kMaxDescriptorBytes) {
        close(descriptor_fd);
        return Error::invalidArgument("Descriptor exceeds 64 KiB limit");
    }
    if (opened_hook) opened_hook(path);

    std::string contents(static_cast<size_t>(info.st_size), '\0');
    size_t offset = 0;
    while (offset < contents.size()) {
        const auto count = read(descriptor_fd, contents.data() + offset, contents.size() - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) {
            close(descriptor_fd);
            return Error::internal("Descriptor read failed");
        }
        offset += static_cast<size_t>(count);
    }
    close(descriptor_fd);
    return contents;
#endif
}

bool isProcessInstanceAlive(uint64_t pid, int64_t started_at_ms) {
    const auto identity = queryProcessIdentity(pid);
    return identity.isOk() &&
           std::llabs(identity.value().started_at_ms - started_at_ms) <=
               identity.value().resolution_ms;
}

struct DiscoveredSession {
    SessionDescriptor descriptor;
    bool alive{false};
};

std::vector<DiscoveredSession> discoverSessions(json& diagnostics,
                                                const DescriptorOpenedHook& opened_hook) {
    std::vector<DiscoveredSession> sessions;
    const auto directory = descriptorDirectory();
    std::error_code ec;
    if (!std::filesystem::exists(directory, ec)) return sessions;
    if (ec || !std::filesystem::is_directory(directory, ec)) {
        diagnostics.push_back({{"path", directory.string()}, {"error", "Session descriptor directory is not readable"}});
        return sessions;
    }

    for (std::filesystem::directory_iterator it(directory, ec), end; !ec && it != end; it.increment(ec)) {
        const auto& entry = *it;
        const auto path = entry.path();
        if (path.extension() != ".json") continue;
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
            descriptor.project_path = canonicalPath(descriptor.project_path).string();
            const bool alive = isProcessInstanceAlive(descriptor.pid, descriptor.started_at_ms);
            sessions.push_back({std::move(descriptor), alive});
        } catch (const std::exception& error) {
            diagnostics.push_back({{"path", path.string()}, {"error", std::string("Malformed descriptor: ") + error.what()}});
        }
    }
    if (ec) diagnostics.push_back({{"path", directory.string()}, {"error", "Failed while scanning descriptor directory"}});
    return sessions;
}

class RuntimeSessionClient final : public IRuntimeSessionClient {
public:
    RuntimeSessionClient(std::string project_root, ipc::IpcClientFactory factory,
                         DescriptorOpenedHook opened_hook)
        : m_projectRoot(canonicalPath(project_root).string()), m_factory(std::move(factory)),
          m_descriptorOpenedHook(std::move(opened_hook)) {}

    bool connect(const std::string&, int) override { return isConnected(); }

    void disconnect() override {
        std::shared_ptr<ipc::IIpcClient> previous;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            previous = std::move(m_activeClient);
            m_activeDescriptor.reset();
        }
        if (previous) previous->disconnect();
    }

    bool isConnected() const override {
        std::shared_ptr<ipc::IIpcClient> active;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            active = m_activeClient;
        }
        return active && active->isConnected();
    }

    Result<json> sendRequest(const std::string& method, const json& params, int timeout_ms) override {
        std::shared_ptr<ipc::IIpcClient> active;
        std::optional<SessionDescriptor> descriptor;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            active = m_activeClient;
            descriptor = m_activeDescriptor;
        }
        if (!active || !descriptor.has_value() || !active->isConnected()) {
            return Error::notConnected("No runtime session is attached");
        }
        json routed_params = params.is_object() ? params : json::object();
        routed_params["_didi_session_token"] = descriptor->token;
        return active->sendRequest(method, routed_params, timeout_ms);
    }

    Result<json> listSessions(const std::optional<std::string>& project_path) override {
        json diagnostics = json::array();
        const auto filter = project_path.has_value() ? canonicalPath(*project_path).string() : std::string{};
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

    Result<json> attachSession(const std::string& session_id) override {
        json diagnostics = json::array();
        const auto sessions = discoverSessions(diagnostics, m_descriptorOpenedHook);
        auto found = std::find_if(sessions.begin(), sessions.end(), [&](const DiscoveredSession& item) {
            return item.descriptor.session_id == session_id;
        });
        if (found == sessions.end()) return Error::notFound("Runtime session not found: " + session_id);
        if (!found->alive) return Error::notConnected("Runtime session is stale: " + session_id);
        if (!m_factory) return Error::internal("Runtime IPC client factory is not configured");

        auto candidate = std::shared_ptr<ipc::IIpcClient>(m_factory());
        if (!candidate || !candidate->connect(found->descriptor.endpoint, 2000)) {
            return Error::notConnected("Unable to connect to runtime session: " + session_id);
        }
        json handshake_params = {{"_didi_session_token", found->descriptor.token},
                                 {"protocol_version", "1.3"}};
        auto handshake = candidate->sendRequest("session.handshake", handshake_params,
                                                kSessionHandshakeTimeoutMs);
        if (handshake.isErr()) {
            candidate->disconnect();
            return handshake.error();
        }
        const auto& response = handshake.value();
        if (!response.is_object() || !response.contains("status") || !response["status"].is_string() ||
            response["status"] != "ok" || !response.contains("session_id") ||
            !response["session_id"].is_string() || response["session_id"] != found->descriptor.session_id ||
            !response.contains("protocol_version") || !response["protocol_version"].is_string() ||
            response["protocol_version"] != "1.3") {
            candidate->disconnect();
            return Error(409, "Runtime session handshake was not accepted by the descriptor contract");
        }

        std::shared_ptr<ipc::IIpcClient> previous;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            previous = std::move(m_activeClient);
            m_activeClient = std::move(candidate);
            m_activeDescriptor = found->descriptor;
        }
        if (previous) previous->disconnect();
        return json{{"session", found->descriptor.toJson()}, {"handshake", handshake.value()}};
    }

    Result<json> detachSession() override {
        std::shared_ptr<ipc::IIpcClient> previous;
        std::optional<SessionDescriptor> descriptor;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_activeDescriptor.has_value()) return Error::notConnected("No runtime session is attached");
            previous = std::move(m_activeClient);
            descriptor = std::move(m_activeDescriptor);
        }
        if (previous) previous->disconnect();
        return json{{"session", descriptor->toJson()}};
    }

    std::optional<SessionDescriptor> activeSession() const override {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_activeDescriptor;
    }

private:
    std::string m_projectRoot;
    ipc::IpcClientFactory m_factory;
    DescriptorOpenedHook m_descriptorOpenedHook;
    mutable std::mutex m_mutex;
    std::shared_ptr<ipc::IIpcClient> m_activeClient;
    std::optional<SessionDescriptor> m_activeDescriptor;
};

} // namespace

json SessionDescriptor::toJson(bool include_token) const {
    json value = {
        {"schema_version", schema_version}, {"session_id", session_id}, {"pid", pid},
        {"kind", kind}, {"project_path", project_path}, {"endpoint", endpoint},
        {"started_at_ms", started_at_ms}, {"protocol_version", protocol_version}
    };
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
                           value.at("session_id").get<std::string>()) ||
            !value.at("started_at_ms").is_number_integer() || value.at("started_at_ms").get<int64_t>() <= 0 ||
            !value.at("protocol_version").is_string() || value.at("protocol_version") != "1.3") {
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
        return descriptor;
    } catch (const std::exception&) {
        return Error::invalidArgument("Invalid session descriptor values");
    }
}

std::shared_ptr<IRuntimeSessionClient> createRuntimeSessionClient(const std::string& project_root,
                                                                   ipc::IpcClientFactory ipc_client_factory,
                                                                   DescriptorOpenedHook descriptor_opened_hook) {
    return std::make_shared<RuntimeSessionClient>(project_root, std::move(ipc_client_factory),
                                                   std::move(descriptor_opened_hook));
}

} // namespace didi::runtime
