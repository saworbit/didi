#pragma once

#include "didi/common/ipc_channel.hpp"
#include "didi/gdextension/session_host.hpp"
#include <memory>

namespace didi {
namespace godot {

class GDExtensionIpc {
public:
    static GDExtensionIpc& instance();

    bool start(const std::string& kind, const std::string& project_path);
    void stop();
    bool isRunning() const;

private:
    GDExtensionIpc();
    ~GDExtensionIpc();

    std::unique_ptr<ipc::IIpcServer> m_server;
    SessionHost m_sessionHost;
};

} // namespace godot
} // namespace didi
