#include "didi/runtime/session_client.hpp"
#include "didi/gdextension/session_host.hpp"

#include <cstdlib>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <set>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <libproc.h>
#include <sys/stat.h>
#include <unistd.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

#define ASSERT_TRUE(cond) if (!(cond)) throw std::runtime_error("Assertion failed: " #cond);
#define ASSERT_FALSE(cond) ASSERT_TRUE(!(cond))
#define ASSERT_EQ(a, b) ASSERT_TRUE((a) == (b))

void registerTest(const std::string& name, std::function<void()> fn);

namespace {

int g_lastHandshakeTimeoutMs = 0;

uint64_t currentProcessId() {
#if defined(_WIN32)
    return static_cast<uint64_t>(GetCurrentProcessId());
#else
    return static_cast<uint64_t>(getpid());
#endif
}

size_t currentOpenHandleCount() {
#if defined(_WIN32)
    DWORD count = 0;
    ASSERT_TRUE(GetProcessHandleCount(GetCurrentProcess(), &count) != 0);
    return static_cast<size_t>(count);
#elif defined(__linux__)
    size_t count = 0;
    for (const auto& entry : std::filesystem::directory_iterator("/proc/self/fd")) {
        (void)entry;
        ++count;
    }
    return count;
#else
    return 0;
#endif
}

std::pair<int64_t, int64_t> currentProcessStartIdentity() {
#if defined(_WIN32)
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                 static_cast<DWORD>(currentProcessId()));
    ASSERT_TRUE(process != nullptr);
    FILETIME created{}, exited{}, kernel{}, user{};
    ASSERT_TRUE(GetProcessTimes(process, &created, &exited, &kernel, &user) != 0);
    CloseHandle(process);
    ULARGE_INTEGER ticks{};
    ticks.LowPart = created.dwLowDateTime;
    ticks.HighPart = created.dwHighDateTime;
    return {static_cast<int64_t>(ticks.QuadPart / 10000ULL - 11644473600000ULL), 1};
#elif defined(__linux__)
    std::ifstream stat("/proc/self/stat");
    std::ifstream system_stat("/proc/stat");
    std::string stat_line;
    ASSERT_TRUE(std::getline(stat, stat_line));
    const auto command_end = stat_line.rfind(')');
    ASSERT_TRUE(command_end != std::string::npos);
    std::istringstream fields(stat_line.substr(command_end + 2));
    std::string field;
    uint64_t start_ticks = 0;
    for (int index = 0; index <= 19; ++index) {
        ASSERT_TRUE(static_cast<bool>(fields >> field));
        if (index == 19) start_ticks = std::stoull(field);
    }
    int64_t boot_seconds = 0;
    std::string line;
    while (std::getline(system_stat, line)) {
        if (line.rfind("btime ", 0) == 0) {
            boot_seconds = std::stoll(line.substr(6));
            break;
        }
    }
    ASSERT_TRUE(boot_seconds > 0);
    const long ticks_per_second = sysconf(_SC_CLK_TCK);
    ASSERT_TRUE(ticks_per_second > 0);
    const int64_t resolution = std::max<int64_t>(1, (1000 + ticks_per_second - 1) / ticks_per_second);
    return {boot_seconds * 1000 + static_cast<int64_t>(start_ticks * 1000ULL /
                                                       static_cast<uint64_t>(ticks_per_second)), resolution};
#elif defined(__APPLE__)
    proc_bsdinfo info{};
    ASSERT_TRUE(proc_pidinfo(static_cast<int>(currentProcessId()), PROC_PIDTBSDINFO, 0,
                             &info, sizeof(info)) == sizeof(info));
    return {static_cast<int64_t>(info.pbi_start_tvsec) * 1000 +
            static_cast<int64_t>(info.pbi_start_tvusec) / 1000, 1};
#else
    throw std::runtime_error("Process start identity test is unsupported on this platform");
#endif
}

std::string endpointFor(const std::string& session_id, uint64_t pid = currentProcessId()) {
#if defined(_WIN32)
    return "\\\\.\\pipe\\godot_didi_" + std::to_string(pid) + "_" + session_id;
#else
    return (std::filesystem::temp_directory_path() /
            ("godot_didi_" + std::to_string(pid) + "_" + session_id + ".sock")).string();
#endif
}

didi::json validDescriptor(const std::string& session_id, const std::string& endpoint) {
    const auto [started_at_ms, resolution_ms] = currentProcessStartIdentity();
    (void)resolution_ms;
    return {
        {"schema_version", 1},
        {"session_id", session_id},
        {"token", std::string(64, 'a')},
        {"pid", currentProcessId()},
        {"kind", "editor"},
        {"project_path", std::filesystem::current_path().string()},
        {"endpoint", endpoint},
        {"started_at_ms", started_at_ms},
        {"protocol_version", "1.3"}
    };
}

class FakeIpcClient final : public didi::ipc::IIpcClient {
public:
    bool connect(const std::string& endpoint, int) override {
        m_endpoint = endpoint;
        m_connected = endpoint.find("fedcba9876543210fedcba9876543210") == std::string::npos;
        return m_connected;
    }

    void disconnect() override { m_connected = false; }
    bool isConnected() const override { return m_connected; }

