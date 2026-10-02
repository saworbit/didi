#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "didi/runtime/language_server_client.hpp"
#include "didi/common/project_path.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <mutex>
#include <system_error>

namespace didi::runtime {
namespace language_server {

std::string frame(const json& message) {
    const auto body = message.dump();
    return "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
}

std::optional<json> FrameReader::next() {
    if (failed()) return std::nullopt;
    const auto header_end = m_buffer.find("\r\n\r\n");
    if (header_end == std::string::npos) {
        // A header is a line or two. Anything this long with no blank line is
        // not this protocol, and waiting for more of it would never end.
        if (m_buffer.size() > 64 * 1024) m_error = "a header ran past 64 KiB without ending";
        return std::nullopt;
    }
    std::optional<std::size_t> length;
    std::size_t line_start = 0;
    while (line_start < header_end) {
        auto line_end = m_buffer.find("\r\n", line_start);
        if (line_end == std::string::npos || line_end > header_end) line_end = header_end;
        const auto line = m_buffer.substr(line_start, line_end - line_start);
        line_start = line_end + 2;
        const auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string name = line.substr(0, colon);
        std::transform(name.begin(), name.end(), name.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (name != "content-length") continue;
        std::size_t value = 0;
        bool digits = false;
        for (std::size_t i = colon + 1; i < line.size(); ++i) {
            const unsigned char c = static_cast<unsigned char>(line[i]);
            if (c == ' ' || c == '\t') {
                if (digits) break;
                continue;
            }
            if (!std::isdigit(c)) {
                digits = false;
                break;
            }
            if (value > (SIZE_MAX - 9) / 10) {
                m_error = "a Content-Length overflowed";
                return std::nullopt;
            }
            value = value * 10 + static_cast<std::size_t>(c - '0');
            digits = true;
        }
        if (!digits) {
            m_error = "a Content-Length was not a number";
            return std::nullopt;
        }
        length = value;
    }
    if (!length.has_value()) {
        m_error = "a message arrived with no Content-Length";
        return std::nullopt;
    }
    if (*length > m_max) {
        m_error = "a message of " + std::to_string(*length) + " bytes is larger than the " +
                  std::to_string(m_max) + " this client reads";
        return std::nullopt;
    }
    const auto body_start = header_end + 4;
    if (m_buffer.size() - body_start < *length) return std::nullopt;
    auto message = json::parse(m_buffer.begin() + static_cast<std::ptrdiff_t>(body_start),
                               m_buffer.begin() + static_cast<std::ptrdiff_t>(body_start + *length),
                               nullptr, false);
    m_buffer.erase(0, body_start + *length);
    if (message.is_discarded()) {
        m_error = "a message body was not JSON";
        return std::nullopt;
    }
    return message;
}

namespace {

bool unreservedInUri(unsigned char c) {
    return std::isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~' || c == '/' ||
           c == ':';
}

int hexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string percentDecoded(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '%' && i + 2 < value.size()) {
            const int high = hexValue(value[i + 1]);
            const int low = hexValue(value[i + 2]);
            if (high >= 0 && low >= 0) {
                out.push_back(static_cast<char>(high * 16 + low));
                i += 2;
                continue;
            }
        }
        out.push_back(value[i]);
    }
    return out;
}

// Cut at a byte limit without splitting a UTF-8 sequence, which would make the
// answer unserialisable.
std::string boundedText(const std::string& value, std::size_t limit) {
    if (value.size() <= limit) return value;
    std::size_t cut = limit;
    while (cut > 0 && (static_cast<unsigned char>(value[cut]) & 0xC0) == 0x80) --cut;
    return value.substr(0, cut) + "...";
}

} // namespace

std::string fileUri(const std::string& absolute_path) {
    std::string path = absolute_path;
    std::replace(path.begin(), path.end(), '\\', '/');
    static const char* const kHex = "0123456789ABCDEF";
    std::string encoded;
    encoded.reserve(path.size());
    for (const char raw : path) {
        const auto c = static_cast<unsigned char>(raw);
        if (unreservedInUri(c)) {
            encoded.push_back(raw);
        } else {
            encoded.push_back('%');
            encoded.push_back(kHex[c >> 4]);
            encoded.push_back(kHex[c & 0x0F]);
        }
    }
    return (encoded.rfind('/', 0) == 0 ? "file://" : "file:///") + encoded;
}

std::string comparablePath(const std::string& path_or_uri) {
    std::string value = path_or_uri;
    if (value.size() >= 7) {
        std::string scheme = value.substr(0, 7);
        std::transform(scheme.begin(), scheme.end(), scheme.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (scheme == "file://") value = percentDecoded(value.substr(7));
    }
    std::replace(value.begin(), value.end(), '\\', '/');
    // file:///C:/x decodes to /C:/x.
    if (value.size() >= 3 && value[0] == '/' && std::isalpha(static_cast<unsigned char>(value[1])) &&
        value[2] == ':') {
        value.erase(0, 1);
    }
    while (value.size() > 1 && value.back() == '/') value.pop_back();
#if defined(_WIN32)
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
#endif
    return value;
}

std::optional<offline::ScriptDiagnostic> diagnosticFromLsp(const json& diagnostic) {
    if (!diagnostic.is_object()) return std::nullopt;
    const auto message = diagnostic.find("message");
    if (message == diagnostic.end() || !message->is_string() || message->get<std::string>().empty()) {
        return std::nullopt;
    }
    offline::ScriptDiagnostic out;
    out.message = boundedText(message->get<std::string>(), 4096);
    out.rule = "godot_language_server";
    // The protocol leaves an absent severity to the client. Godot always sends
    // one; reading a missing one as an error keeps a diagnostic from passing
    // for a clean script.
    const auto severity = diagnostic.find("severity");
    const int level = severity != diagnostic.end() && severity->is_number_integer()
                          ? severity->get<int>()
                          : 1;
    out.severity = level == 2 ? "warning" : (level == 3 || level == 4) ? "info" : "error";
    out.line = 0;
    out.column = 0;
    const auto range = diagnostic.find("range");
    if (range != diagnostic.end() && range->is_object()) {
        const auto start = range->find("start");
        if (start != range->end() && start->is_object()) {
            const auto line = start->find("line");
            const auto character = start->find("character");
            // Zero-based in the protocol, one-based in every other answer.
            if (line != start->end() && line->is_number_integer() && line->get<long long>() >= 0 &&
                line->get<long long>() < INT_MAX) {
                out.line = static_cast<int>(line->get<long long>()) + 1;
            }
            if (character != start->end() && character->is_number_integer() &&
                character->get<long long>() >= 0 && character->get<long long>() < INT_MAX) {
                out.column = static_cast<int>(character->get<long long>()) + 1;
            }
        }
    }
    return out;
}

std::optional<std::string> loopbackAddressFor(const std::string& host) {
    std::string value;
    for (const char c : host) {
        if (!std::isspace(static_cast<unsigned char>(c))) {
            value.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
    }
    // An editor told to listen on every interface is listening on loopback too.
    if (value.empty() || value == "localhost" || value == "*" || value == "0.0.0.0") {
        return std::string("127.0.0.1");
    }
    if (value == "::1" || value == "[::1]" || value == "::" || value == "[::]") {
        return std::string("::1");
    }
    unsigned a = 0, b = 0, c = 0, d = 0;
    char tail = 0;
    if (std::sscanf(value.c_str(), "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) == 4 && a == 127 &&
        b <= 255 && c <= 255 && d <= 255) {
        return value;
    }
    return std::nullopt;
}

} // namespace language_server

namespace {

using Clock = std::chrono::steady_clock;

#if defined(_WIN32)
using SocketHandle = SOCKET;
constexpr SocketHandle kNoSocket = INVALID_SOCKET;

bool ensureWinsock() {
    static const bool started = [] {
        WSADATA data;
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    return started;
}

bool wouldBlock() {
    const int error = WSAGetLastError();
    return error == WSAEWOULDBLOCK || error == WSAEINPROGRESS || error == WSAEINTR;
}

void closeHandle(SocketHandle socket) { closesocket(socket); }
#else
using SocketHandle = int;
constexpr SocketHandle kNoSocket = -1;

bool ensureWinsock() { return true; }

bool wouldBlock() { return errno == EWOULDBLOCK || errno == EAGAIN || errno == EINPROGRESS || errno == EINTR; }

void closeHandle(SocketHandle socket) { ::close(socket); }
#endif

class Socket {
public:
    Socket() = default;
    ~Socket() { reset(); }
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    void reset(SocketHandle handle = kNoSocket) {
        if (m_handle != kNoSocket) closeHandle(m_handle);
        m_handle = handle;
    }
    SocketHandle get() const { return m_handle; }
    SocketHandle release() {
        const auto handle = m_handle;
        m_handle = kNoSocket;
        return handle;
    }

private:
    SocketHandle m_handle{kNoSocket};
};

int millisecondsUntil(Clock::time_point deadline) {
    const auto left =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
    return left <= 0 ? 0 : static_cast<int>(std::min<long long>(left, INT_MAX));
}

// 1 ready, 0 time is up, -1 the socket failed.
int waitFor(SocketHandle socket, bool for_write, Clock::time_point deadline) {
#if defined(_WIN32)
    // select rather than WSAPoll, which on Windows before 10 2004 never
    // reports a connect that failed.
    fd_set ready;
    fd_set failed;
    FD_ZERO(&ready);
    FD_ZERO(&failed);
    FD_SET(socket, &ready);
    FD_SET(socket, &failed);
    const int wait_ms = millisecondsUntil(deadline);
    timeval timeout{wait_ms / 1000, (wait_ms % 1000) * 1000};
    const int result = select(0, for_write ? nullptr : &ready, for_write ? &ready : nullptr, &failed,
                              &timeout);
    if (result < 0) return -1;
    if (result == 0) return 0;
    if (FD_ISSET(socket, &failed) && !FD_ISSET(socket, &ready)) return -1;
    return 1;
#else
    pollfd entry{socket, static_cast<short>(for_write ? POLLOUT : POLLIN), 0};
    for (;;) {
        const int result = ::poll(&entry, 1, millisecondsUntil(deadline));
        if (result < 0 && errno == EINTR) continue;
        if (result < 0) return -1;
        if (result == 0) return 0;
        // A closed peer is readable: recv answers 0 and says so.
        if ((entry.revents & (POLLERR | POLLNVAL)) != 0) return -1;
        return 1;
    }
#endif
}

bool setNonBlocking(SocketHandle socket) {
#if defined(_WIN32)
    u_long enabled = 1;
    return ioctlsocket(socket, FIONBIO, &enabled) == 0;
#else
    const int flags = fcntl(socket, F_GETFL, 0);
    return flags >= 0 && fcntl(socket, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

// A connected socket, or why there is none. The handle is not inherited: a
// child Didi starts later (a detached game, a headless check) would otherwise
// hold the connection open after Didi closed it.
std::optional<std::string> connectSocket(const std::string& address, int port,
                                         std::chrono::milliseconds timeout, Socket& out) {
    if (!ensureWinsock()) return std::string("the socket library did not start");
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_NUMERICHOST | AI_NUMERICSERV;
    addrinfo* found = nullptr;
    if (getaddrinfo(address.c_str(), std::to_string(port).c_str(), &hints, &found) != 0 || !found) {
        return std::string("the address could not be read");
    }
    std::unique_ptr<addrinfo, decltype(&freeaddrinfo)> addresses(found, &freeaddrinfo);
    const auto deadline = Clock::now() + timeout;
#if defined(_WIN32)
    SocketHandle handle = WSASocketW(found->ai_family, found->ai_socktype, found->ai_protocol, nullptr,
                                     0, WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
#elif defined(SOCK_CLOEXEC)
    SocketHandle handle = ::socket(found->ai_family, found->ai_socktype | SOCK_CLOEXEC,
                                   found->ai_protocol);
#else
    SocketHandle handle = ::socket(found->ai_family, found->ai_socktype, found->ai_protocol);
    if (handle != kNoSocket) fcntl(handle, F_SETFD, FD_CLOEXEC);
#endif
    if (handle == kNoSocket) return std::string("a socket could not be created");
    Socket socket;
    socket.reset(handle);
#if defined(__APPLE__)
    int no_sigpipe = 1;
    setsockopt(handle, SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe, sizeof(no_sigpipe));
#endif
    if (!setNonBlocking(handle)) return std::string("the socket could not be made non-blocking");
    if (::connect(handle, found->ai_addr, static_cast<int>(found->ai_addrlen)) != 0) {
        if (!wouldBlock()) return std::string("the connection was refused");
        const int ready = waitFor(handle, true, deadline);
        if (ready == 0) return std::string("the connection did not complete in time");
        int error = 0;
#if defined(_WIN32)
        int length = sizeof(error);
        getsockopt(handle, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &length);
#else
        socklen_t length = sizeof(error);
        getsockopt(handle, SOL_SOCKET, SO_ERROR, &error, &length);
#endif
        if (ready < 0 || error != 0) return std::string("the connection was refused");
    }
    int no_delay = 1;
    setsockopt(handle, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&no_delay),
               sizeof(no_delay));
    out.reset(socket.release());
    return std::nullopt;
}

struct Connection {
    explicit Connection(std::size_t max_message_bytes) : reader(max_message_bytes) {}
    Socket socket;
    language_server::FrameReader reader;
    std::string key;
    // The project directory spelled the way the server spells it. Godot builds
    // the URIs it publishes from its own root, so a check is matched against
    // that spelling.
    std::string root;
    long long next_id{1};
};

std::mutex g_connection_mutex;
std::unique_ptr<Connection> g_connection;

bool sendAll(Connection& connection, const std::string& bytes, Clock::time_point deadline) {
    std::size_t sent = 0;
    while (sent < bytes.size()) {
        const auto chunk = static_cast<int>(std::min<std::size_t>(bytes.size() - sent, 1u << 20));
#if defined(_WIN32)
        const int written = ::send(connection.socket.get(), bytes.data() + sent, chunk, 0);
#elif defined(MSG_NOSIGNAL)
        const auto written = ::send(connection.socket.get(), bytes.data() + sent,
                                    static_cast<std::size_t>(chunk), MSG_NOSIGNAL);
#else
        const auto written =
            ::send(connection.socket.get(), bytes.data() + sent, static_cast<std::size_t>(chunk), 0);
#endif
        if (written > 0) {
            sent += static_cast<std::size_t>(written);
            continue;
        }
        if (written < 0 && wouldBlock()) {
            if (waitFor(connection.socket.get(), true, deadline) != 1) return false;
            continue;
        }
        return false;
    }
    return true;
}

bool sendMessage(Connection& connection, const json& message, Clock::time_point deadline) {
    return sendAll(connection, language_server::frame(message), deadline);
}

enum class Read { Message, Timeout, Closed, Failed };

Read readMessage(Connection& connection, Clock::time_point deadline, json& out) {
    char buffer[16384];
    for (;;) {
        if (auto message = connection.reader.next()) {
            out = std::move(*message);
            return Read::Message;
        }
        if (connection.reader.failed()) return Read::Failed;
        const int ready = waitFor(connection.socket.get(), false, deadline);
        if (ready == 0) return Read::Timeout;
        if (ready < 0) return Read::Failed;
        const auto received = ::recv(connection.socket.get(), buffer, static_cast<int>(sizeof(buffer)), 0);
        if (received == 0) return Read::Closed;
        if (received < 0) {
            if (wouldBlock()) continue;
            return Read::Closed;
        }
        connection.reader.push(buffer, static_cast<std::size_t>(received));
    }
}

// A string member, or empty when it is absent or not a string. json::value
// throws on a member of another type, and what answers on the port is not
// trusted to be Godot.
std::string stringField(const json& object, const char* name) {
    if (!object.is_object()) return {};
    const auto found = object.find(name);
    return found != object.end() && found->is_string() ? found->get<std::string>() : std::string();
}

bool isServerRequest(const json& message) {
    return message.is_object() && message.contains("method") && message["method"].is_string() &&
           message.contains("id") && !message["id"].is_null();
}

// A request from the server is answered rather than left hanging. The only one
// Godot sends is workspace/applyEdit, from the Connect dialog, and Didi does not
// edit files on a language server's say-so.
bool answerServerRequest(Connection& connection, const json& message, Clock::time_point deadline) {
    json reply = {{"jsonrpc", "2.0"}, {"id", message["id"]}};
    if (message["method"] == "workspace/applyEdit") {
        reply["result"] = {{"applied", false},
                           {"failureReason", "Didi reads diagnostics and does not apply edits."}};
    } else {
        reply["error"] = {{"code", -32601},
                          {"message", "Didi does not handle " + message["method"].get<std::string>()}};
    }
    return sendMessage(connection, reply, deadline);
}

std::string seconds(std::chrono::milliseconds value) {
    const auto count = value.count();
    if (count % 1000 == 0) return std::to_string(count / 1000) + " s";
    return std::to_string(count) + " ms";
}

bool sameProject(const std::string& reported, const std::string& root) {
    if (language_server::comparablePath(reported) == language_server::comparablePath(root)) return true;
    std::error_code error;
    const bool equivalent = std::filesystem::equivalent(paths::projectPathFromUtf8(reported),
                                                        paths::projectPathFromUtf8(root), error);
    return !error && equivalent;
}

// initialize, then initialized. Nothing when the server is this project's,
// otherwise why not.
std::optional<std::string> handshake(Connection& connection, const std::string& project_root,
                                     const LanguageServerLimits& limits, LanguageServerCheck& check) {
    const auto deadline = Clock::now() + limits.initialize;
    const long long id = connection.next_id++;
    const json initialize = {
        {"jsonrpc", "2.0"},
        {"id", id},
        {"method", "initialize"},
        {"params",
         {{"processId", nullptr},
          {"clientInfo", {{"name", "didi"}}},
          {"rootPath", project_root},
          {"rootUri", language_server::fileUri(project_root)},
          {"capabilities", json::object()}}}};
    if (!sendMessage(connection, initialize, deadline)) {
        return std::string("the connection closed before initialize could be sent");
    }
    std::string other_project;
    for (;;) {
        json message;
        switch (readMessage(connection, deadline, message)) {
        case Read::Timeout:
            return "it did not answer initialize within " + seconds(limits.initialize) +
                   ". The first client an editor sees makes it parse every script in the project, "
                   "and a busy editor answers nothing until its main thread is free";
        case Read::Closed:
            return std::string("it closed the connection during initialize");
        case Read::Failed:
            return "what answered is not a language server: " + connection.reader.error();
        case Read::Message:
            break;
        }
        if (isServerRequest(message)) {
            if (!answerServerRequest(connection, message, deadline)) {
                return std::string("the connection closed during initialize");
            }
            continue;
        }
        if (stringField(message, "method") == "gdscript_client/changeWorkspace") {
            const auto path = message.contains("params") ? stringField(message["params"], "path")
                                                         : std::string();
            if (!path.empty()) {
                if (sameProject(path, project_root)) {
                    connection.root = path;
                } else {
                    other_project = path;
                }
            }
            continue;
        }
        if (message.is_object() && message.contains("id") && message["id"] == id &&
            !message.contains("method")) {
            if (message.contains("error")) {
                const auto reason = stringField(message["error"], "message");
                return "it refused initialize: " +
                       (reason.empty() ? std::string("no reason given") : reason);
            }
            break;
        }
    }
    if (!other_project.empty()) {
        check.other_project = other_project;
        return "it belongs to the editor of another project, " + other_project +
               ". Two editors cannot share one port, so the second one's language server never "
               "started. Give this editor its own port in Editor Settings > Network > Language "
               "Server > Remote Port, or start it with --lsp-port";
    }
    if (!sendMessage(connection, {{"jsonrpc", "2.0"}, {"method", "initialized"}, {"params", json::object()}},
                     deadline)) {
        return std::string("the connection closed after initialize");
    }
    return std::nullopt;
}

// Whatever arrived since the last check, read without waiting. False when the
// connection has gone, which is how an editor that restarted is noticed.
bool drain(Connection& connection) {
    for (;;) {
        json message;
        switch (readMessage(connection, Clock::now(), message)) {
        case Read::Timeout:
            return true;
        case Read::Closed:
        case Read::Failed:
            return false;
        case Read::Message:
            break;
        }
        if (isServerRequest(message) &&
            !answerServerRequest(connection, message, Clock::now() + std::chrono::seconds(1))) {
            return false;
        }
    }
}

std::string portOrigin(const LanguageServerEndpoint& endpoint) {
    return endpoint.port_source == "command_line" ? "the editor was started with (--lsp-port)"
                                                  : "the editor's settings name";
}

} // namespace

void closeLanguageServerConnection() {
    std::lock_guard<std::mutex> lock(g_connection_mutex);
    g_connection.reset();
}

LanguageServerCheck checkScriptWithLanguageServer(const LanguageServerEndpoint& endpoint,
                                                  const std::string& connection_key,
                                                  const std::string& project_root,
                                                  const std::string& res_path,
                                                  const std::string& source_text,
                                                  const LanguageServerLimits& limits) {
    LanguageServerCheck check;
    const auto started = Clock::now();
    const auto finish = [&](std::string failure) {
        check.failure = std::move(failure);
        check.seconds = std::chrono::duration<double>(Clock::now() - started).count();
        return check;
    };

    const auto address = language_server::loopbackAddressFor(endpoint.host);
    if (!address.has_value()) {
        return finish("The editor's language server listens on " + endpoint.host +
                      ", which is not this machine's loopback address, and Didi does not open a "
                      "connection off this machine to read a project's scripts.");
    }
    if (endpoint.port <= 0 || endpoint.port > 65535) {
        return finish("The editor reported " + std::to_string(endpoint.port) +
                      " as its language server's port, which is not a port.");
    }
    const std::string where = *address + ":" + std::to_string(endpoint.port);
    std::string relative = res_path;
    if (relative.rfind("res://", 0) == 0) relative.erase(0, 6);
    std::string root = project_root;
    std::replace(root.begin(), root.end(), '\\', '/');
    while (root.size() > 1 && root.back() == '/') root.pop_back();
    const std::string key =
        connection_key + "|" + where + "|" + language_server::comparablePath(root);

    std::lock_guard<std::mutex> lock(g_connection_mutex);
    for (int attempt = 0; attempt < 2; ++attempt) {
        const bool reused = g_connection && g_connection->key == key;
        if (reused) {
            if (!drain(*g_connection)) {
                // The editor went away or restarted since the last check.
                g_connection.reset();
                continue;
            }
        } else {
            g_connection.reset();
            auto connection = std::make_unique<Connection>(limits.max_message_bytes);
            if (const auto refused = connectSocket(*address, endpoint.port, limits.connect,
                                                   connection->socket)) {
                return finish("Nothing answered on " + where + ", the port " + portOrigin(endpoint) +
                              " for its GDScript language server: " + *refused + ".");
            }
            connection->key = key;
            connection->root = root;
            if (const auto refused = handshake(*connection, root, limits, check)) {
                return finish("The GDScript language server on " + where + " was not used: " +
                              *refused + ".");
            }
            g_connection = std::move(connection);
        }

        auto& connection = *g_connection;
        const auto deadline = Clock::now() + limits.diagnostics;
        const auto uri = language_server::fileUri(connection.root + "/" + relative);
        const auto wanted = language_server::comparablePath(uri);
        const json open = {{"jsonrpc", "2.0"},
                           {"method", "textDocument/didOpen"},
                           {"params",
                            {{"textDocument",
                              {{"uri", uri}, {"languageId", "gdscript"}, {"version", 1},
                               {"text", source_text}}}}}};
        if (!sendMessage(connection, open, deadline)) {
            g_connection.reset();
            if (reused && attempt == 0) continue;
            return finish("The GDScript language server on " + where +
                          " closed the connection before the script could be sent.");
        }
        std::optional<json> published;
        bool closed = false;
        while (!published) {
            json message;
            const auto read = readMessage(connection, deadline, message);
            if (read == Read::Timeout) {
                g_connection.reset();
                return finish("The GDScript language server on " + where +
                              " published no diagnostics for " + res_path + " within " +
                              seconds(limits.diagnostics) + ".");
            }
            if (read == Read::Failed) {
                const auto reason = connection.reader.error();
                g_connection.reset();
                return finish("The GDScript language server on " + where + " sent " +
                              (reason.empty() ? std::string("something unreadable") : reason) + ".");
            }
            if (read == Read::Closed) {
                closed = true;
                break;
            }
            if (isServerRequest(message)) {
                if (!answerServerRequest(connection, message, deadline)) {
                    closed = true;
                    break;
                }
                continue;
            }
            if (stringField(message, "method") != "textDocument/publishDiagnostics" ||
                !message.contains("params")) {
                continue;
            }
            const auto& params = message["params"];
            const auto published_uri = stringField(params, "uri");
            if (published_uri.empty() || language_server::comparablePath(published_uri) != wanted) {
                continue;
            }
            published = params;
        }
        if (closed) {
            g_connection.reset();
            if (reused && attempt == 0) continue;
            return finish("The GDScript language server on " + where +
                          " closed the connection before it published diagnostics for " +
                          res_path + ".");
        }
        // Godot 4.6 and later refuse a second open of a file a client holds.
        if (!sendMessage(connection,
                         {{"jsonrpc", "2.0"},
                          {"method", "textDocument/didClose"},
                          {"params", {{"textDocument", {{"uri", uri}}}}}},
                         deadline)) {
            g_connection.reset();
        }
        const auto diagnostics = published->find("diagnostics");
        if (diagnostics != published->end() && diagnostics->is_array()) {
            for (const auto& item : *diagnostics) {
                auto diagnostic = language_server::diagnosticFromLsp(item);
                if (!diagnostic) continue;
                if (check.diagnostics.size() >= limits.max_diagnostics) {
                    check.truncated = true;
                    break;
                }
                check.diagnostics.push_back(std::move(*diagnostic));
            }
        }
        check.answered = true;
        return finish({});
    }
    return finish("The GDScript language server on " + where +
                  " closed the connection twice in a row.");
}

} // namespace didi::runtime
