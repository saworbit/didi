#include "didi/mcp/tool_registry.hpp"
#include <functional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
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
#include <optional>
#include <string>
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
std::optional<Error> refuseUnusableSignalArguments(const ResolvedToolBinding&, const json&);
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

// Answers scene.getProperty the way the bridge does, so the preview probe has
// something to read.
class SignalProbeClient final : public didi::ipc::IIpcClient {
public:
    bool connect(const std::string&, int) override { return true; }
    void disconnect() override { connected = false; }
    bool isConnected() const override { return connected; }
    didi::Result<didi::json> sendRequest(const std::string& method,
                                         const didi::json&,
                                         int) override {
        ++requests;
        if (method == "scene.getProperty") {
            return didi::json{{"status", "success"}, {"value", "Domain"}};
        }
        return didi::json{{"emitted", true}};
    }

    bool connected{true};
    int requests{0};
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

void test_a_signal_emit_preview_reports_what_it_actually_read() {
    // Break caught: a dry run signs argument values the confirmed call refuses
    // (#616), and reports the node's name as the before state of a planned
    // mutation of a property the call never touches (#621).
    using namespace didi::mcp;

    const auto binding = resolveAliasBinding("signal_emit");
    const auto refusal = [&binding](const didi::json& arguments) {
        return refuseUnusableSignalArguments(
            binding, didi::json{{"target_node", "/root/Domain"},
                                {"signal_name", "renamed"},
                                {"arguments", arguments}});
    };

    // Every rule the confirmed call applies, named, with the bound.
    didi::json deep = 1;
    for (int level = 0; level < 9; ++level) deep = didi::json{{"a", deep}};
    const auto nested = refusal(didi::json::array({deep}));
    ASSERT_TRUE(nested.has_value() && nested->code == 400);
    ASSERT_TRUE(nested->message.find("entry 0") != std::string::npos);
    ASSERT_TRUE(nested->message.find("nested more than 8") != std::string::npos);

    didi::json long_array = didi::json::array();
    for (int index = 0; index < 200; ++index) long_array.push_back(index);
    const auto oversized = refusal(didi::json::array({long_array}));
    ASSERT_TRUE(oversized.has_value() && oversized->code == 400);
    ASSERT_TRUE(oversized->message.find("200 entries") != std::string::npos);
    ASSERT_TRUE(oversized->message.find("limit is 64") != std::string::npos);

    didi::json wide = didi::json::object();
    for (int index = 0; index < 200; ++index) wide[std::to_string(index)] = index;
    const auto keys = refusal(didi::json::array({wide}));
    ASSERT_TRUE(keys.has_value() && keys->message.find("200 keys") != std::string::npos);

    didi::json too_many = didi::json::array();
    for (int index = 0; index < 17; ++index) too_many.push_back(index);
    const auto counted = refusal(too_many);
    ASSERT_TRUE(counted.has_value() && counted->message.find("at most 16") != std::string::npos);

    didi::json big = didi::json::array({std::string(4097, 'x')});
    const auto long_string = refusal(big);
    ASSERT_TRUE(long_string.has_value() && long_string->message.find("4096") != std::string::npos);

    // Arguments the call accepts are not refused, and neither is another tool's.
    ASSERT_TRUE(!refusal(didi::json::array({1, "two", true, nullptr})).has_value());
    ASSERT_TRUE(!refuseUnusableSignalArguments(
        resolveAliasBinding("scene_set_property"),
        didi::json{{"arguments", didi::json::array({deep})}}).has_value());

    // The dry run runs them before it composes anything, so nothing is signed.
    auto client = std::make_shared<SignalProbeClient>();
    auto& registry = ToolRegistry::instance();
    registry.registerAllDefaultTools();
    registry.setIpcClient(client);
    const auto signed_deep = registry.callTool(
        "signal_emit", didi::json{{"target_node", "/root/Domain"},
                                  {"signal_name", "renamed"},
                                  {"arguments", didi::json::array({deep})},
                                  {"dry_run", true}});
    ASSERT_TRUE(signed_deep.isError);
    const auto refused_payload = signalResultPayload(signed_deep);
    ASSERT_TRUE(refused_payload["error"]["code"] == 400);
    ASSERT_TRUE(refused_payload["error"]["message"].get<std::string>().find("entry 0") !=
                std::string::npos);
    ASSERT_TRUE(signed_deep.content.front().text.find("confirmation_token") == std::string::npos);

    // And what it did read is reported as what it was: the node resolved.
    const auto preview = registry.callTool(
        "signal_emit", didi::json{{"target_node", "/root/Domain"},
                                  {"signal_name", "renamed"},
                                  {"arguments", didi::json::array()},
                                  {"dry_run", true}});
    registry.setIpcClient(nullptr);
    ASSERT_TRUE(!preview.isError);
    const auto preview_payload = signalResultPayload(preview);
    const auto& change = preview_payload["mutation_preview"]["changes"][0];
    ASSERT_TRUE(preview_payload["mutation_preview"]["target_read"] == true);
    ASSERT_TRUE(change["kind"] == "resolved_target");
    ASSERT_TRUE(change["before"]["resolved"] == true);
    ASSERT_TRUE(!change["before"].contains("property_name"));
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
        // Everything outside the accepted set is still refused without
        // dispatching. The set itself is spelled out here rather than read from
        // the rule the handler uses, so this stays a second opinion: 2 is
        // CONNECT_PERSIST, and 3, 6 and 7 add CONNECT_DEFERRED and
        // CONNECT_ONE_SHOT, which is what the editor's Connect dialog writes
        // (#852). What those four do is measured in FlagsTheEditorWrites.
        for (int flag = -16; flag <= 16; ++flag) {
            if (flag == 2 || flag == 3 || flag == 6 || flag == 7) continue;
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

void test_phase7_signal_connect_writes_the_flags_the_editor_writes() {
    // Break caught: signal_connect refuses the flag combinations the editor's
    // own Connect dialog writes, so a connection a user authored with Deferred
    // or One Shot ticked can be read through the surface and removed through
    // the surface and not put back, and the one that is put back has quietly
    // stopped being deferred (#852).
    //
    // Measured on 4.5.1, 4.6.2 and 4.7.2: connecting with 2, 3, 6 or 7, packing,
    // saving and loading with CACHE_MODE_IGNORE round-trips the value exactly,
    // and nothing without CONNECT_PERSIST is written to the file at all.
    using namespace didi::mcp;
    const didi::json connect_request = {
        {"emitter_node", "/root/Emitter"}, {"signal_name", "observed"},
        {"target_node", "/root/Receiver"}, {"target_method", "receive"}};
    const auto binding = resolveAliasBinding("signal_connect");

    // What the editor writes, and what reaches the engine.
    for (const auto [asked, forwarded] : std::vector<std::pair<int64_t, int64_t>>{
             {2, 2}, {3, 3}, {6, 6}, {7, 7},
             // CONNECT_INHERITED is the engine's note that a connection came
             // from an instanced scene. signal_list_connections reports it, so
             // a caller reconciling connections hands it straight back; it is
             // provenance rather than a setting, and connect() gets the part
             // the caller chose.
             {34, 2}, {35, 3}, {38, 6}, {39, 7}}) {
        auto client = std::make_shared<SignalRecordingClient>();
        auto request = connect_request;
        request["flags"] = asked;
        const auto result = handleSignalConnect(binding, request, client);
        ASSERT_TRUE(!result.isError);
        ASSERT_TRUE(client->requests == 1);
        ASSERT_TRUE(client->last_params["flags"] == forwarded);
    }

    // Absent still means CONNECT_PERSIST. Every caller written against the old
    // surface keeps working.
    {
        auto client = std::make_shared<SignalRecordingClient>();
        const auto result = handleSignalConnect(binding, connect_request, client);
        ASSERT_TRUE(!result.isError);
        ASSERT_TRUE(client->last_params["flags"] == 2);
    }

    // The refusal names the argument and says what is on offer. It used to be
    // the bare identifier invalid_signal_connect_request, with flags named
    // nowhere, so a caller who passed 3 could not tell whether the problem was
    // the flag, the method, the node or the signal.
    struct RefusalCase {
        int64_t flags;
        const char* names;
    };
    for (const auto& refusal : std::vector<RefusalCase>{
             {0, "CONNECT_PERSIST"},
             {1, "CONNECT_PERSIST"},
             {4, "CONNECT_PERSIST"},
             {10, "CONNECT_REFERENCE_COUNTED"},
             {18, "CONNECT_APPEND_SOURCE_OBJECT"}}) {
        auto client = std::make_shared<SignalRecordingClient>();
        auto request = connect_request;
        request["flags"] = refusal.flags;
        const auto result = handleSignalConnect(binding, request, client);
        ASSERT_TRUE(result.isError);
        ASSERT_TRUE(client->requests == 0);
        const auto message =
            signalResultPayload(result)["error"]["message"].get<std::string>();
        ASSERT_TRUE(message.find("'flags'") != std::string::npos);
        ASSERT_TRUE(message.find(refusal.names) != std::string::npos);
        // Every refusal says what would have been accepted.
        ASSERT_TRUE(message.find("Accepted: 2") != std::string::npos);
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

void test_emitter_node_names_the_emitter_on_every_signal_tool() {
    // Break caught: signal_connect and signal_disconnect call the emitter
    // emitter_node and use target_node for the receiver, while
    // signal_list_connections and signal_emit called the emitter target_node,
    // so the spelling the siblings insist on was refused here (#769).
    using namespace didi::mcp;
    auto& registry = ToolRegistry::instance();
    registry.registerAllDefaultTools();

    // Published on both, and neither requires the old name any more.
    for (const auto* name : {"signal_list_connections", "signal_emit"}) {
        const auto& schema = registry.getTool(name)->inputSchema;
        ASSERT_TRUE(schema["properties"].contains("emitter_node"));
        ASSERT_TRUE(schema["properties"].contains("target_node"));
        const auto required = schema.value("required", didi::json::array());
        for (const auto& entry : required) ASSERT_TRUE(entry != "target_node");
    }

    // The listing forwards the emitter under the name the bridge reads.
    {
        auto client = std::make_shared<SignalRecordingClient>();
        registry.setIpcClient(client);
        const auto result = registry.callTool("signal_list_connections",
                                              didi::json{{"emitter_node", "/root/Emitter"}});
        registry.setIpcClient(nullptr);
        ASSERT_TRUE(!result.isError);
        ASSERT_TRUE(client->requests == 1);
        ASSERT_TRUE(client->last_method == "signal.listConnections");
        ASSERT_TRUE(client->last_params == didi::json({{"target_node", "/root/Emitter"}}));
    }

    // The emit's dry run reads its node through the new name, so the gate and
    // the preview saw the call the handler will get.
    // And the token it mints confirms the same call spelled the same way.
    {
        auto client = std::make_shared<SignalProbeClient>();
        registry.setIpcClient(client);
        const didi::json call = {{"emitter_node", "/root/Domain"}, {"signal_name", "renamed"}};
        auto dry_run = call;
        dry_run["dry_run"] = true;
        const auto preview = registry.callTool("signal_emit", dry_run);
        ASSERT_TRUE(!preview.isError);
        const auto payload = signalResultPayload(preview);
        ASSERT_TRUE(payload["mutation_preview"]["target_read"] == true);
        ASSERT_TRUE(payload["mutation_preview"]["changes"][0]["kind"] == "resolved_target");
        auto confirmed = call;
        confirmed["confirmation_token"] = payload["mutation_preview"]["confirmation_token"];
        const auto emitted = registry.callTool("signal_emit", confirmed);
        registry.setIpcClient(nullptr);
        ASSERT_TRUE(!emitted.isError);
    }

    // Both, or neither, is refused before anything is sent, naming emitter_node.
    for (const auto& [name, arguments] : std::vector<std::pair<const char*, didi::json>>{
             {"signal_list_connections",
              {{"emitter_node", "/root/A"}, {"target_node", "/root/A"}}},
             {"signal_list_connections", didi::json::object()},
             {"signal_emit",
              {{"emitter_node", "/root/A"}, {"target_node", "/root/B"}, {"signal_name", "s"}}},
             {"signal_emit", {{"signal_name", "s"}}}}) {
        auto client = std::make_shared<SignalRecordingClient>();
        registry.setIpcClient(client);
        const auto result = registry.callTool(name, arguments);
        registry.setIpcClient(nullptr);
        ASSERT_TRUE(result.isError);
        ASSERT_TRUE(client->requests == 0);
        const auto payload = signalResultPayload(result);
        ASSERT_TRUE(payload["error"]["code"] == 400);
        ASSERT_TRUE(payload["error"]["message"].get<std::string>().find("emitter_node") !=
                    std::string::npos);
    }

    // The old spelling still works, and a wrong name is still named as wrong.
    {
        auto client = std::make_shared<SignalRecordingClient>();
        registry.setIpcClient(client);
        const auto kept = registry.callTool("signal_list_connections",
                                            didi::json{{"target_node", "/root/Emitter"}});
        const auto wrong = registry.callTool("signal_list_connections",
                                             didi::json{{"node", "/root/Emitter"}});
        registry.setIpcClient(nullptr);
        ASSERT_TRUE(!kept.isError);
        ASSERT_TRUE(client->requests == 1);
        ASSERT_TRUE(wrong.isError);
        ASSERT_TRUE(signalResultPayload(wrong)["error"]["message"].get<std::string>().find(
                        "Unknown argument 'node'") != std::string::npos);
    }
}

struct RegisterPhase7SignalBehavior {
    RegisterPhase7SignalBehavior() {
        registerTest("Phase7Signals.EmitterNodeNamesTheEmitter",
                     test_emitter_node_names_the_emitter_on_every_signal_tool);
        registerTest("Phase7Signals.StrictHandlerValidation",
                     test_phase7_signal_handlers_reject_non_exact_requests_without_dispatch);
        registerTest("Phase7Signals.ExactForwarding",
                     test_phase7_signal_handlers_forward_exact_normalized_requests_once);
        registerTest("Phase7Signals.FlagsTheEditorWrites",
                     test_phase7_signal_connect_writes_the_flags_the_editor_writes);
        registerTest("Phase7Signals.EmitPreviewReportsWhatItRead",
                     test_a_signal_emit_preview_reports_what_it_actually_read);
        registerTest("Phase7Signals.ConfirmationAndPublicGate",
                     test_phase7_signal_emit_confirmation_replay_and_public_gate_do_not_dispatch);
    }
} g_registerPhase7SignalBehavior;

} // namespace
// TASK 2 SIGNAL BEHAVIOR END