    didi::Result<didi::json> sendRequest(const std::string& method, const didi::json& params, int timeout_ms) override {
        if (!m_connected) return didi::Error::notConnected();
        if (method == "session.handshake") {
            g_lastHandshakeTimeoutMs = timeout_ms;
            if (m_endpoint.find("77777777777777777777777777777777") != std::string::npos) {
                return didi::Error(504, "Black-hole handshake timed out");
            }
            if (params.value("_didi_session_token", "") != std::string(64, 'a')) {
                return didi::Error(401, "token rejected");
            }
            if (params.value("protocol_version", "") != "1.3") {
                return didi::Error(409, "protocol rejected");
            }
            if (m_endpoint.find("11111111111111111111111111111111") != std::string::npos) return didi::json::object();
            if (m_endpoint.find("22222222222222222222222222222222") != std::string::npos) return didi::json::array();
            const auto separator = m_endpoint.find_last_of('_');
            auto session_id = separator == std::string::npos ? std::string{} : m_endpoint.substr(separator + 1);
#if !defined(_WIN32)
            constexpr auto socket_suffix = ".sock";
            const auto suffix_size = std::char_traits<char>::length(socket_suffix);
            if (session_id.size() >= suffix_size &&
                session_id.compare(session_id.size() - suffix_size, suffix_size, socket_suffix) == 0) {
                session_id.resize(session_id.size() - suffix_size);
            }
#endif
            auto response = validDescriptor(session_id, m_endpoint);
            response.erase("token");
            response["status"] = "ok";
            if (m_endpoint.find("33333333333333333333333333333333") != std::string::npos) {
                response["status"] = "rejected";
                return response;
            }
            if (m_endpoint.find("44444444444444444444444444444444") != std::string::npos) {
                response["session_id"] = "fedcba9876543210fedcba9876543210";
                return response;
            }
            if (m_endpoint.find("55555555555555555555555555555555") != std::string::npos) {
                response["protocol_version"] = "1.2";
                return response;
            }
            return response;
        }
        return didi::json{{"endpoint", m_endpoint}, {"token", params.value("_didi_session_token", "")}};
    }

private:
    std::string m_endpoint;
    bool m_connected{false};
};

class FakeIpcServer final : public didi::ipc::IIpcServer {
public:
    explicit FakeIpcServer(bool start_result, std::function<void()> on_start = {})
        : m_startResult(start_result), m_onStart(std::move(on_start)) {}

    bool start(const std::string& endpoint) override {
        ++start_calls;
        started_endpoint = endpoint;
        if (m_onStart) m_onStart();
        running = m_startResult;
        return m_startResult;
    }
    void stop() override { ++stop_calls; running = false; }
    bool isRunning() const override { return running; }
    void setHandler(didi::ipc::MessageHandler handler) override { m_handler = std::move(handler); }

    int start_calls{0};
    int stop_calls{0};
    bool running{false};
    std::string started_endpoint;

private:
    bool m_startResult;
    std::function<void()> m_onStart;
    didi::ipc::MessageHandler m_handler;
};

std::filesystem::path makeSessionDirectory() {
    const auto directory = std::filesystem::temp_directory_path() /
                           ("didi-runtime-session-test-" + std::to_string(currentProcessId()));
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory);
#if !defined(_WIN32)
    ASSERT_TRUE(chmod(directory.c_str(), S_IRWXU) == 0);
#endif
    return directory;
}

void writeDescriptor(const std::filesystem::path& directory, const std::string& name,
                     const didi::json& descriptor) {
    std::ofstream output(directory / name, std::ios::binary);
    output << descriptor.dump();
    output.close();
#if !defined(_WIN32)
    ASSERT_TRUE(chmod((directory / name).c_str(), S_IRUSR | S_IWUSR) == 0);
#endif
}

void setSessionDirectory(const std::filesystem::path& directory) {
#if defined(_WIN32)
    _putenv_s("DIDI_SESSION_DIR", directory.string().c_str());
#else
    setenv("DIDI_SESSION_DIR", directory.string().c_str(), 1);
#endif
}

void clearSessionDirectory() {
#if defined(_WIN32)
    _putenv_s("DIDI_SESSION_DIR", "");
#else
    unsetenv("DIDI_SESSION_DIR");
#endif
}

void test_session_host_prepares_private_unique_descriptor_and_authorizes_without_token_forwarding() {
    // Would fail if prepare published early, generated a fixed endpoint, accepted a bad token, or forwarded the token.
    const auto directory = makeSessionDirectory();
    setSessionDirectory(directory);

    didi::godot::SessionHost host;
    ASSERT_TRUE(host.prepare("editor", std::filesystem::current_path().string()).isOk());
    const auto descriptor = host.descriptor();
    ASSERT_TRUE(descriptor.has_value());
    ASSERT_EQ(descriptor->pid, currentProcessId());
    const auto [process_started_at_ms, process_resolution_ms] = currentProcessStartIdentity();
    ASSERT_TRUE(std::llabs(descriptor->started_at_ms - process_started_at_ms) <= process_resolution_ms);
    ASSERT_TRUE(descriptor->endpoint.find(std::to_string(descriptor->pid)) != std::string::npos);
    ASSERT_TRUE(descriptor->endpoint.find(descriptor->session_id) != std::string::npos);
    ASSERT_TRUE(std::filesystem::is_empty(directory));

    const auto missing_token = host.authorize({{"method", "session.handshake"}, {"params", didi::json::object()}});
    ASSERT_TRUE(missing_token.isErr());
    ASSERT_EQ(missing_token.error().code, 401);

    const auto denied = host.authorize({{"method", "session.handshake"},
                                        {"params", {{"_didi_session_token", "wrong"}}}});
    ASSERT_TRUE(denied.isErr());
    ASSERT_EQ(denied.error().code, 401);

    const auto allowed = host.authorize({{"method", "session.handshake"},
                                         {"params", {{"_didi_session_token", descriptor->token}, {"depth", 2}}}});
    ASSERT_TRUE(allowed.isOk());
    ASSERT_TRUE(!allowed.value()["params"].contains("_didi_session_token"));
    ASSERT_EQ(allowed.value()["params"]["depth"], 2);

    host.stop();
    clearSessionDirectory();
    std::filesystem::remove_all(directory);
}

