#include "didi/gdextension/gdextension_ipc.hpp"
#include "didi/gdextension/editor_hook.hpp"
#include "didi/common/logger.hpp"

namespace didi {
namespace godot {

GDExtensionIpc& GDExtensionIpc::instance() {
    static GDExtensionIpc s_instance;
    return s_instance;
}

GDExtensionIpc::GDExtensionIpc() {
    m_server = ipc::createIpcServer();
}

GDExtensionIpc::~GDExtensionIpc() {
    stop();
}

bool GDExtensionIpc::start(const std::string& kind, const std::string& project_path) {
    if (!m_server || m_server->isRunning()) return false;

    // Establish the logger mirror and immutable session classification before lifecycle events are emitted.
    EditorHook::instance();
    EditorHook::instance().setSessionKind(kind);

    const auto prepared = m_sessionHost.prepare(kind, project_path);
    if (prepared.isErr()) {
        DIDI_LOG_ERROR("GDEXT_IPC", "Unable to prepare runtime session: ", prepared.error().message);
        return false;
    }
    const auto descriptor = m_sessionHost.descriptor();
    if (!descriptor.has_value()) {
        m_sessionHost.stop();
        DIDI_LOG_ERROR("GDEXT_IPC", "Runtime session preparation returned no descriptor");
        return false;
    }

    m_server->setHandler([this](const json& request) -> json {
        const auto authorized = m_sessionHost.authorize(request);
        if (authorized.isErr()) {
            return {{"error", {{"code", authorized.error().code}, {"message", authorized.error().message}}}};
        }
        const auto session = m_sessionHost.descriptor();
        if (!session.has_value()) {
            return {{"error", {{"code", 503}, {"message", "Runtime session host is unavailable"}}}};
        }
        const auto& sanitized = authorized.value();
        std::string method = sanitized.value("method", "");
        json params = sanitized.value("params", json::object());

        if (method == "session.handshake") {
            DIDI_LOG_INFO("GDEXT_IPC", "Authenticated runtime session handshake completed");
            return {{"status", "ok"},
                    {"session_id", session->session_id},
                    {"protocol_version", session->protocol_version},
                    {"kind", session->kind},
                    {"project_path", session->project_path},
                    {"pid", session->pid}};
        }

        DIDI_LOG_INFO("GDEXT_IPC", "Live command started: ", method);

        // Forward to EditorHook to execute safely on Godot's Main Thread
        auto ticket = EditorHook::instance().postCommand(method, params);

        auto status = ticket.response.wait_for(std::chrono::seconds(15));
        if (status == std::future_status::ready) {
            auto response = ticket.response.get();
            if (response.contains("error")) {
                EditorHook::instance().runtimeLogs().append("error", "GDEXT_IPC", "Live command failed",
                                                            {{"method", method}, {"code", response["error"].value("code", 500)}});
            } else {
                DIDI_LOG_INFO("GDEXT_IPC", "Live command completed: ", method);
            }
            if (method == "runtime.getLogs" && !response.contains("error")) {
                response["session"] = session->toJson();
            }
            return response;
        }

        if (ticket.control && ticket.control->tryCancelPending()) {
            if (ticket.response_promise && ticket.control->tryClaimResponse()) {
                ticket.response_promise->set_value(
                    {{"error", {{"code", 504}, {"message", "Main thread command timed out before execution"}}}});
            }
            DIDI_LOG_ERROR("GDEXT_IPC", "Command timed out on main thread: ", method);
            return ticket.response.get();
        }

        DIDI_LOG_WARN("GDEXT_IPC", "Command exceeded timeout after execution started; waiting for definitive result: ", method);
        ticket.response.wait();
        auto response = ticket.response.get();
        if (response.contains("error")) {
            EditorHook::instance().runtimeLogs().append("error", "GDEXT_IPC", "Live command failed",
                                                        {{"method", method}, {"code", response["error"].value("code", 500)}});
        } else {
            DIDI_LOG_INFO("GDEXT_IPC", "Live command completed: ", method);
        }
        if (method == "runtime.getLogs" && !response.contains("error")) {
            response["session"] = session->toJson();
        }
        return response;
    });

    const auto started = m_sessionHost.startServer(*m_server);
    if (started.isErr()) {
        DIDI_LOG_ERROR("GDEXT_IPC", "Unable to bind and publish runtime session: ", started.error().message);
        return false;
    }
    DIDI_LOG_INFO("GDEXT_IPC", "Published authenticated runtime session at ", descriptor->endpoint);
    return true;
}

void GDExtensionIpc::stop() {
    DIDI_LOG_INFO("GDEXT_IPC", "Runtime session shutdown requested");
    EditorHook::instance().cancelPendingCommands("Godot runtime session is shutting down");
    if (m_server && m_server->isRunning()) {
        m_server->stop();
    }
    m_sessionHost.stop();
}

bool GDExtensionIpc::isRunning() const {
    return m_server && m_server->isRunning();
}

} // namespace godot
} // namespace didi
