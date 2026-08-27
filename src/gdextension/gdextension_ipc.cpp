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
            return {{"status", "ok"},
                    {"session_id", session->session_id},
                    {"protocol_version", session->protocol_version},
                    {"kind", session->kind},
                    {"project_path", session->project_path},
                    {"pid", session->pid}};
        }

        DIDI_LOG_DEBUG("GDEXT_IPC", "Handling request: ", method);

        // Forward to EditorHook to execute safely on Godot's Main Thread
        auto ticket = EditorHook::instance().postCommand(method, params);

        auto status = ticket.response.wait_for(std::chrono::seconds(15));
        if (status == std::future_status::ready) {
            return ticket.response.get();
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
        return ticket.response.get();
    });

    if (!m_server->start(descriptor->endpoint)) {
        m_sessionHost.stop();
        DIDI_LOG_ERROR("GDEXT_IPC", "Unable to bind runtime endpoint");
        return false;
    }
    const auto published = m_sessionHost.publish();
    if (published.isErr()) {
        DIDI_LOG_ERROR("GDEXT_IPC", "Unable to publish runtime session: ", published.error().message);
        m_server->stop();
        m_sessionHost.stop();
        return false;
    }
    DIDI_LOG_INFO("GDEXT_IPC", "Published authenticated runtime session at ", descriptor->endpoint);
    return true;
}

void GDExtensionIpc::stop() {
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