void test_session_host_publishes_atomically_and_removes_only_its_descriptor() {
    // Would fail if publication left a partial file, used permissive POSIX permissions, or stop removed another host's descriptor.
    const auto directory = makeSessionDirectory();
    setSessionDirectory(directory);

    didi::godot::SessionHost first;
    didi::godot::SessionHost second;
    ASSERT_TRUE(first.prepare("editor", std::filesystem::current_path().string()).isOk());
    ASSERT_TRUE(second.prepare("game", std::filesystem::current_path().string()).isOk());
    ASSERT_TRUE(first.publish().isOk());
    ASSERT_TRUE(second.publish().isOk());

    std::set<std::string> descriptor_names;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        ASSERT_EQ(entry.path().extension(), ".json");
        descriptor_names.insert(entry.path().filename().string());
#if !defined(_WIN32)
        const auto permissions = entry.status().permissions();
        ASSERT_TRUE((permissions & (std::filesystem::perms::group_all | std::filesystem::perms::others_all)) ==
                    std::filesystem::perms::none);
#endif
        std::ifstream input(entry.path(), std::ios::binary);
        ASSERT_TRUE(didi::runtime::SessionDescriptor::fromJson(didi::json::parse(input)).isOk());
    }
    ASSERT_EQ(descriptor_names.size(), 2u);

    const auto active_descriptor_count = [&] {
        size_t count = 0;
        for (const auto& entry : std::filesystem::directory_iterator(directory)) {
            if (entry.path().extension() == ".json") ++count;
        }
        return count;
    };
    first.stop();
    ASSERT_EQ(active_descriptor_count(), 1u);
    second.stop();
    ASSERT_EQ(active_descriptor_count(), 0u);
#if defined(_WIN32)
    ASSERT_TRUE(std::filesystem::is_empty(directory));
#else
    size_t retained_tombstones = 0;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        ASSERT_TRUE(entry.path().filename().string().find(".didi-retired-") != std::string::npos);
        ++retained_tombstones;
    }
    ASSERT_EQ(retained_tombstones, 2u);
#endif

    clearSessionDirectory();
    std::filesystem::remove_all(directory);
}

void test_session_host_fails_closed_when_descriptor_publication_becomes_unavailable() {
    // Would fail if publication reported success after its destination became unavailable.
    const auto directory = makeSessionDirectory();
    setSessionDirectory(directory);

    didi::godot::SessionHost host;
    ASSERT_TRUE(host.prepare("editor", std::filesystem::current_path().string()).isOk());
    const auto descriptor = host.descriptor();
    ASSERT_TRUE(descriptor.has_value());
    std::filesystem::remove_all(directory);
    std::ofstream blocking_file(directory, std::ios::binary);
    blocking_file << "not a directory";
    blocking_file.close();

    ASSERT_TRUE(host.publish().isErr());
    ASSERT_FALSE(std::filesystem::exists(directory / (descriptor->session_id + ".json")));

    host.stop();
    clearSessionDirectory();
    std::filesystem::remove(directory);
}

void test_session_host_stops_server_and_discards_descriptor_when_bind_fails() {
    // Would fail if a failed bind left a runnable server or a discoverable descriptor behind.
    const auto directory = makeSessionDirectory();
    setSessionDirectory(directory);
    didi::godot::SessionHost host;
    FakeIpcServer server(false);

    ASSERT_TRUE(host.prepare("editor", std::filesystem::current_path().string()).isOk());
    const auto started = host.startServer(server);

    ASSERT_TRUE(started.isErr());
    ASSERT_EQ(server.start_calls, 1);
    ASSERT_EQ(server.stop_calls, 1);
    ASSERT_FALSE(server.isRunning());
    ASSERT_FALSE(host.descriptor().has_value());
    ASSERT_TRUE(std::filesystem::is_empty(directory));

    clearSessionDirectory();
    std::filesystem::remove_all(directory);
}

void test_session_host_stops_bound_server_when_publication_fails() {
    // Would fail if a successfully bound server survived a failed descriptor publication.
    const auto directory = makeSessionDirectory();
    setSessionDirectory(directory);
    didi::godot::SessionHost host;
    FakeIpcServer server(true, [&] {
        std::filesystem::remove_all(directory);
        std::ofstream blocking_file(directory, std::ios::binary);
        blocking_file << "not a directory";
    });

    ASSERT_TRUE(host.prepare("editor", std::filesystem::current_path().string()).isOk());
    const auto started = host.startServer(server);

    ASSERT_TRUE(started.isErr());
    ASSERT_EQ(server.start_calls, 1);
    ASSERT_EQ(server.stop_calls, 1);
    ASSERT_FALSE(server.isRunning());
    ASSERT_FALSE(host.descriptor().has_value());
    clearSessionDirectory();
    std::filesystem::remove(directory);
}

void test_session_host_cleanup_preserves_same_path_replacement() {
    // Would fail if stop deleted a descriptor that another writer replaced at the host's original path.
    const auto directory = makeSessionDirectory();
    setSessionDirectory(directory);
    didi::godot::SessionHost host;
    ASSERT_TRUE(host.prepare("editor", std::filesystem::current_path().string()).isOk());
    const auto descriptor = host.descriptor();
    ASSERT_TRUE(descriptor.has_value());
    ASSERT_TRUE(host.publish().isOk());

    const auto replacement_path = directory / (descriptor->session_id + ".json");
    auto replacement = validDescriptor(descriptor->session_id, descriptor->endpoint);
    replacement["kind"] = descriptor->kind;
    replacement["project_path"] = descriptor->project_path;
    replacement["started_at_ms"] = descriptor->started_at_ms;
    host.setBeforeRetirementHookForTesting([&](const std::filesystem::path&) {
        writeDescriptor(directory, replacement_path.filename().string(), replacement);
    });
    host.stop();

    ASSERT_TRUE(std::filesystem::exists(replacement_path));
    std::ifstream input(replacement_path, std::ios::binary);
    ASSERT_EQ(didi::json::parse(input)["token"], std::string(64, 'a'));
    input.close();
    clearSessionDirectory();
    std::filesystem::remove_all(directory);
}

