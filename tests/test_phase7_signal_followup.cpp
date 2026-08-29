#include "didi/common/ipc_channel.hpp"
#include "didi/runtime/session_client.hpp"
#include "didi/tools/phase7_live_forward.hpp"
#include "didi/tools/resolved_tool_binding.hpp"

#include <functional>
#include <memory>
#include <stdexcept>
#include <string>

#define ASSERT_TRUE(cond) if (!(cond)) throw std::runtime_error("Assertion failed: " #cond)
#define ASSERT_EQ(a, b) ASSERT_TRUE((a) == (b))
#define TEST_CASE(name) static void name()
void registerTest(const std::string& name, std::function<void()> fn);

namespace {

class FailingRoute final : public didi::ipc::IIpcClient,
                           public didi::runtime::IRuntimeRouteLeaseProvider,
                           public std::enable_shared_from_this<FailingRoute> {
public:
    explicit FailingRoute(didi::Error failure, bool quarantine_result = true)
        : failure_(std::move(failure)), quarantine_result_(quarantine_result) {
        descriptor_.schema_version = 1;
        descriptor_.session_id = "11111111111111111111111111111111";
        descriptor_.token = std::string(64, '2');
        descriptor_.pid = 1;
        descriptor_.kind = "editor";
        descriptor_.project_path = "C:/phase7-signal-test";
        descriptor_.endpoint = "\\\\.\\pipe\\godot_didi_1_" + descriptor_.session_id;
        descriptor_.started_at_ms = 1;
        descriptor_.protocol_version = "1.3";
    }

    bool connect(const std::string&, int) override { return true; }
    void disconnect() override {}
    bool isConnected() const override { return true; }
    didi::Result<didi::json> sendRequest(const std::string& method,
                                         const didi::json&, int timeout_ms) override {
        ++calls;
        observed_method = method;
        observed_timeout = timeout_ms;
        return failure_;
    }
    std::optional<didi::runtime::RuntimeRouteLease> acquireRouteLease() override {
        return didi::runtime::RuntimeRouteLease{shared_from_this(), descriptor_, 41};
    }
    bool quarantineRoute(const didi::runtime::RuntimeRouteLease& lease) override {
        ++quarantines;
        return lease.generation == 41 && quarantine_result_;
    }

    int calls{0};
    int quarantines{0};
    int observed_timeout{0};
    std::string observed_method;

private:
    didi::Error failure_;
    bool quarantine_result_{true};
    didi::runtime::SessionDescriptor descriptor_;
};

didi::json errorPayload(const didi::mcp::CallToolResult& result) {
    ASSERT_TRUE(result.isError);
    ASSERT_EQ(result.content.size(), 1u);
    return didi::json::parse(result.content.front().text).at("error");
}

TEST_CASE(phase7_signal_forwarder_post_dispatch_contract) {
    const auto emit = didi::mcp::resolveAliasBinding("signal_emit");
    auto deadline = std::make_shared<FailingRoute>(didi::ipc::transportFailure(
        "deadline after dispatch", {true, true, true}));
    const auto emit_error = errorPayload(
        didi::mcp::sendPhase7LiveRequest(emit, didi::json::object(), deadline));
    ASSERT_EQ(deadline->calls, 1);
    ASSERT_EQ(deadline->quarantines, 1);
    ASSERT_EQ(deadline->observed_method, "signal.emit");
    ASSERT_EQ(deadline->observed_timeout, 17000);
    ASSERT_EQ(emit_error.at("code"), 504);
    ASSERT_EQ(emit_error.at("message"), "unknown_outcome");
    ASSERT_EQ(emit_error.at("data").at("retryable"), false);
    ASSERT_EQ(emit_error.at("data").at("outcome"), "unknown");
    ASSERT_EQ(emit_error.at("data").at("route_quarantine"), true);

    const auto connect = didi::mcp::resolveAliasBinding("signal_connect");
    auto failure = std::make_shared<FailingRoute>(didi::Error(502, "transport failed"), false);
    const auto connect_error = errorPayload(
        didi::mcp::sendPhase7LiveRequest(connect, didi::json::object(), failure));
    ASSERT_EQ(failure->calls, 1);
    ASSERT_EQ(failure->quarantines, 1);
    ASSERT_EQ(connect_error.at("code"), 503);
    ASSERT_EQ(connect_error.at("message"), "runtime_route_request_failed");
    ASSERT_EQ(connect_error.at("data").at("retryable"), false);
    ASSERT_EQ(connect_error.at("data").at("route_quarantine"), false);
}

struct RegisterPhase7SignalFollowup {
    RegisterPhase7SignalFollowup() {
        registerTest("Phase7Signals.PostDispatchForwarderContract",
                     [] { phase7_signal_forwarder_post_dispatch_contract(); });
    }
} g_registerPhase7SignalFollowup;

} // namespace
