#pragma once

#include "didi/common/ipc_channel.hpp"
#include "didi/gdextension/session_host.hpp"
#include <memory>

namespace didi {
namespace godot {

// The refusal envelope the IPC handler answers with.
//
// Built from the whole Error rather than from the two fields somebody
// remembered. `Error` has three, and `data` is where this project keeps the
// stable identifier a client branches on: see `data.code` throughout
// editor_hook.cpp, and #862 and #865, which were both this shape losing it.
// The handler answers a peer whose token has not been accepted yet, so it is
// the last place that should be dropping structure.
//
// `data` is left out entirely when the Error carries none, rather than written
// as null. A null is not the same as absent to the code that reads this:
// `error.value("data", json::object())` returns the null when the key is
// present, so emitting one would hand every reader a null where it asked for
// an object.
json sessionRefusal(const Error& error);

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