void test_session_host_retirement_collision_preserves_selected_destination() {
    // Would fail if retirement overwrote a file created after its destination was selected.
    const auto directory = makeSessionDirectory();
    setSessionDirectory(directory);
    didi::godot::SessionHost host;
    ASSERT_TRUE(host.prepare("editor", std::filesystem::current_path().string()).isOk());
    const auto descriptor = host.descriptor();
    ASSERT_TRUE(descriptor.has_value());
    ASSERT_TRUE(host.publish().isOk());

    std::filesystem::path collision_path;
    std::filesystem::path retired_path;
    bool collision_created = false;
    auto collision = validDescriptor(descriptor->session_id, descriptor->endpoint);
    collision["kind"] = descriptor->kind;
    collision["project_path"] = descriptor->project_path;
    collision["started_at_ms"] = descriptor->started_at_ms;
    host.setBeforeRetirementHookForTesting([&](const std::filesystem::path& selected) {
        if (!collision_created) {
            collision_created = true;
            collision_path = selected;
            writeDescriptor(directory, selected.filename().string(), collision);
        }
    });
    host.setAfterRetiredVerificationHookForTesting([&](const std::filesystem::path& verified) {
        retired_path = verified;
    });
    host.stop();

    ASSERT_TRUE(std::filesystem::exists(collision_path));
    std::ifstream collision_input(collision_path, std::ios::binary);
    ASSERT_EQ(didi::json::parse(collision_input)["token"], std::string(64, 'a'));
    collision_input.close();
#if defined(_WIN32)
    ASSERT_FALSE(std::filesystem::exists(retired_path));
#else
    ASSERT_TRUE(std::filesystem::exists(retired_path));
#endif
    ASSERT_TRUE(retired_path != collision_path);
    ASSERT_FALSE(std::filesystem::exists(directory / (descriptor->session_id + ".json")));
    clearSessionDirectory();
    std::filesystem::remove_all(directory);
}

void test_session_host_retirement_exhaustion_never_overwrites_collisions() {
    // Break caught: bounded retirement retries overwrite an attacker-selected destination.
    const auto directory = makeSessionDirectory();
    setSessionDirectory(directory);
    didi::godot::SessionHost host;
    ASSERT_TRUE(host.prepare("editor", std::filesystem::current_path().string()).isOk());
    const auto descriptor = host.descriptor();
    ASSERT_TRUE(descriptor.has_value());
    ASSERT_TRUE(host.publish().isOk());

    std::vector<std::filesystem::path> collisions;
    auto collision = validDescriptor(descriptor->session_id, descriptor->endpoint);
    collision["kind"] = descriptor->kind;
    collision["project_path"] = descriptor->project_path;
    collision["started_at_ms"] = descriptor->started_at_ms;
    host.setBeforeRetirementHookForTesting([&](const std::filesystem::path& selected) {
        collisions.push_back(selected);
        writeDescriptor(directory, selected.filename().string(), collision);
    });
    host.stop();

    ASSERT_EQ(collisions.size(), 8u);
    for (const auto& path : collisions) {
        ASSERT_TRUE(std::filesystem::exists(path));
        std::ifstream input(path, std::ios::binary);
        ASSERT_EQ(didi::json::parse(input)["token"], std::string(64, 'a'));
    }
    // Exhaustion fails safe: the owned active pathname remains rather than deleting
    // or overwriting a path whose identity cannot be proven after the collision race.
    ASSERT_TRUE(std::filesystem::exists(directory / (descriptor->session_id + ".json")));
    clearSessionDirectory();
    std::filesystem::remove_all(directory);
}

void test_session_host_retained_file_survives_replacement_after_verification() {
    // Would fail if cleanup deleted or overwrote the retired pathname after ownership verification.
    const auto directory = makeSessionDirectory();
    setSessionDirectory(directory);
    didi::godot::SessionHost host;
    ASSERT_TRUE(host.prepare("editor", std::filesystem::current_path().string()).isOk());
    const auto descriptor = host.descriptor();
    ASSERT_TRUE(descriptor.has_value());
    ASSERT_TRUE(host.publish().isOk());

    std::filesystem::path retired_path;
    auto replacement = validDescriptor(descriptor->session_id, descriptor->endpoint);
    replacement["kind"] = descriptor->kind;
    replacement["project_path"] = descriptor->project_path;
    replacement["started_at_ms"] = descriptor->started_at_ms;
    host.setAfterRetiredVerificationHookForTesting([&](const std::filesystem::path& verified) {
        retired_path = verified;
        writeDescriptor(directory, verified.filename().string(), replacement);
    });
    host.stop();

    ASSERT_FALSE(std::filesystem::exists(directory / (descriptor->session_id + ".json")));
    ASSERT_TRUE(std::filesystem::exists(retired_path));
    std::ifstream replacement_input(retired_path, std::ios::binary);
    ASSERT_EQ(didi::json::parse(replacement_input)["token"], std::string(64, 'a'));
    replacement_input.close();
    clearSessionDirectory();
    std::filesystem::remove_all(directory);
}

