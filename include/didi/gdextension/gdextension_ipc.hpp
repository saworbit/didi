#pragma once

#include "didi/common/ipc_channel.hpp"
#include <memory>

namespace didi {
namespace godot {

class GDExtensionIpc {
public:
    static GDExtensionIpc& instance();

    bool start();
    void stop();
    bool isRunning() const;

private:
    GDExtensionIpc();
    ~GDExtensionIpc();

    std::unique_ptr<ipc::IIpcServer> m_server;
};

} // namespace godot
} // namespace didi
