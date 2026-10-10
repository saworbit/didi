// The editor's GDScript language server as script_check_syntax's engine (Q11
// in docs/BUILD_QUEUE.md).
//
// Asserted here without a Godot: the framing, the URI spellings Godot uses,
// what a published diagnostic becomes, which hosts a check will connect to,
// how an editor's --lsp-port is read, and the client against a fake server
// that answers the way Godot 4.5.1, 4.6.2 and 4.7.2 were measured to: a server
// owned by another project's editor, a request from the server, silence, and a
// restart between two checks. The live half is tests/run_godot_integration.ps1.

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "didi/runtime/godot_arguments.hpp"
#include "didi/runtime/language_server_client.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#define ASSERT_TRUE(cond) if (!(cond)) throw std::runtime_error("Assertion failed: " #cond);
#define ASSERT_FALSE(cond) ASSERT_TRUE(!(cond))
#define ASSERT_EQ(a, b) ASSERT_TRUE((a) == (b))

void registerTest(const std::string& name, std::function<void()> fn);

namespace {

using didi::json;
using didi::runtime::LanguageServerEndpoint;
using didi::runtime::LanguageServerLimits;
namespace ls = didi::runtime::language_server;
using namespace std::chrono_literals;

#if defined(_WIN32)
using Handle = SOCKET;
constexpr Handle kNone = INVALID_SOCKET;
void closeSocket(Handle socket) { closesocket(socket); }
#else
using Handle = int;
constexpr Handle kNone = -1;
void closeSocket(Handle socket) { ::close(socket); }
#endif

bool readable(Handle socket, int milliseconds) {
    fd_set set;
    FD_ZERO(&set);
    FD_SET(socket, &set);
    timeval timeout{milliseconds / 1000, (milliseconds % 1000) * 1000};
    return select(static_cast<int>(socket) + 1, &set, nullptr, nullptr, &timeout) > 0;
}

// One listening socket on an ephemeral loopback port, serving one client at a
// time on its own thread. `handler` sees every message a client sends.
class FakeServer {
public:
    using Handler = std::function<void(FakeServer&, Handle, const json&)>;