void test_runtime_session_discovery_ignores_retained_retirement_files() {
    // Would fail if retained non-.json retirement files appeared as discoverable sessions.
    const auto directory = makeSessionDirectory();
    setSessionDirectory(directory);
    didi::godot::SessionHost host;
    ASSERT_TRUE(host.prepare("editor", std::filesystem::current_path().string()).isOk());
    const auto descriptor = host.descriptor();
    ASSERT_TRUE(descriptor.has_value());
    ASSERT_TRUE(host.publish().isOk());
    std::filesystem::path retired_path;
    auto replacement = validDescriptor(descriptor->session_id, descriptor->endpoint);
    replacement["kind"] = descriptor->kind;
    replacement["project_path"] = descriptor->project_path;
    replacement["started_at_ms"] = descriptor->started_at_ms;
    host.setAfterRetiredVerificationHookForTesting([&](const std::filesystem::path& verified) {
        retired_path = verified;
        writeDescriptor(directory, verified.filename().string(), replacement);
    });
    host.stop();

    ASSERT_TRUE(std::filesystem::exists(retired_path));
    auto client = didi::runtime::createRuntimeSessionClient(
        std::filesystem::current_path().string(), [] { return std::make_unique<FakeIpcClient>(); });
    const auto listed = client->listSessions(std::nullopt);
    ASSERT_TRUE(listed.isOk());
    ASSERT_TRUE(listed.value()["sessions"].empty());
    ASSERT_TRUE(listed.value()["diagnostics"].empty());

    clearSessionDirectory();
    std::filesystem::remove_all(directory);
}

void test_session_descriptor_rejects_wrong_token_length() {
    const auto session_id = std::string("0123456789abcdef0123456789abcdef");
    auto valid = validDescriptor(session_id, endpointFor(session_id));
    ASSERT_TRUE(didi::runtime::SessionDescriptor::fromJson(valid).isOk());

    auto wrong_token = valid;
    wrong_token["token"] = std::string(63, 'a');
    ASSERT_TRUE(didi::runtime::SessionDescriptor::fromJson(wrong_token).isErr());

    // Break caught: a pipe that merely shares the public prefix can route attach
    // to an endpoint unrelated to the descriptor's claimed process/session.
    auto prefix_trick = valid;
    prefix_trick["endpoint"] = "\\\\.\\pipe\\godot_didi_unrelated";
    ASSERT_TRUE(didi::runtime::SessionDescriptor::fromJson(prefix_trick).isErr());

    auto pid_mismatch = valid;
    pid_mismatch["endpoint"] = endpointFor(session_id, 9999);
    ASSERT_TRUE(didi::runtime::SessionDescriptor::fromJson(pid_mismatch).isErr());

#if !defined(_WIN32)
    auto wrong_socket_parent = valid;
    wrong_socket_parent["endpoint"] =
        (std::filesystem::temp_directory_path() / "wrong-parent" /
         ("godot_didi_" + std::to_string(currentProcessId()) + "_" + session_id + ".sock")).string();
    ASSERT_TRUE(didi::runtime::SessionDescriptor::fromJson(wrong_socket_parent).isErr());
#endif
}

void test_session_attach_keeps_existing_route_when_candidate_handshake_fails() {
    const auto directory = makeSessionDirectory();
    const auto healthy_id = "0123456789abcdef0123456789abcdef";
    const auto bad_id = "fedcba9876543210fedcba9876543210";
    const auto black_hole_id = "77777777777777777777777777777777";
    writeDescriptor(directory, "healthy.json", validDescriptor(healthy_id, endpointFor(healthy_id)));
    writeDescriptor(directory, "bad.json", validDescriptor(bad_id, endpointFor(bad_id)));
    writeDescriptor(directory, "black-hole.json", validDescriptor(black_hole_id, endpointFor(black_hole_id)));

#if defined(_WIN32)
    _putenv_s("DIDI_SESSION_DIR", directory.string().c_str());
#else
    setenv("DIDI_SESSION_DIR", directory.string().c_str(), 1);
#endif
    auto client = didi::runtime::createRuntimeSessionClient(
        std::filesystem::current_path().string(), [] { return std::make_unique<FakeIpcClient>(); });

    ASSERT_TRUE(client->attachSession(healthy_id).isOk());
    ASSERT_TRUE(client->attachSession(bad_id).isErr());
    g_lastHandshakeTimeoutMs = 0;
    ASSERT_TRUE(client->attachSession(black_hole_id).isErr());
    ASSERT_TRUE(g_lastHandshakeTimeoutMs > 0);
    ASSERT_TRUE(g_lastHandshakeTimeoutMs <= 5000);
    ASSERT_TRUE(client->activeSession().has_value());
    ASSERT_EQ(client->activeSession()->session_id, healthy_id);

    auto routed = client->sendRequest("runtime.getTree", {{"root_path", "/root"}});
    ASSERT_TRUE(routed.isOk());
    ASSERT_TRUE(routed.value()["endpoint"].get<std::string>().find(healthy_id) != std::string::npos);
    ASSERT_EQ(routed.value()["token"], std::string(64, 'a'));

#if defined(_WIN32)
    _putenv_s("DIDI_SESSION_DIR", "");
#else
    unsetenv("DIDI_SESSION_DIR");
#endif
    std::filesystem::remove_all(directory);
}

