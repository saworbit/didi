#include "didi/mcp/tool_registry.hpp"
#include <functional>
#include <stdexcept>
#define ASSERT_TRUE(cond) if (!(cond)) throw std::runtime_error("Assertion failed: " #cond);
#define TEST_CASE(name, tags) static void phase7_contract_test()
void registerTest(const std::string& name, std::function<void()> fn);
// Phase 7 partial delivery. The four signal names are live; the remaining
// fourteen stay closed. Asserting both halves in one place is what keeps a
// delivery from silently widening past what was actually trialled.
TEST_CASE("Phase7Signals partial delivery contract", "[phase7][signals][contract]") {
    auto& registry = didi::mcp::ToolRegistry::instance(); registry.registerAllDefaultTools();
    for (const auto* name : {"signal_list_connections", "signal_connect", "signal_disconnect", "signal_emit"}) {
        const auto* tool = registry.getTool(name); ASSERT_TRUE(tool != nullptr);
        ASSERT_TRUE(tool->capability.implemented);
        // Still an error without a live route -- but for want of a session, not
        // for want of an implementation.
        const auto result = registry.callTool(name, didi::json::object()); ASSERT_TRUE(result.isError);
        ASSERT_TRUE(result.content[0].text.find("no trustworthy execution path") == std::string::npos);
    }
    for (const auto* name : {"physics_simulate_step", "nav_bake_mesh",
                             "anim_list_tracks", "anim_play_track",
                             "tilemap_set_cells", "tilemap_get_used_rect", "gridmap_set_cells",
                             "viewport_set_camera_transform", "viewport_toggle_debug_draw",
                             "runtime_get_call_stack"}) {
        const auto* tool = registry.getTool(name); ASSERT_TRUE(tool != nullptr);
        ASSERT_TRUE(!tool->capability.implemented);
        const auto result = registry.callTool(name, didi::json::object()); ASSERT_TRUE(result.isError);
        ASSERT_TRUE(result.content[0].text.find(name) != std::string::npos);
    }
}
struct RegisterPhase7Signals { RegisterPhase7Signals() { registerTest("Phase7Signals partial delivery contract", [] { phase7_contract_test(); }); } } g_registerPhase7Signals;

// TASK 2 SIGNAL BEHAVIOR BEGIN
#include "didi/common/ipc_channel.hpp"
#include "didi/mcp/mutation_safety.hpp"
#include "didi/tools/resolved_tool_binding.hpp"
#include <limits>
#include <memory>
#include <vector>

namespace didi::mcp {
CallToolResult handleSignalListConnections(const ResolvedToolBinding&, const json&,
                                           std::shared_ptr<ipc::IIpcClient>);
CallToolResult handleSignalConnect(const ResolvedToolBinding&, const json&,
                                   std::shared_ptr<ipc::IIpcClient>);
CallToolResult handleSignalDisconnect(const ResolvedToolBinding&, const json&,
                                      std::shared_ptr<ipc::IIpcClient>);
CallToolResult handleSignalEmit(const ResolvedToolBinding&, const json&,
                                std::shared_ptr<ipc::IIpcClient>);
}

namespace {

class SignalRecordingClient final : public didi::ipc::IIpcClient {
public:
    bool connect(const std::string&, int) override { return true; }
    void disconnect() override { connected = false; }
    bool isConnected() const override { return connected; }
    didi::Result<didi::json> sendRequest(const std::string& method,
                                         const didi::json& params,
                                         int timeout_ms) override {
        ++requests;
        last_method = method;
        last_params = params;
        last_timeout_ms = timeout_ms;
        return didi::json{{"native_observed", true}};
    }

