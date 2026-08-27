#include "didi/runtime/session_client.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <signal.h>
#endif

namespace didi::runtime {
namespace {

constexpr uintmax_t kMaxDescriptorBytes = 64 * 1024;

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

bool validEndpoint(const std::string& endpoint) {
    if (endpoint.empty() || endpoint.find_first_of("\r\n") != std::string::npos) return false;
#if defined(_WIN32)
    return endpoint.rfind("\\\\.\\pipe\\godot_didi_", 0) == 0;
#else
    return std::filesystem::path(endpoint).is_absolute();
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

bool isReparseOrSymlink(const std::filesystem::directory_entry& entry) {
    std::error_code ec;
    if (entry.is_symlink(ec)) return true;
#if defined(_WIN32)
    const auto attrs = GetFileAttributesW(entry.path().wstring().c_str());
    return attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
    return false;
#endif
}

bool isPidRunning(uint64_t pid) {
    if (pid == 0) return false;
#if defined(_WIN32)
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (!process) return false;
    DWORD exit_code = 0;
    const bool running = GetExitCodeProcess(process, &exit_code) && exit_code == STILL_ACTIVE;
    CloseHandle(process);
    return running;
#else
    if (kill(static_cast<pid_t>(pid), 0) == 0) return true;
    return errno == EPERM;
#endif
}

struct DiscoveredSession {
    SessionDescriptor descriptor;
    bool alive{false};
};

std::vector<DiscoveredSession> discoverSessions(json& diagnostics) {
    std::vector<DiscoveredSession> sessions;
    const auto directory = descriptorDirectory();
    std::error_code ec;
    if (!std::filesystem::exists(directory, ec)) return sessions;
    if (ec || !std::filesystem::is_directory(directory, ec)) {
        diagnostics.push_back({{"path", directory.string()}, {"error", "Session descriptor directory is not readable"}});
        return sessions;
    }

    const auto canonical_directory = canonicalPath(directory);
    for (std::filesystem::directory_iterator it(directory, ec), end; !ec && it != end; it.increment(ec)) {
        const auto& entry = *it;
        const auto path = entry.path();
        if (path.extension() != ".json") continue;
        if (isReparseOrSymlink(entry)) {
            diagnostics.push_back({{"path", path.string()}, {"error", "Descriptor must not be a symlink or reparse point"}});
            continue;
        }
        if (!entry.is_regular_file(ec) || ec) {
            diagnostics.push_back({{"path", path.string()}, {"error", "Descriptor must be a regular file"}});
            ec.clear();
            continue;
        }
        const auto canonical_file = canonicalPath(path);
        if (canonical_file.parent_path() != canonical_directory) {
            diagnostics.push_back({{"path", path.string()}, {"error", "Descriptor escaped descriptor directory"}});
            continue;
        }
        const auto bytes = entry.file_size(ec);
        if (ec || bytes > kMaxDescriptorBytes) {
            diagnostics.push_back({{"path", path.string()}, {"error", "Descriptor exceeds 64 KiB limit"}});
            ec.clear();
            continue;
        }
        std::ifstream input(path, std::ios::binary);
        std::string contents((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        try {
            auto decoded = SessionDescriptor::fromJson(json::parse(contents));
            if (decoded.isErr()) {
                diagnostics.push_back({{"path", path.string()}, {"error", decoded.error().message}});
                continue;
            }
            auto descriptor = decoded.value();
            descriptor.project_path = canonicalPath(descriptor.project_path).string();
            sessions.push_back({std::move(descriptor), isPidRunning(decoded.value().pid)});
        } catch (const std::exception& error) {
            diagnostics.push_back({{"path", path.string()}, {"error", std::string("Malformed descriptor: ") + error.what()}});
        }
    }
    if (ec) diagnostics.push_back({{"path", directory.string()}, {"error", "Failed while scanning descriptor directory"}});
    return sessions;
}

class RuntimeSessionClient final : public IRuntimeSessionClient {
public:
    RuntimeSessionClient(std::string project_root, ipc::IpcClientFactory factory)
        : m_projectRoot(canonicalPath(project_root).string()), m_factory(std::move(factory)) {}

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
        for (const auto& session : discoverSessions(diagnostics)) {
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
        const auto sessions = discoverSessions(diagnostics);
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
                                                ipc::kWaitForDefinitiveResponse);
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
            !value.at("endpoint").is_string() || !validEndpoint(value.at("endpoint").get<std::string>()) ||
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
                                                                   ipc::IpcClientFactory ipc_client_factory) {
    return std::make_shared<RuntimeSessionClient>(project_root, std::move(ipc_client_factory));
}

} // namespace didi::runtime