void test_session_attach_rejects_semantically_invalid_handshakes_without_replacing_route() {
    const auto directory = makeSessionDirectory();
    const auto healthy_id = "0123456789abcdef0123456789abcdef";
    const std::vector<std::pair<std::string, std::string>> invalid_sessions = {
        {"11111111111111111111111111111111", "missing-handshake"},
        {"22222222222222222222222222222222", "nonobject-handshake"},
        {"33333333333333333333333333333333", "bad-status"},
        {"44444444444444444444444444444444", "bad-session"},
        {"55555555555555555555555555555555", "bad-protocol"}
    };
    writeDescriptor(directory, "healthy.json", validDescriptor(healthy_id, endpointFor(healthy_id)));
    for (const auto& [session_id, handshake_kind] : invalid_sessions) {
        writeDescriptor(directory, handshake_kind + ".json", validDescriptor(session_id, endpointFor(session_id)));
    }

#if defined(_WIN32)
    _putenv_s("DIDI_SESSION_DIR", directory.string().c_str());
#else
    setenv("DIDI_SESSION_DIR", directory.string().c_str(), 1);
#endif
    auto client = didi::runtime::createRuntimeSessionClient(
        std::filesystem::current_path().string(), [] { return std::make_unique<FakeIpcClient>(); });

    ASSERT_TRUE(client->attachSession(healthy_id).isOk());
    for (const auto& [session_id, handshake_kind] : invalid_sessions) {
        ASSERT_TRUE(client->attachSession(session_id).isErr());
        ASSERT_TRUE(client->activeSession().has_value());
        ASSERT_EQ(client->activeSession()->session_id, healthy_id);
        auto routed = client->sendRequest("runtime.getTree", didi::json::object());
        ASSERT_TRUE(routed.isOk());
        ASSERT_TRUE(routed.value()["endpoint"].get<std::string>().find(healthy_id) != std::string::npos);
    }

#if defined(_WIN32)
    _putenv_s("DIDI_SESSION_DIR", "");
#else
    unsetenv("DIDI_SESSION_DIR");
#endif
    std::filesystem::remove_all(directory);
}

void test_session_discovery_rejects_non_regular_json_entries() {
    const auto directory = makeSessionDirectory();
    std::filesystem::create_directories(directory / "not-a-file.json");
#if defined(_WIN32)
    _putenv_s("DIDI_SESSION_DIR", directory.string().c_str());
#else
    setenv("DIDI_SESSION_DIR", directory.string().c_str(), 1);
#endif
    auto client = didi::runtime::createRuntimeSessionClient(std::filesystem::current_path().string());
    const auto listed = client->listSessions(std::nullopt);
    ASSERT_TRUE(listed.isOk());
    ASSERT_EQ(listed.value()["sessions"].size(), 0u);
    ASSERT_EQ(listed.value()["diagnostics"][0]["error"], "Descriptor must be a regular file");
#if defined(_WIN32)
    _putenv_s("DIDI_SESSION_DIR", "");
#else
    unsetenv("DIDI_SESSION_DIR");
#endif
    std::filesystem::remove_all(directory);
}

void test_session_discovery_does_not_treat_reused_pid_metadata_as_live() {
    // Break caught: a stale descriptor inherits liveness when its PID is reused.
    const auto directory = makeSessionDirectory();
    const auto session_id = std::string("66666666666666666666666666666666");
    auto stale = validDescriptor(session_id, endpointFor(session_id));
    stale["started_at_ms"] = 1;
    writeDescriptor(directory, "stale.json", stale);
    setSessionDirectory(directory);

    auto client = didi::runtime::createRuntimeSessionClient(std::filesystem::current_path().string());
    const auto listed = client->listSessions(std::nullopt);
    ASSERT_TRUE(listed.isOk());
    ASSERT_TRUE(listed.value()["sessions"].empty());
    ASSERT_FALSE(std::filesystem::exists(directory / "stale.json"));

    auto near_mismatch = validDescriptor(session_id, endpointFor(session_id));
    const auto [process_started_at_ms, resolution_ms] = currentProcessStartIdentity();
    near_mismatch["started_at_ms"] = process_started_at_ms + resolution_ms + 1;
    writeDescriptor(directory, "near-mismatch.json", near_mismatch);
    const auto relisted = client->listSessions(std::nullopt);
    ASSERT_TRUE(relisted.isOk());
    ASSERT_TRUE(relisted.value()["sessions"].empty());
    ASSERT_FALSE(std::filesystem::exists(directory / "near-mismatch.json"));

    clearSessionDirectory();
    std::filesystem::remove_all(directory);
}

void test_session_discovery_preserves_replacement_at_proven_stale_path() {
    // Break caught: stale cleanup retires or deletes a different descriptor that replaced the validated object.
    const auto directory = makeSessionDirectory();
    const auto session_id = std::string("abababababababababababababababab");
    auto stale = validDescriptor(session_id, endpointFor(session_id));
    stale["started_at_ms"] = 1;
    const auto descriptor_path = directory / "stale-race.json";
    writeDescriptor(directory, descriptor_path.filename().string(), stale);
    setSessionDirectory(directory);

    auto replacement = validDescriptor(session_id, endpointFor(session_id));
    replacement["token"] = std::string(64, 'b');
    bool swapped = false;
    auto client = didi::runtime::createRuntimeSessionClient(
        std::filesystem::current_path().string(),
        [] { return std::make_unique<FakeIpcClient>(); },
        [&](const std::filesystem::path& opened) {
            if (swapped || opened != descriptor_path) return;
            swapped = true;
            std::filesystem::rename(opened, directory / "validated-stale.retired");
            writeDescriptor(directory, descriptor_path.filename().string(), replacement);
        });

    const auto listed = client->listSessions(std::nullopt);
    ASSERT_TRUE(listed.isOk());
    ASSERT_TRUE(swapped);
    ASSERT_TRUE(listed.value()["sessions"].empty());
    ASSERT_TRUE(std::filesystem::exists(descriptor_path));
    std::ifstream replacement_input(descriptor_path, std::ios::binary);
    ASSERT_EQ(didi::json::parse(replacement_input)["token"], std::string(64, 'b'));
    replacement_input.close();

    clearSessionDirectory();
    std::filesystem::remove_all(directory);
}

#if !defined(_WIN32)
void test_session_registry_prefers_xdg_runtime_directory() {
    // Break caught: default discovery shares a global temporary registry across Unix users.
    const auto runtime_root = makeSessionDirectory();
    clearSessionDirectory();
    setenv("XDG_RUNTIME_DIR", runtime_root.c_str(), 1);
    const auto resolved = didi::runtime::resolveSessionDescriptorDirectory();
    ASSERT_TRUE(resolved.isOk());
    ASSERT_EQ(resolved.value(), runtime_root / "didi-sessions");
    unsetenv("XDG_RUNTIME_DIR");
    std::filesystem::remove_all(runtime_root);
}