    explicit FakeServer(Handler handler) : m_handler(std::move(handler)) {
#if defined(_WIN32)
        WSADATA data;
        WSAStartup(MAKEWORD(2, 2), &data);
#endif
        m_listener = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        if (::bind(m_listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
            ::listen(m_listener, 4) != 0) {
            throw std::runtime_error("the fake language server could not listen");
        }
#if defined(_WIN32)
        int length = sizeof(address);
#else
        socklen_t length = sizeof(address);
#endif
        getsockname(m_listener, reinterpret_cast<sockaddr*>(&address), &length);
        m_port = ntohs(address.sin_port);
        m_thread = std::thread([this] { serve(); });
    }

    ~FakeServer() {
        m_stop = true;
        m_thread.join();
        closeSocket(m_listener);
#if defined(_WIN32)
        WSACleanup();
#endif
    }

    int port() const { return m_port; }
    int connections() const { return m_connections; }

    void send(Handle client, const json& message) {
        const auto bytes = ls::frame(message);
        size_t sent = 0;
        while (sent < bytes.size()) {
            const auto written = ::send(client, bytes.data() + sent, static_cast<int>(bytes.size() - sent), 0);
            if (written <= 0) return;
            sent += static_cast<size_t>(written);
        }
    }

    // Ends the current client's connection after this message is handled.
    void hangUp() { m_hang_up = true; }

    std::vector<json> received() {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_received;
    }

private:
    void serve() {
        while (!m_stop) {
            if (!readable(m_listener, 20)) continue;
            const Handle client = ::accept(m_listener, nullptr, nullptr);
            if (client == kNone) continue;
            ++m_connections;
            ls::FrameReader reader(1u << 20);
            m_hang_up = false;
            char buffer[4096];
            while (!m_stop && !m_hang_up) {
                if (!readable(client, 20)) continue;
                const auto got = ::recv(client, buffer, static_cast<int>(sizeof(buffer)), 0);
                if (got <= 0) break;
                reader.push(buffer, static_cast<size_t>(got));
                while (auto message = reader.next()) {
                    {
                        std::lock_guard<std::mutex> lock(m_mutex);
                        m_received.push_back(*message);
                    }
                    m_handler(*this, client, *message);
                    if (m_hang_up) break;
                }
            }
            closeSocket(client);
        }
    }

    Handler m_handler;
    Handle m_listener{kNone};
    int m_port{0};
    std::atomic<bool> m_stop{false};
    std::atomic<bool> m_hang_up{false};
    std::atomic<int> m_connections{0};
    std::mutex m_mutex;
    std::vector<json> m_received;
    std::thread m_thread;
};

const std::string kRoot = "C:/Proj Dir/\xC3\xA9t\xC3\xA9";

// Godot publishes with the drive colon encoded: file:///C%3A/...
std::string godotSpelling(std::string uri) {
    const auto colon = uri.find(':', 8);
    if (colon != std::string::npos && colon == 9) uri.replace(colon, 1, "%3A");
    return uri;
}

// Answers the way Godot does: initialize, then diagnostics for a didOpen.
FakeServer::Handler godotLike(json diagnostics, std::string workspace = {}) {
    return [diagnostics, workspace](FakeServer& server, Handle client, const json& message) {
        const auto method = message.value("method", std::string());
        if (method == "initialize") {
            if (!workspace.empty()) {
                server.send(client, {{"jsonrpc", "2.0"},
                                     {"method", "window/showMessage"},
                                     {"params", {{"type", 2}, {"message", "might not work"}}}});
                server.send(client, {{"jsonrpc", "2.0"},
                                     {"method", "gdscript_client/changeWorkspace"},
                                     {"params", {{"path", workspace}}}});
            }
            server.send(client, {{"jsonrpc", "2.0"},
                                 {"id", message["id"]},
                                 {"result", {{"capabilities", json::object()}}}});
        } else if (method == "textDocument/didOpen") {
            const auto uri = message["params"]["textDocument"]["uri"].get<std::string>();
            // Another file's diagnostics first, which the client must skip.
            server.send(client, {{"jsonrpc", "2.0"},
                                 {"method", "textDocument/publishDiagnostics"},
                                 {"params", {{"uri", godotSpelling(uri) + "x"}, {"diagnostics", json::array()}}}});
            server.send(client, {{"jsonrpc", "2.0"},
                                 {"method", "textDocument/publishDiagnostics"},
                                 {"params", {{"uri", godotSpelling(uri)}, {"diagnostics", diagnostics}}}});
        }
    };
}

LanguageServerEndpoint endpointFor(const FakeServer& server) {
    return LanguageServerEndpoint{"127.0.0.1", server.port(), "editor_settings"};
}

LanguageServerLimits quickLimits() {
    LanguageServerLimits limits;
    limits.connect = 1000ms;
    limits.initialize = 2000ms;
    limits.diagnostics = 2000ms;
    return limits;
}

json lspDiagnostic(int line, int character, int severity, const std::string& message) {
    return {{"range", {{"start", {{"line", line}, {"character", character}}},
                       {"end", {{"line", line}, {"character", character + 1}}}}},
            {"severity", severity},
            {"code", -1},
            {"source", "gdscript"},
            {"message", message}};
}

void test_frames_split_and_joined_arrive_whole() {
    const auto first = ls::frame({{"id", 1}});
    const auto second = ls::frame({{"id", 2}, {"text", "\xC3\xA9"}});
    ls::FrameReader reader(1024);
    const auto both = first + second;
    reader.push(both.data(), 5);
    ASSERT_FALSE(reader.next().has_value());
    reader.push(both.data() + 5, both.size() - 5);
    const auto one = reader.next();
    const auto two = reader.next();
    ASSERT_TRUE(one.has_value() && (*one)["id"] == 1);
    ASSERT_TRUE(two.has_value() && (*two)["text"] == "\xC3\xA9");
    ASSERT_FALSE(reader.next().has_value());
    ASSERT_FALSE(reader.failed());

    // Other headers are skipped and the name is not case-sensitive.
    const std::string typed = "content-type: application/vscode-jsonrpc\r\nCONTENT-LENGTH: 2\r\n\r\n{}";
    ls::FrameReader second_reader(1024);
    second_reader.push(typed.data(), typed.size());
    ASSERT_TRUE(second_reader.next().has_value());
}

void test_a_malformed_stream_fails_and_stays_failed() {
    const std::string unframed = "Content-Type: x\r\n\r\n{}";
    ls::FrameReader no_length(1024);
    no_length.push(unframed.data(), unframed.size());
    ASSERT_FALSE(no_length.next().has_value());
    ASSERT_TRUE(no_length.failed());

    const std::string large = "Content-Length: 4096\r\n\r\n";
    ls::FrameReader bounded(1024);
    bounded.push(large.data(), large.size());
    ASSERT_FALSE(bounded.next().has_value());
    ASSERT_TRUE(bounded.failed() && bounded.error().find("larger") != std::string::npos);

    const std::string not_json = "Content-Length: 3\r\n\r\n{{{" + ls::frame({{"id", 1}});
    ls::FrameReader garbage(1024);
    garbage.push(not_json.data(), not_json.size());
    ASSERT_FALSE(garbage.next().has_value());
    ASSERT_TRUE(garbage.failed());
    ASSERT_FALSE(garbage.next().has_value());
}

void test_uris_are_encoded_and_compare_across_spellings() {
    const auto uri = ls::fileUri(kRoot + "/scripts/hud.gd");
    ASSERT_EQ(uri, std::string("file:///C:/Proj%20Dir/%C3%A9t%C3%A9/scripts/hud.gd"));
    ASSERT_EQ(ls::fileUri("/home/me/a b.gd"), std::string("file:///home/me/a%20b.gd"));
    // Godot's own spelling, the colon encoded, is the same file.
    ASSERT_EQ(ls::comparablePath(godotSpelling(uri)), ls::comparablePath(uri));
    ASSERT_EQ(ls::comparablePath(uri), ls::comparablePath(kRoot + "/scripts/hud.gd"));
    ASSERT_EQ(ls::comparablePath("C:\\Proj Dir\\x\\"), ls::comparablePath("C:/Proj Dir/x"));
    ASSERT_FALSE(ls::comparablePath(uri) == ls::comparablePath(uri + "x"));
}

void test_a_published_diagnostic_becomes_a_script_diagnostic() {
    const auto error = ls::diagnosticFromLsp(lspDiagnostic(3, 7, 1, "Identifier \"X\" not declared."));
    ASSERT_TRUE(error.has_value());
    ASSERT_EQ(error->severity, std::string("error"));
    ASSERT_EQ(error->line, 4);
    ASSERT_EQ(error->column, 8);
    ASSERT_EQ(error->rule, std::string("godot_language_server"));

    ASSERT_EQ(ls::diagnosticFromLsp(lspDiagnostic(0, 0, 2, "w"))->severity, std::string("warning"));
    ASSERT_EQ(ls::diagnosticFromLsp(lspDiagnostic(0, 0, 3, "i"))->severity, std::string("info"));
    ASSERT_EQ(ls::diagnosticFromLsp(lspDiagnostic(0, 0, 4, "h"))->severity, std::string("info"));

    // No severity is read as an error, so it cannot pass for a clean script.
    auto unspecified = lspDiagnostic(0, 0, 1, "m");
    unspecified.erase("severity");
    unspecified.erase("range");
    const auto read = ls::diagnosticFromLsp(unspecified);
    ASSERT_TRUE(read.has_value() && read->severity == "error" && read->line == 0 && read->column == 0);

    ASSERT_FALSE(ls::diagnosticFromLsp(lspDiagnostic(0, 0, 1, "")).has_value());
    ASSERT_FALSE(ls::diagnosticFromLsp(json::array()).has_value());

    // Bounded without splitting a character, so the answer still serialises.
    const std::string long_message = std::string(4095, 'a') + "\xC3\xA9" + std::string(10, 'b');
    const auto bounded = ls::diagnosticFromLsp(lspDiagnostic(0, 0, 1, long_message));
    ASSERT_TRUE(bounded.has_value());
    ASSERT_EQ(bounded->message, std::string(4095, 'a') + "...");
    ASSERT_FALSE(json(bounded->message).dump().empty());
}

void test_only_loopback_hosts_are_connected_to() {
    ASSERT_EQ(*ls::loopbackAddressFor("127.0.0.1"), std::string("127.0.0.1"));
    ASSERT_EQ(*ls::loopbackAddressFor(" localhost "), std::string("127.0.0.1"));
    ASSERT_EQ(*ls::loopbackAddressFor("0.0.0.0"), std::string("127.0.0.1"));
    ASSERT_EQ(*ls::loopbackAddressFor("*"), std::string("127.0.0.1"));
    ASSERT_EQ(*ls::loopbackAddressFor(""), std::string("127.0.0.1"));
    ASSERT_EQ(*ls::loopbackAddressFor("127.0.0.2"), std::string("127.0.0.2"));
    ASSERT_EQ(*ls::loopbackAddressFor("::1"), std::string("::1"));
    ASSERT_FALSE(ls::loopbackAddressFor("192.168.1.5").has_value());
    ASSERT_FALSE(ls::loopbackAddressFor("example.com").has_value());
    ASSERT_FALSE(ls::loopbackAddressFor("127.0.0.256").has_value());

    const auto check = didi::runtime::checkScriptWithLanguageServer(
        {"192.168.1.5", 6005, "editor_settings"}, "session", kRoot, "res://a.gd", "", quickLimits());
    ASSERT_FALSE(check.answered);
    ASSERT_TRUE(check.failure.find("not this machine's loopback") != std::string::npos);
}

void test_port_overrides_are_read_as_godot_reads_them() {
    using didi::runtime::protocolPortOverrides;
    const auto both = protocolPortOverrides({"--editor", "--path", "x", "--lsp-port", "6010", "--dap-port", " 6012 "});
    ASSERT_EQ(both.language_server.value_or(-1), 6010);
    ASSERT_EQ(both.debug_adapter.value_or(-1), 6012);
    // Godot's main does not split --key=value, so neither does this.
    ASSERT_FALSE(protocolPortOverrides({"--lsp-port=6010"}).language_server.has_value());
    // The last one wins, as it does in main.
    ASSERT_EQ(protocolPortOverrides({"--lsp-port", "1", "--lsp-port", "2"}).language_server.value_or(-1), 2);
    // After -- or ++ the arguments are the game's.
    ASSERT_FALSE(protocolPortOverrides({"--", "--lsp-port", "6010"}).language_server.has_value());
    ASSERT_FALSE(protocolPortOverrides({"++", "--dap-port", "6010"}).debug_adapter.has_value());
    ASSERT_FALSE(protocolPortOverrides({"--lsp-port"}).language_server.has_value());
    ASSERT_FALSE(protocolPortOverrides({}).language_server.has_value());
}

// A game's frame is one physics tick only at a fixed rate, and nothing the
// engine exposes says whether --fixed-fps was given, so runtime_step reads it
// off the game's own command line the way the ports are read (#1209). The
// marker that starts a scenario's game paused rides after `--`, where the
// engine leaves the game's own arguments (#1208).
void test_fixed_rate_and_start_marker_are_read_from_arguments() {
    using didi::runtime::fixedFpsArgument;
    using didi::runtime::startPausedRequested;
    ASSERT_EQ(fixedFpsArgument({"--headless", "res://main.tscn", "--fixed-fps", "60"}).value_or(-1), 60);
    ASSERT_EQ(fixedFpsArgument({"--fixed-fps", "30", "--fixed-fps", " 120 "}).value_or(-1), 120);
    ASSERT_FALSE(fixedFpsArgument({"--fixed-fps", "0"}).has_value());
    ASSERT_FALSE(fixedFpsArgument({"--fixed-fps", "-1"}).has_value());
    ASSERT_FALSE(fixedFpsArgument({"--fixed-fps"}).has_value());
    ASSERT_FALSE(fixedFpsArgument({"--fixed-fps=60"}).has_value());
    ASSERT_FALSE(fixedFpsArgument({"--", "--fixed-fps", "60"}).has_value());
    ASSERT_FALSE(fixedFpsArgument({}).has_value());

    ASSERT_TRUE(startPausedRequested({"res://main.tscn", "--fixed-fps", "60", "--", "--didi-start-paused"}));
    ASSERT_TRUE(startPausedRequested({"++", "--didi-start-paused"}));
    ASSERT_FALSE(startPausedRequested({"--didi-start-paused"}));
    ASSERT_FALSE(startPausedRequested({"--", "--didi-start-paused=1"}));
    ASSERT_FALSE(startPausedRequested({}));
}

void test_a_check_is_answered_and_its_connection_kept() {
    didi::runtime::closeLanguageServerConnection();
    FakeServer server(godotLike(json::array({lspDiagnostic(3, 7, 1, "Identifier \"NotAThing\" not declared."),
                                             lspDiagnostic(3, 1, 2, "(UNUSED_VARIABLE): unused")})));
    const auto first = didi::runtime::checkScriptWithLanguageServer(
        endpointFor(server), "session-a", kRoot, "res://scripts/typo.gd", "extends Node\n", quickLimits());
    ASSERT_TRUE(first.answered);
    ASSERT_EQ(first.diagnostics.size(), size_t{2});
    ASSERT_EQ(first.diagnostics[0].severity, std::string("error"));
    ASSERT_EQ(first.diagnostics[0].line, 4);
    ASSERT_EQ(first.diagnostics[1].severity, std::string("warning"));

    const auto second = didi::runtime::checkScriptWithLanguageServer(
        endpointFor(server), "session-a", kRoot, "res://scripts/typo.gd", "extends Node\n", quickLimits());
    ASSERT_TRUE(second.answered);
    // One connection and one initialize for both, and each open was closed.
    // The client returns once didClose is sent, so the server may read it a
    // moment later.
    ASSERT_EQ(server.connections(), 1);
    const auto count = [&](const char* method) {
        int found = 0;
        for (const auto& message : server.received()) found += message.value("method", std::string()) == method;
        return found;
    };
    for (int wait = 0; wait < 200 && count("textDocument/didClose") < 2; ++wait) std::this_thread::sleep_for(5ms);
    int initializes = 0, opens = 0, closes = 0;
    for (const auto& message : server.received()) {
        const auto method = message.value("method", std::string());
        initializes += method == "initialize";
        opens += method == "textDocument/didOpen";
        closes += method == "textDocument/didClose";
        if (method == "initialize") {
            ASSERT_EQ(message["params"]["rootUri"].get<std::string>(), ls::fileUri(kRoot));
        }
        if (method == "textDocument/didOpen") {
            ASSERT_EQ(message["params"]["textDocument"]["text"].get<std::string>(), std::string("extends Node\n"));
            ASSERT_EQ(message["params"]["textDocument"]["uri"].get<std::string>(),
                      ls::fileUri(kRoot + "/scripts/typo.gd"));
        }
    }
    ASSERT_EQ(initializes, 1);
    ASSERT_EQ(opens, 2);
    ASSERT_EQ(closes, 2);

    // Another editor session is never answered on this one's connection.
    const auto other = didi::runtime::checkScriptWithLanguageServer(
        endpointFor(server), "session-b", kRoot, "res://scripts/typo.gd", "", quickLimits());
    ASSERT_TRUE(other.answered);
    ASSERT_EQ(server.connections(), 2);
    didi::runtime::closeLanguageServerConnection();
}

void test_another_projects_editor_is_named_and_not_used() {
    didi::runtime::closeLanguageServerConnection();
    FakeServer server(godotLike(json::array(), "D:/elsewhere/other_project"));
    const auto check = didi::runtime::checkScriptWithLanguageServer(
        endpointFor(server), "session", kRoot, "res://a.gd", "", quickLimits());
    ASSERT_FALSE(check.answered);
    ASSERT_EQ(check.other_project, std::string("D:/elsewhere/other_project"));
    ASSERT_TRUE(check.failure.find("another project") != std::string::npos);
    ASSERT_TRUE(check.failure.find("--lsp-port") != std::string::npos);
    for (const auto& message : server.received()) {
        ASSERT_FALSE(message.value("method", std::string()) == "textDocument/didOpen");
    }

    // The same project spelled the server's way is this project, and its
    // spelling is the one the check is sent under.
    didi::runtime::closeLanguageServerConnection();
    FakeServer same(godotLike(json::array(), "c:/proj dir/\xC3\xA9t\xC3\xA9"));
#if defined(_WIN32)
    const auto spelled = didi::runtime::checkScriptWithLanguageServer(
        endpointFor(same), "session", kRoot, "res://a.gd", "", quickLimits());
    ASSERT_TRUE(spelled.answered);
#endif
    didi::runtime::closeLanguageServerConnection();
}

void test_a_request_from_the_server_is_answered_not_applied() {
    didi::runtime::closeLanguageServerConnection();
    auto base = godotLike(json::array());
    FakeServer server([base](FakeServer& self, Handle client, const json& message) {
        if (message.value("method", std::string()) == "textDocument/didOpen") {
            self.send(client, {{"jsonrpc", "2.0"},
                               {"id", 77},
                               {"method", "workspace/applyEdit"},
                               {"params", {{"edit", json::object()}}}});
        }
        base(self, client, message);
    });
    const auto check = didi::runtime::checkScriptWithLanguageServer(
        endpointFor(server), "session", kRoot, "res://a.gd", "", quickLimits());
    ASSERT_TRUE(check.answered);
    bool answered = false;
    for (int wait = 0; wait < 100 && !answered; ++wait) {
        for (const auto& message : server.received()) {
            if (message.contains("id") && message["id"] == 77 && message.contains("result")) {
                ASSERT_EQ(message["result"]["applied"], false);
                answered = true;
            }
        }
        if (!answered) std::this_thread::sleep_for(10ms);
    }
    ASSERT_TRUE(answered);
    didi::runtime::closeLanguageServerConnection();
}

void test_silence_and_absence_are_reasons_not_answers() {
    didi::runtime::closeLanguageServerConnection();
    FakeServer silent([](FakeServer& server, Handle client, const json& message) {
        if (message.value("method", std::string()) == "initialize") {
            server.send(client, {{"jsonrpc", "2.0"}, {"id", message["id"]}, {"result", json::object()}});
        }
    });
    auto limits = quickLimits();
    limits.diagnostics = 200ms;
    const auto started = std::chrono::steady_clock::now();
    const auto quiet = didi::runtime::checkScriptWithLanguageServer(
        endpointFor(silent), "session", kRoot, "res://a.gd", "", limits);
    ASSERT_FALSE(quiet.answered);
    ASSERT_TRUE(quiet.failure.find("published no diagnostics") != std::string::npos);
    ASSERT_TRUE(std::chrono::steady_clock::now() - started < 5s);

    int port = 0;
    {
        FakeServer gone(godotLike(json::array()));
        port = gone.port();
    }
    const auto absent = didi::runtime::checkScriptWithLanguageServer(
        {"127.0.0.1", port, "command_line"}, "session-absent", kRoot, "res://a.gd", "", quickLimits());
    ASSERT_FALSE(absent.answered);
    ASSERT_TRUE(absent.failure.find("Nothing answered") != std::string::npos);
    ASSERT_TRUE(absent.failure.find("--lsp-port") != std::string::npos);
    didi::runtime::closeLanguageServerConnection();
}

void test_a_restarted_server_is_reconnected_to() {
    didi::runtime::closeLanguageServerConnection();
    auto base = godotLike(json::array());
    FakeServer server([base](FakeServer& self, Handle client, const json& message) {
        base(self, client, message);
        // The editor restarts after every check.
        if (message.value("method", std::string()) == "textDocument/didClose") self.hangUp();
    });
    for (int round = 0; round < 3; ++round) {
        const auto check = didi::runtime::checkScriptWithLanguageServer(
            endpointFor(server), "session", kRoot, "res://a.gd", "", quickLimits());
        ASSERT_TRUE(check.answered);
        std::this_thread::sleep_for(50ms);
    }
    ASSERT_EQ(server.connections(), 3);
    didi::runtime::closeLanguageServerConnection();
}

struct RegisterLanguageServerTests {
    RegisterLanguageServerTests() {
        registerTest("ProtocolServers.FixedRateAndStartMarkerFromArguments",
                     test_fixed_rate_and_start_marker_are_read_from_arguments);
        registerTest("LanguageServer.FramesSplitAndJoinedArriveWhole",
                     test_frames_split_and_joined_arrive_whole);
        registerTest("LanguageServer.MalformedStreamFailsAndStaysFailed",
                     test_a_malformed_stream_fails_and_stays_failed);
        registerTest("LanguageServer.UrisEncodedAndCompareAcrossSpellings",
                     test_uris_are_encoded_and_compare_across_spellings);
        registerTest("LanguageServer.PublishedDiagnosticBecomesScriptDiagnostic",
                     test_a_published_diagnostic_becomes_a_script_diagnostic);
        registerTest("LanguageServer.OnlyLoopbackHostsAreConnectedTo",
                     test_only_loopback_hosts_are_connected_to);
        registerTest("LanguageServer.PortOverridesReadAsGodotReadsThem",
                     test_port_overrides_are_read_as_godot_reads_them);
        registerTest("LanguageServer.CheckAnsweredAndConnectionKept",
                     test_a_check_is_answered_and_its_connection_kept);
        registerTest("LanguageServer.AnotherProjectsEditorNamedNotUsed",
                     test_another_projects_editor_is_named_and_not_used);
        registerTest("LanguageServer.ServerRequestAnsweredNotApplied",
                     test_a_request_from_the_server_is_answered_not_applied);
        registerTest("LanguageServer.SilenceAndAbsenceAreReasons",
                     test_silence_and_absence_are_reasons_not_answers);
        registerTest("LanguageServer.RestartedServerReconnected",
                     test_a_restarted_server_is_reconnected_to);
    }
} g_register_language_server_tests;

} // namespace
