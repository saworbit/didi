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

bool GDExtensionIpc::start() {
    if (!m_server) return false;

    m_server->setHandler([](const json& request) -> json {
        std::string method = request.value("method", "");
        json params = request.value("params", json::object());

        DIDI_LOG_DEBUG("GDEXT_IPC", "Handling request: ", method);

        // Forward to EditorHook to execute safely on Godot's Main Thread
        auto ticket = EditorHook::instance().postCommand(method, params);

        auto status = ticket.response.wait_for(std::chrono::seconds(15));
        if (status == std::future_status::ready) {
            return ticket.response.get();
        } else {
            ticket.cancelled->store(true);
            DIDI_LOG_ERROR("GDEXT_IPC", "Command timed out on main thread: ", method);
            return {{"error", {{"code", 504}, {"message", "Main thread command execution timed out"}}}};
        }
    });

    return m_server->start(ipc::resolvePipeName());
}

void GDExtensionIpc::stop() {
    if (m_server && m_server->isRunning()) {
        m_server->stop();
    }
}

bool GDExtensionIpc::isRunning() const {
    return m_server && m_server->isRunning();
}

} // namespace godot
} // namespace didi