void test_session_registry_uid_qualifies_temporary_fallback() {
    // Break caught: the no-XDG fallback is a cross-user /tmp/didi-sessions directory.
    clearSessionDirectory();
    unsetenv("XDG_RUNTIME_DIR");
    const auto resolved = didi::runtime::resolveSessionDescriptorDirectory();
    ASSERT_TRUE(resolved.isOk());
    ASSERT_EQ(resolved.value().filename(), "didi-sessions-" + std::to_string(geteuid()));
}

void test_session_registry_relative_xdg_falls_back_to_uid_temporary() {
    // Break caught: a malformed relative XDG path disables discovery instead of using the secure fallback.
    clearSessionDirectory();
    setenv("XDG_RUNTIME_DIR", "relative/runtime", 1);
    const auto resolved = didi::runtime::resolveSessionDescriptorDirectory();
    unsetenv("XDG_RUNTIME_DIR");
    ASSERT_TRUE(resolved.isOk());
    ASSERT_EQ(resolved.value().filename(), "didi-sessions-" + std::to_string(geteuid()));
    ASSERT_TRUE(resolved.value().is_absolute());
}

void test_posix_retirement_retains_final_path_replacement() {
    // Break caught: fstatat followed by unlinkat can delete a replacement in the final name race.
    const auto directory = makeSessionDirectory();
    setSessionDirectory(directory);
    didi::godot::SessionHost host;
    ASSERT_TRUE(host.prepare("editor", std::filesystem::current_path().string()).isOk());
    const auto descriptor = host.descriptor();
    ASSERT_TRUE(descriptor.has_value());
    ASSERT_TRUE(host.publish().isOk());

    std::filesystem::path tombstone;
    std::filesystem::path verified_original;
    auto replacement = descriptor->toJson(true);
    replacement["token"] = std::string(64, 'b');
    const auto outcome = didi::runtime::retireOwnedSessionDescriptor(
        directory / (descriptor->session_id + ".json"), *descriptor, {}, {},
        [&](const std::filesystem::path& final_path) {
            tombstone = final_path;
            verified_original = directory / "verified-original.retained";
            std::filesystem::rename(final_path, verified_original);
            writeDescriptor(directory, final_path.filename().string(), replacement);
        });

    ASSERT_EQ(outcome, didi::runtime::DescriptorRetirementOutcome::retained_collision_or_race);
    ASSERT_TRUE(std::filesystem::exists(verified_original));
    ASSERT_TRUE(std::filesystem::exists(tombstone));
    std::ifstream replacement_input(tombstone, std::ios::binary);
    ASSERT_EQ(didi::json::parse(replacement_input)["token"], std::string(64, 'b'));
    replacement_input.close();
    host.stop();
    clearSessionDirectory();
    std::filesystem::remove_all(directory);
}

void test_session_discovery_rejects_permissive_registry_directory() {
    // Break caught: discovery trusts a registry directory writable/readable by another user.
    const auto directory = makeSessionDirectory();
    const auto session_id = std::string("cdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcd");
    writeDescriptor(directory, "permissive-dir.json", validDescriptor(session_id, endpointFor(session_id)));
    ASSERT_TRUE(chmod(directory.c_str(), S_IRWXU | S_IRGRP | S_IXGRP) == 0);
    setSessionDirectory(directory);
    auto client = didi::runtime::createRuntimeSessionClient(std::filesystem::current_path().string());
    const auto listed = client->listSessions(std::nullopt);
    ASSERT_TRUE(listed.isOk());
    ASSERT_TRUE(listed.value()["sessions"].empty());
    ASSERT_TRUE(!listed.value()["diagnostics"].empty());
    clearSessionDirectory();
    std::filesystem::remove_all(directory);
}

void test_session_discovery_rejects_permissive_descriptor_file() {
    // Break caught: discovery authenticates a descriptor readable by another user.
    const auto directory = makeSessionDirectory();
    const auto session_id = std::string(32, 'd');
    const auto descriptor_path = directory / "permissive-file.json";
    auto descriptor = validDescriptor(session_id, endpointFor(session_id));
    writeDescriptor(directory, descriptor_path.filename().string(), descriptor);
    ASSERT_TRUE(chmod(descriptor_path.c_str(), S_IRUSR | S_IWUSR | S_IRGRP) == 0);
    setSessionDirectory(directory);
    auto client = didi::runtime::createRuntimeSessionClient(std::filesystem::current_path().string());
    const auto listed = client->listSessions(std::nullopt);
    ASSERT_TRUE(listed.isOk());
    ASSERT_TRUE(listed.value()["sessions"].empty());
    ASSERT_TRUE(!listed.value()["diagnostics"].empty());
    clearSessionDirectory();
    std::filesystem::remove_all(directory);
}
#endif

void test_session_discovery_reads_the_validated_descriptor_object() {
    // Break caught: discovery validates one pathname object, then reopens and trusts a replacement.
    const auto directory = makeSessionDirectory();
    const auto session_id = std::string("0123456789abcdef0123456789abcdef");
    const auto descriptor_path = directory / "swap.json";
    writeDescriptor(directory, descriptor_path.filename().string(),
                    validDescriptor(session_id, endpointFor(session_id)));
    setSessionDirectory(directory);

    bool swapped = false;
    auto client = didi::runtime::createRuntimeSessionClient(
        std::filesystem::current_path().string(),
        [] { return std::make_unique<FakeIpcClient>(); },
        [&](const std::filesystem::path& opened) {
            if (swapped || opened != descriptor_path) return;
            swapped = true;
            std::filesystem::rename(opened, directory / "original.retired");
            auto replacement = validDescriptor(session_id, endpointFor(session_id));
            replacement["token"] = std::string(64, 'b');
            writeDescriptor(directory, descriptor_path.filename().string(), replacement);
        });
    ASSERT_TRUE(client->attachSession(session_id).isOk());
    ASSERT_TRUE(swapped);

    clearSessionDirectory();
    std::filesystem::remove_all(directory);
}

