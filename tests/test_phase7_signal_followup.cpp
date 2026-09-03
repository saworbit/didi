#include "didi/common/ipc_channel.hpp"
#include "didi/runtime/session_client.hpp"
#include "didi/tools/phase7_live_forward.hpp"
#include "didi/tools/resolved_tool_binding.hpp"

#include <filesystem>
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
        // Descriptor validation is platform-specific: acquireRuntimeRouteLease
        // runs SessionDescriptor::fromJson, whose validEndpoint accepts a named
        // pipe on Windows and a .sock path elsewhere. A hardcoded pipe made the
        // lease unobtainable on POSIX, so the forwarder returned early and never
        // dispatched.
#if defined(_WIN32)
        descriptor_.project_path = "C:/phase7-signal-test";
        descriptor_.endpoint = "\\\\.\\pipe\\godot_didi_1_" + descriptor_.session_id;
#else
        descriptor_.project_path = "/tmp/phase7-signal-test";
        descriptor_.endpoint = (std::filesystem::temp_directory_path() /
                                ("godot_didi_1_" + descriptor_.session_id + ".sock")).string();
#endif
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
    ASSERT_EQ(emit_error.at("data").at("outcome"), "unknown_outcome");
    ASSERT_EQ(emit_error.at("data").at("route_quarantine"), true);

    // Every mutating Phase 7 call has the same no-retry contract once the
    // transport confirms dispatch but cannot determine the outcome. Limiting
    // this to signal_emit made tile/grid edits look safely retryable after an
    // ambiguous timeout.
    for (const auto* name : {
             "signal_connect", "signal_disconnect",
             "viewport_set_camera_transform", "viewport_toggle_debug_draw",
             "tilemap_set_cells", "gridmap_set_cells", "anim_play_track",
             "runtime_inject_input"}) {
        auto route = std::make_shared<FailingRoute>(didi::ipc::transportFailure(
            "deadline after dispatch", {true, true, true}));
        const auto error = errorPayload(didi::mcp::sendPhase7LiveRequest(
            didi::mcp::resolveAliasBinding(name), didi::json::object(), route));
        ASSERT_EQ(error.at("code"), 504);
        ASSERT_EQ(error.at("message"), "unknown_outcome");
        ASSERT_EQ(error.at("data").at("retryable"), false);
        ASSERT_EQ(error.at("data").at("outcome"), "unknown_outcome");
        ASSERT_EQ(error.at("data").at("route_quarantine"), true);
    }

    const auto connect = didi::mcp::resolveAliasBinding("signal_connect");
    // A real transport failure, which is what this case was always named for.
    // It previously used a bare Error(502), which carries no transport state and
    // so is indistinguishable from an ordinary engine rejection.
    auto failure = std::make_shared<FailingRoute>(
        didi::ipc::transportFailure("transport failed", {true, false, false}), false);
    const auto connect_error = errorPayload(
        didi::mcp::sendPhase7LiveRequest(connect, didi::json::object(), failure));
    ASSERT_EQ(failure->calls, 1);
    ASSERT_EQ(failure->quarantines, 1);
    ASSERT_EQ(connect_error.at("code"), 503);
    ASSERT_EQ(connect_error.at("message"), "runtime_route_request_failed");
    ASSERT_EQ(connect_error.at("data").at("retryable"), false);
    ASSERT_EQ(connect_error.at("data").at("route_quarantine"), false);
}

TEST_CASE(phase7_application_error_does_not_quarantine_the_route) {
    // Break caught: an ordinary rejection from the engine -- a bad node path, a
    // missing method, a validation failure -- tears down the runtime route, so
    // every later live call in the same session fails with "no atomic runtime
    // route is available for live dispatch". Observed against a live Godot
    // 4.5.1 editor: a signal_connect at request 20 of the integration harness
    // left scene_get_property at request 24 unable to dispatch, and that tool
    // is unrelated and already implemented.
    //
    // Quarantine is for a broken transport, not for an engine that answered.
    const auto connect = didi::mcp::resolveAliasBinding("signal_connect");
    auto rejected = std::make_shared<FailingRoute>(
        didi::Error(422, "invalid_signal_connect_request"));

    const auto error = errorPayload(
        didi::mcp::sendPhase7LiveRequest(connect, didi::json::object(), rejected));

    ASSERT_EQ(rejected->calls, 1);
    ASSERT_EQ(rejected->quarantines, 0);
    ASSERT_EQ(error.at("data").at("route_quarantine"), false);
}

struct RegisterPhase7SignalFollowup {
    RegisterPhase7SignalFollowup() {
        registerTest("Phase7Signals.ApplicationErrorKeepsRoute",
                     [] { phase7_application_error_does_not_quarantine_the_route(); });
        registerTest("Phase7Signals.PostDispatchForwarderContract",
                     [] { phase7_signal_forwarder_post_dispatch_contract(); });
    }
} g_registerPhase7SignalFollowup;

} // namespace