    bool connected{true};
    int requests{0};
    int last_timeout_ms{0};
    std::string last_method;
    didi::json last_params;
};

didi::json signalResultPayload(const didi::mcp::CallToolResult& result) {
    ASSERT_TRUE(!result.content.empty());
    return didi::json::parse(result.content.front().text);
}

void assertSignalRequestRejected(const didi::mcp::CallToolResult& result,
                                 const std::shared_ptr<SignalRecordingClient>& client) {
    ASSERT_TRUE(result.isError);
    const auto payload = signalResultPayload(result);
    ASSERT_TRUE(payload["error"]["code"] == 400);
    ASSERT_TRUE(payload["error"]["data"]["retryable"] == false);
    ASSERT_TRUE(client->requests == 0);
}

void test_phase7_signal_handlers_reject_non_exact_requests_without_dispatch() {
    // Break caught: a malformed signal request, unsupported flag, or over-cap emit
    // reaches the live route before the complete Task 2 request is validated.
    using namespace didi::mcp;

    {
        auto client = std::make_shared<SignalRecordingClient>();
        const auto binding = resolveAliasBinding("signal_list_connections");
        for (const auto& invalid : std::vector<didi::json>{
                 didi::json(nullptr), didi::json::array(), didi::json::object(),
                 {{"target_node", ""}}, {{"target_node", std::string(1025, 'n')}},
                 {{"target_node", 7}},
                 {{"target_node", "/root/Emitter"}, {"extra", true}}}) {
            assertSignalRequestRejected(handleSignalListConnections(binding, invalid, client), client);
        }
    }

    const didi::json connect_request = {
        {"emitter_node", "/root/Emitter"}, {"signal_name", "observed"},
        {"target_node", "/root/Receiver"}, {"target_method", "receive"}};
    {
        auto client = std::make_shared<SignalRecordingClient>();
        const auto binding = resolveAliasBinding("signal_connect");
        for (int flag = -16; flag <= 16; ++flag) {
            if (flag == 2) continue;
            auto invalid = connect_request;
            invalid["flags"] = flag;
            assertSignalRequestRejected(handleSignalConnect(binding, invalid, client), client);
        }
        for (const auto flag : {std::numeric_limits<int64_t>::min(),
                                std::numeric_limits<int64_t>::max()}) {
            auto invalid = connect_request;
            invalid["flags"] = flag;
            assertSignalRequestRejected(handleSignalConnect(binding, invalid, client), client);
        }
        for (const auto& flag : std::vector<didi::json>{nullptr, true, "2", 2.0}) {
            auto invalid = connect_request;
            invalid["flags"] = flag;
            assertSignalRequestRejected(handleSignalConnect(binding, invalid, client), client);
        }
        for (const auto* required : {"emitter_node", "signal_name", "target_node", "target_method"}) {
            auto missing = connect_request;
            missing.erase(required);
            assertSignalRequestRejected(handleSignalConnect(binding, missing, client), client);
            auto empty = connect_request;
            empty[required] = "";
            assertSignalRequestRejected(handleSignalConnect(binding, empty, client), client);
        }
        auto long_path = connect_request;
        long_path["emitter_node"] = std::string(1025, 'e');
        assertSignalRequestRejected(handleSignalConnect(binding, long_path, client), client);
        auto long_name = connect_request;
        long_name["target_method"] = std::string(129, 'm');
        assertSignalRequestRejected(handleSignalConnect(binding, long_name, client), client);
        auto extra = connect_request;
        extra["one_shot"] = true;
        assertSignalRequestRejected(handleSignalConnect(binding, extra, client), client);
    }

    {
        auto client = std::make_shared<SignalRecordingClient>();
        const auto binding = resolveAliasBinding("signal_disconnect");
        auto with_flags = connect_request;
        with_flags["flags"] = 2;
        assertSignalRequestRejected(handleSignalDisconnect(binding, with_flags, client), client);
        auto missing = connect_request;
        missing.erase("target_method");
        assertSignalRequestRejected(handleSignalDisconnect(binding, missing, client), client);
    }

    const didi::json emit_request = {
        {"target_node", "/root/Emitter"}, {"signal_name", "observed"}};
    {
        auto client = std::make_shared<SignalRecordingClient>();
        const auto binding = resolveAliasBinding("signal_emit");
        for (const auto& invalid : std::vector<didi::json>{
                 didi::json::object(), {{"target_node", ""}, {"signal_name", "observed"}},
                 {{"target_node", "/root/Emitter"}, {"signal_name", ""}},
                 {{"target_node", "/root/Emitter"}, {"signal_name", "observed"},
                  {"arguments", didi::json::object()}},
                 {{"target_node", "/root/Emitter"}, {"signal_name", "observed"},
                  {"extra", true}}}) {
            assertSignalRequestRejected(handleSignalEmit(binding, invalid, client), client);
        }

        auto too_many_arguments = emit_request;
        too_many_arguments["arguments"] = didi::json::array();
        for (int index = 0; index < 17; ++index) too_many_arguments["arguments"].push_back(index);
        assertSignalRequestRejected(handleSignalEmit(binding, too_many_arguments, client), client);

        didi::json nested = 1;
        for (int depth = 0; depth < 9; ++depth) nested = didi::json::array({nested});
        auto too_deep = emit_request;
        too_deep["arguments"] = didi::json::array({nested});
        assertSignalRequestRejected(handleSignalEmit(binding, too_deep, client), client);

        auto oversized_array = emit_request;
        oversized_array["arguments"] = didi::json::array({didi::json::array()});
        for (int index = 0; index < 65; ++index) oversized_array["arguments"][0].push_back(index);
        assertSignalRequestRejected(handleSignalEmit(binding, oversized_array, client), client);

        didi::json object = didi::json::object();
        for (int index = 0; index < 65; ++index) object["key" + std::to_string(index)] = index;
        auto oversized_object = emit_request;
        oversized_object["arguments"] = didi::json::array({object});
        assertSignalRequestRejected(handleSignalEmit(binding, oversized_object, client), client);

        auto oversized_string = emit_request;
        oversized_string["arguments"] = didi::json::array({std::string(4097, 's')});
        assertSignalRequestRejected(handleSignalEmit(binding, oversized_string, client), client);

        auto oversized_key = emit_request;
        oversized_key["arguments"] = didi::json::array(
            {didi::json{{std::string(4097, 'k'), true}}});
        assertSignalRequestRejected(handleSignalEmit(binding, oversized_key, client), client);

        auto non_finite = emit_request;
        non_finite["arguments"] = didi::json::array(
            {std::numeric_limits<double>::infinity()});
        assertSignalRequestRejected(handleSignalEmit(binding, non_finite, client), client);

        auto oversized_compact_arguments = emit_request;
        oversized_compact_arguments["arguments"] = didi::json::array();
        for (int index = 0; index < 9; ++index) {
            oversized_compact_arguments["arguments"].push_back(std::string(4096, 'b'));
        }
        const auto oversized_result =
            handleSignalEmit(binding, oversized_compact_arguments, client);
        ASSERT_TRUE(oversized_result.isError);
        const auto oversized_payload = signalResultPayload(oversized_result);
        ASSERT_TRUE(oversized_payload["error"]["code"] == 413);
        ASSERT_TRUE(oversized_payload["error"]["data"]["retryable"] == false);
        ASSERT_TRUE(client->requests == 0);
    }
}

void test_phase7_signal_handlers_forward_exact_normalized_requests_once() {
    // Break caught: a signal handler synthesizes success, chooses another IPC method,
    // omits contract defaults, retries, or uses a deadline other than 17 seconds.
    using namespace didi::mcp;
    struct ForwardCase {
        const char* tool;
        const char* method;
        didi::json arguments;
    };
    const std::vector<ForwardCase> cases = {
        {"signal_list_connections", "signal.listConnections",
         {{"target_node", "/root/Emitter"}}},
        {"signal_connect", "signal.connect",
         {{"emitter_node", "/root/Emitter"}, {"signal_name", "observed"},
          {"target_node", "/root/Receiver"}, {"target_method", "receive"}}},
        {"signal_disconnect", "signal.disconnect",
         {{"emitter_node", "/root/Emitter"}, {"signal_name", "observed"},
          {"target_node", "/root/Receiver"}, {"target_method", "receive"}}},
        {"signal_emit", "signal.emit",
         {{"target_node", "/root/Emitter"}, {"signal_name", "observed"}}},
    };

    for (const auto& test : cases) {
        auto client = std::make_shared<SignalRecordingClient>();
        const auto binding = resolveAliasBinding(test.tool, test.arguments);
        CallToolResult result;
        if (std::string_view(test.tool) == "signal_list_connections") {
            result = handleSignalListConnections(binding, test.arguments, client);
        } else if (std::string_view(test.tool) == "signal_connect") {
            result = handleSignalConnect(binding, test.arguments, client);
        } else if (std::string_view(test.tool) == "signal_disconnect") {
            result = handleSignalDisconnect(binding, test.arguments, client);
        } else {
            result = handleSignalEmit(binding, test.arguments, client);
        }
        ASSERT_TRUE(!result.isError);
        ASSERT_TRUE(client->requests == 1);
        ASSERT_TRUE(client->last_method == test.method);
        ASSERT_TRUE(client->last_timeout_ms == 17000);
        if (std::string_view(test.tool) == "signal_connect") {
            ASSERT_TRUE(client->last_params["flags"] == 2);
        }
        if (std::string_view(test.tool) == "signal_emit") {
            ASSERT_TRUE(client->last_params["arguments"] == didi::json::array());
        }
    }
}

void test_phase7_signal_emit_confirmation_replay_and_public_gate_do_not_dispatch() {
    // Break caught: signal.emit bypasses central confirmation, reuses a token,
    // or becomes publicly callable before the atomic Phase 7 activation task.
    using namespace didi::mcp;
    const auto arguments = didi::json{
        {"target_node", "/root/Emitter"}, {"signal_name", "observed"},
        {"arguments", didi::json::array({7})}};
    const auto binding = resolveAliasBinding("signal_emit", arguments);
    ASSERT_TRUE(MutationSafety::isMutation(binding));
    ASSERT_TRUE(MutationSafety::canRequireConfirmation(binding));

    MutationSafety safety([] { return int64_t{1000}; },
                          [] { return std::string(64, 'c'); });
    MutationContext context;
    context.project_root = "C:/project";
    context.execution_mode = "live";
    context.session_id = "0123456789abcdef0123456789abcdef";
    context.route_generation = 9;

    const auto absent = safety.evaluate(binding, arguments, context);
    ASSERT_TRUE(!absent.execute && absent.is_error);
    ASSERT_TRUE(absent.payload["error"]["code"] == 428);

    auto preview_arguments = arguments;
    preview_arguments["dry_run"] = true;
    const auto preview = safety.evaluate(binding, preview_arguments, context);
    ASSERT_TRUE(!preview.execute && !preview.is_error);
    const auto token = preview.payload["mutation_preview"]["confirmation_token"].get<std::string>();
    ASSERT_TRUE(token.size() == 64);

    auto confirmed_arguments = arguments;
    confirmed_arguments["confirmation_token"] = token;
    const auto confirmed = safety.evaluate(binding, confirmed_arguments, context);
    ASSERT_TRUE(confirmed.execute && !confirmed.is_error);
    ASSERT_TRUE(!confirmed.arguments.contains("confirmation_token"));
    const auto replay = safety.evaluate(binding, confirmed_arguments, context);
    ASSERT_TRUE(!replay.execute && replay.is_error);
    ASSERT_TRUE(replay.payload["error"]["code"] == 409);

    // signal_emit is now publicly callable, so what must still stop this call is
    // confirmation, not admission: the token above was consumed by the replay
    // check, and a spent token must never reach the live route.
    auto client = std::make_shared<SignalRecordingClient>();
    auto& registry = ToolRegistry::instance();
    registry.registerAllDefaultTools();
    registry.setIpcClient(client);
    const auto public_result = registry.callTool("signal_emit", confirmed_arguments);
    registry.setIpcClient(nullptr);
    ASSERT_TRUE(public_result.isError);
    ASSERT_TRUE(client->requests == 0);
    const auto public_payload = didi::json::parse(public_result.content.front().text);
    const auto public_code = public_payload["error"]["code"].get<int>();
    ASSERT_TRUE(public_code == 428 || public_code == 409);
    for (const auto* blocker : {"physics_simulate_step", "nav_bake_mesh",
                                "runtime_get_call_stack"}) {
        const auto* tool = registry.getTool(blocker);
        ASSERT_TRUE(tool != nullptr && !tool->capability.implemented);
    }
}

struct RegisterPhase7SignalBehavior {
    RegisterPhase7SignalBehavior() {
        registerTest("Phase7Signals.StrictHandlerValidation",
                     test_phase7_signal_handlers_reject_non_exact_requests_without_dispatch);
        registerTest("Phase7Signals.ExactForwarding",
                     test_phase7_signal_handlers_forward_exact_normalized_requests_once);
        registerTest("Phase7Signals.ConfirmationAndPublicGate",
                     test_phase7_signal_emit_confirmation_replay_and_public_gate_do_not_dispatch);
    }
} g_registerPhase7SignalBehavior;

} // namespace
// TASK 2 SIGNAL BEHAVIOR END