void test_session_discovery_closes_validated_handle_when_hook_throws() {
#if defined(_WIN32) || defined(__linux__)
    // Break caught: exceptions after descriptor validation leaked the native handle/FD.
    const auto directory = makeSessionDirectory();
    const auto session_id = std::string("99999999999999999999999999999999");
    writeDescriptor(directory, "throw.json", validDescriptor(session_id, endpointFor(session_id)));
    setSessionDirectory(directory);
    auto client = didi::runtime::createRuntimeSessionClient(
        std::filesystem::current_path().string(),
        [] { return std::make_unique<FakeIpcClient>(); },
        [](const std::filesystem::path&) { throw std::runtime_error("injected descriptor hook failure"); });

    const auto before = currentOpenHandleCount();
    for (int attempt = 0; attempt < 32; ++attempt) {
        bool threw = false;
        try {
            (void)client->listSessions(std::nullopt);
        } catch (const std::runtime_error&) {
            threw = true;
        }
        ASSERT_TRUE(threw);
    }
    const auto after = currentOpenHandleCount();
    ASSERT_TRUE(after <= before + 2);

    clearSessionDirectory();
    std::filesystem::remove_all(directory);
#endif
}

struct RegisterRuntimeSessionTests {
    RegisterRuntimeSessionTests() {
        registerTest("RuntimeSessions.DescriptorRejectsWrongTokenLength",
                     test_session_descriptor_rejects_wrong_token_length);
        registerTest("RuntimeSessions.FailedAttachRetainsHealthyRoute",
                     test_session_attach_keeps_existing_route_when_candidate_handshake_fails);
        registerTest("RuntimeSessions.InvalidHandshakeRetainsHealthyRoute",
                     test_session_attach_rejects_semantically_invalid_handshakes_without_replacing_route);
        registerTest("RuntimeSessions.RejectsNonRegularDescriptorEntries",
                     test_session_discovery_rejects_non_regular_json_entries);
        registerTest("RuntimeSessions.RejectsReusedPidMetadata",
                     test_session_discovery_does_not_treat_reused_pid_metadata_as_live);
        registerTest("RuntimeSessions.StaleRetirementPreservesReplacement",
                     test_session_discovery_preserves_replacement_at_proven_stale_path);
        registerTest("RuntimeSessions.ReadsValidatedDescriptorObject",
                     test_session_discovery_reads_the_validated_descriptor_object);
        registerTest("RuntimeSessions.ClosesValidatedHandleOnException",
                     test_session_discovery_closes_validated_handle_when_hook_throws);
        registerTest("RuntimeSessions.HostPreparesAndAuthorizesWithoutForwardingToken",
                     test_session_host_prepares_private_unique_descriptor_and_authorizes_without_token_forwarding);
        registerTest("RuntimeSessions.HostPublishesAtomicallyAndRemovesOnlyOwnedDescriptor",
                     test_session_host_publishes_atomically_and_removes_only_its_descriptor);
        registerTest("RuntimeSessions.HostFailsClosedWhenPublicationUnavailable",
                     test_session_host_fails_closed_when_descriptor_publication_becomes_unavailable);
        registerTest("RuntimeSessions.HostStopsServerAndDiscardsDescriptorWhenBindFails",
                     test_session_host_stops_server_and_discards_descriptor_when_bind_fails);
        registerTest("RuntimeSessions.HostStopsBoundServerWhenPublicationFails",
                     test_session_host_stops_bound_server_when_publication_fails);
        registerTest("RuntimeSessions.HostCleanupPreservesSamePathReplacement",
                     test_session_host_cleanup_preserves_same_path_replacement);
        registerTest("RuntimeSessions.HostRetirementCollisionPreservesSelectedDestination",
                     test_session_host_retirement_collision_preserves_selected_destination);
        registerTest("RuntimeSessions.HostRetirementExhaustionNeverOverwritesCollisions",
                     test_session_host_retirement_exhaustion_never_overwrites_collisions);
        registerTest("RuntimeSessions.HostRetainedFileSurvivesReplacementAfterVerification",
                     test_session_host_retained_file_survives_replacement_after_verification);
        registerTest("RuntimeSessions.DiscoveryIgnoresRetainedRetirementFiles",
                     test_runtime_session_discovery_ignores_retained_retirement_files);
#if !defined(_WIN32)
        registerTest("RuntimeSessions.RegistryUsesXdgRuntimeDirectory",
                     test_session_registry_prefers_xdg_runtime_directory);
        registerTest("RuntimeSessions.RegistryFallbackIsUidQualified",
                     test_session_registry_uid_qualifies_temporary_fallback);
        registerTest("RuntimeSessions.RelativeXdgFallsBackToUidTemporary",
                     test_session_registry_relative_xdg_falls_back_to_uid_temporary);
        registerTest("RuntimeSessions.PosixRetirementRetainsFinalPathReplacement",
                     test_posix_retirement_retains_final_path_replacement);
        registerTest("RuntimeSessions.RejectsPermissiveRegistryDirectory",
                     test_session_discovery_rejects_permissive_registry_directory);
        registerTest("RuntimeSessions.RejectsPermissiveDescriptorFile",
                     test_session_discovery_rejects_permissive_descriptor_file);
#endif
    }
} g_registerRuntimeSessionTests;

} // namespace
