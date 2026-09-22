#include "didi/gdextension/gdextension_ipc.hpp"
#include "didi/gdextension/editor_hook.hpp"
#include "didi/gdextension/gdextension_api.hpp"
#include "didi/gdextension/runtime_request_router.hpp"
#include "didi/common/logger.hpp"

namespace didi {
namespace godot {

namespace {

// A bridge failure the person at the editor can actually see.
//
// Every failure below is fatal to every live tool, and every one of them went
// only to this process's stderr and to a log ring that is read over the IPC
// route that just failed to exist. A user launching Godot from a desktop icon
// has neither. The addon's own diagnostics already tell them to "check the
// Output panel for GDEXT_IPC errors", and print_error is what puts one there
// and in the engine's --log-file (#711).
void reportToEditor(const std::string& message) {
    const auto& api = GodotApi::instance();
    if (!api.print_error) return;
    api.print_error(message.c_str(), "GDExtensionIpc::start", __FILE__, __LINE__, 1);
}

} // namespace

GDExtensionIpc& GDExtensionIpc::instance() {
    static GDExtensionIpc s_instance;
    return s_instance;
}

GDExtensionIpc::GDExtensionIpc() {
    // Touched here so EditorHook's function-local static finishes constructing
    // first. Statics are destroyed in reverse order of construction, and this
    // object's destructor calls stop(), which calls EditorHook::instance(). The
    // hook used to be constructed inside start(), which is after this one, so
    // at process exit the destructors ran in the order EditorHook then this,
    // and stop() then locked a mutex and swapped a queue that had already been
    // destroyed. macOS threw std::system_error out of a destructor ("mutex lock
    // failed: Invalid argument") and aborted; Linux corrupted the heap and
    // aborted in malloc. Both landed immediately after the editor plugin said
    // it had deactivated (#688). EditorHook's own constructor touches
    // Logger::instance(), so the whole chain -- Logger, EditorHook, this -- is
    // built in an order whose reverse is safe.
    EditorHook::instance();
    m_server = ipc::createIpcServer();
}

GDExtensionIpc::~GDExtensionIpc() {
    stop();
}

bool GDExtensionIpc::start(const std::string& kind, const std::string& project_path) {
    if (!m_server || m_server->isRunning()) return false;

    // Establish the immutable session classification before lifecycle events are
    // emitted. The logger mirror is already up: the constructor above builds the
    // hook so the destruction order comes out right.
    EditorHook::instance().setSessionKind(kind);

    const auto prepared = m_sessionHost.prepare(kind, project_path,
                                                GodotApi::instance().engineVersionString());
    if (prepared.isErr()) {
        DIDI_LOG_ERROR("GDEXT_IPC", "Unable to prepare runtime session: ", prepared.error().message);
        reportToEditor("Didi: unable to prepare a runtime session. " + prepared.error().message +
                       " Live Didi tools have nothing to reach until this is fixed.");
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

        if (method == ipc::kSessionHandshakeMethod) {
            auto response = handleSessionHandshake(params, *session);
            if (!response.contains("error")) {
                DIDI_LOG_INFO("GDEXT_IPC", "Authenticated runtime session handshake completed");
            }
            return response;
        }

        if (auto rejected = rejectDisallowedSessionMethod(method, *session);
            rejected.has_value()) {
            DIDI_LOG_WARN("GDEXT_IPC", "Rejected method for ", session->kind,
                          " session before main-thread dispatch: ", method);
            return std::move(*rejected);
        }

        DIDI_LOG_INFO("GDEXT_IPC", "Live command started: ", method);

        // Forward to EditorHook to execute safely on Godot's Main Thread
        auto ticket = EditorHook::instance().postCommand(method, params);

        auto response = awaitRuntimeCommand(std::move(ticket), method, *session,
                                            std::chrono::seconds(15));
        if (response.contains("error")) {
            EditorHook::instance().runtimeLogs().append("error", "GDEXT_IPC", "Live command failed",
                                                        {{"method", method}, {"code", response["error"].value("code", 500)}});
        } else {
            DIDI_LOG_INFO("GDEXT_IPC", "Live command completed: ", method);
        }
        return response;
    });

    const auto started = m_sessionHost.startServer(*m_server);
    if (started.isErr()) {
        DIDI_LOG_ERROR("GDEXT_IPC", "Unable to bind and publish runtime session: ", started.error().message);
        reportToEditor("Didi: unable to bind and publish a runtime session. " + started.error().message +
                       " Live Didi tools have nothing to reach until this is fixed.");
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
