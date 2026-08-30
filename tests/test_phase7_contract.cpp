#include "didi/mcp/phase7_schemas.hpp"
#include "didi/mcp/tool_registry.hpp"
#include "didi/runtime/session_client.hpp"
#include "didi/tools/phase7_live_forward.hpp"
#include "didi/tools/resolved_tool_binding.hpp"

#include <array>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <functional>
#include <stdexcept>
#include <string>
#include <unordered_set>

#define ASSERT_TRUE(cond) if (!(cond)) throw std::runtime_error("Assertion failed: " #cond);
#define ASSERT_EQ(a, b) ASSERT_TRUE((a) == (b))

void registerTest(const std::string& name, std::function<void()> fn);

namespace {

class RecordingClient final : public didi::ipc::IIpcClient {
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
        return response;
    }
    bool connected{true};
    int requests{0};
    int last_timeout_ms{0};
    std::string last_method;
    didi::json last_params;
    didi::json response{{"status", "ok"}, {"observed", true}};
};

class LeaseAwareClient final : public didi::ipc::IIpcClient,
                               public didi::runtime::IRuntimeRouteLeaseProvider {
public:
    bool connect(const std::string&, int) override { return true; }
    void disconnect() override { connected = false; }
    bool isConnected() const override { return connected; }
    didi::Result<didi::json> sendRequest(const std::string& method,
                                         const didi::json& params,
                                         int timeout_ms) override {
        ++wrapper_requests;
        last_method = method;
        last_params = params;
        last_timeout_ms = timeout_ms;
        return wrapper_response;
    }
    std::optional<didi::runtime::RuntimeRouteLease> acquireRouteLease() override {
        ++lease_acquisitions;
        const std::string session_id = "0123456789abcdef0123456789abcdef";
        didi::runtime::SessionDescriptor descriptor{
            1, session_id, std::string(64, 'a'), 77, "editor", "C:/project",
#if defined(_WIN32)
            "\\\\.\\pipe\\godot_didi_77_" + session_id,
#else
            (std::filesystem::temp_directory_path() /
             ("godot_didi_77_" + session_id + ".sock")).string(),
#endif
            123456789, "1.3"};
        return didi::runtime::RuntimeRouteLease{raw_client, descriptor, generation};
    }
    bool quarantineRoute(const didi::runtime::RuntimeRouteLease& lease) override {
        ++quarantines;
        quarantined_generation = lease.generation;
        return lease.generation == generation;
    }

    bool connected{true};
    uint64_t generation{73};
    uint64_t quarantined_generation{0};
    int lease_acquisitions{0};
    int wrapper_requests{0};
    int quarantines{0};
    int last_timeout_ms{0};
    std::string last_method;
    didi::json last_params;
    didi::json wrapper_response{{"error", {{"code", 409},
                                             {"message", "session_kind_rejected"}}}};
    std::shared_ptr<RecordingClient> raw_client{std::make_shared<RecordingClient>()};
};

class MissingLeaseClient final : public didi::ipc::IIpcClient,
                                 public didi::runtime::IRuntimeRouteLeaseProvider {
public:
    bool connect(const std::string&, int) override { return true; }
    void disconnect() override {}
    bool isConnected() const override { return true; }
    didi::Result<didi::json> sendRequest(const std::string&, const didi::json&, int) override {
        ++requests;
        return didi::json{{"status", "ok"}};
    }
    std::optional<didi::runtime::RuntimeRouteLease> acquireRouteLease() override {
        ++lease_acquisitions;
        return std::nullopt;
    }
    bool quarantineRoute(const didi::runtime::RuntimeRouteLease&) override { return false; }

    int requests{0};
    int lease_acquisitions{0};
};

struct ScopedEmptyCwd {
    ScopedEmptyCwd()
        : original(std::filesystem::current_path()),
          empty(std::filesystem::temp_directory_path() /
                ("didi-phase7-empty-" + std::to_string(
                    std::chrono::steady_clock::now().time_since_epoch().count()))) {
        std::filesystem::create_directories(empty);
        std::filesystem::current_path(empty);
    }
    ~ScopedEmptyCwd() {
        std::error_code error;
        std::filesystem::current_path(original, error);
        std::filesystem::remove_all(empty, error);
    }
    std::filesystem::path original;
    std::filesystem::path empty;
};

void assertBinding(const didi::mcp::ResolvedToolBinding& binding,
                   std::string_view invoked, std::string_view canonical,
                   std::string_view schema, std::string_view capability,
                   std::string_view policy, std::string_view handler,
                   std::string_view method,
                   didi::runtime::SessionKindPolicy session_policy) {
    ASSERT_EQ(binding.invoked_name, invoked);
    ASSERT_EQ(binding.canonical_name, canonical);
    ASSERT_EQ(binding.schema_source, schema);
    ASSERT_EQ(binding.capability_source, capability);
    ASSERT_EQ(binding.policy_source, policy);
    ASSERT_EQ(binding.handler_id, handler);
    ASSERT_EQ(binding.ipc_method, method);
    ASSERT_EQ(binding.session_policy, session_policy);
}

void test_phase7_all_ten_alias_bindings_are_exact() {
    using Policy = didi::runtime::SessionKindPolicy;
    struct Row { const char* invoked; const char* canonical; const char* method; Policy policy; };
    const std::array<Row, 8> direct = {{
        {"get_scene_hierarchy", "scene_get_hierarchy", "scene.getHierarchy", Policy::editor_only},
        {"capture_viewport", "viewport_capture_frame", "vision.captureViewport", Policy::editor_only},
        {"analyze_script_diagnostics", "script_check_syntax", "script.checkSyntax", Policy::editor_only},
        {"patch_script_symbols", "script_patch_method", "script.patchMethod", Policy::editor_only},
        {"create_visual_test_lab", "viewport_create_test_lab", "", Policy::editor_only},
        {"query_project_resources", "project_list_resources", "", Policy::editor_only},
        {"execute_test_session", "runtime_launch", "", Policy::editor_only},
        {"inject_input_event", "runtime_inject_input", "runtime.injectInput", Policy::game_only},
    }};
    const std::unordered_set<std::string_view> blockers = {
        "physics_simulate_step", "nav_bake_mesh", "runtime_get_call_stack"};
    for (const auto& row : direct) {
        const auto binding = didi::mcp::resolveAliasBinding(row.invoked, didi::json::object());
        assertBinding(binding, row.invoked, row.canonical, row.canonical, row.canonical,
                      row.canonical, row.canonical, row.method, row.policy);
        ASSERT_TRUE(blockers.count(binding.canonical_name) == 0);
    }

    const std::array<std::pair<const char*, const char*>, 5> actions = {{
        {"instantiate", "scene_instantiate_node"}, {"remove", "scene_remove_node"},
        {"reparent", "scene_reparent_node"}, {"set_property", "scene_set_property"},
        {"duplicate", "scene_duplicate_node"},
    }};
    for (const auto& [action, canonical] : actions) {
        const auto binding = didi::mcp::resolveAliasBinding(
            "mutate_scene_tree", {{"action", action}});
        ASSERT_EQ(binding.invoked_name, "mutate_scene_tree");
        ASSERT_EQ(binding.canonical_name, canonical);
        ASSERT_EQ(binding.schema_source, "mutate_scene_tree");
        ASSERT_EQ(binding.capability_source, "mutate_scene_tree");
        ASSERT_EQ(binding.policy_source, canonical);
        ASSERT_EQ(binding.handler_id, "mutate_scene_tree");
        ASSERT_EQ(binding.session_policy, Policy::editor_only);
        ASSERT_TRUE(blockers.count(binding.canonical_name) == 0);
    }

    const auto instantiate = didi::mcp::resolveAliasBinding(
        "instantiate_asset", didi::json::object());
    assertBinding(instantiate, "instantiate_asset", "instantiate_asset",
                  "instantiate_asset", "instantiate_asset", "instantiate_asset",
                  "instantiate_asset", "asset.instantiate", Policy::editor_only);
}

void test_phase7_generated_schemas() {
    ScopedEmptyCwd cwd;
    const auto names = didi::mcp::phase7::canonicalNames();
    ASSERT_EQ(names.size(), 18u);
    ASSERT_TRUE(std::is_sorted(names.begin(), names.end()));
    for (const auto name : names) {
        const auto& schema = didi::mcp::phase7::standaloneRequestSchema(name);
        ASSERT_EQ(schema["$schema"], "https://json-schema.org/draft/2020-12/schema");
        ASSERT_EQ(schema["$id"], "https://didi.local/schemas/phase7/generated/" +
                                  std::string(name) + ".request.schema.json");
        ASSERT_EQ(schema["type"], "object");
        ASSERT_TRUE(!schema.contains("$ref"));
    }
    bool rejected_unknown = false;
    try {
        (void)didi::mcp::phase7::standaloneRequestSchema("not_registered");
    } catch (const std::logic_error&) {
        rejected_unknown = true;
    }
    ASSERT_TRUE(rejected_unknown);

    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();
    for (const auto name : names) {
        const auto* tool = registry.getTool(std::string(name));
        ASSERT_TRUE(tool != nullptr);
        ASSERT_EQ(tool->inputSchema["$id"],
                  "https://didi.local/schemas/phase7/generated/" +
                      std::string(name) + ".request.schema.json");
        ASSERT_TRUE(static_cast<bool>(tool->boundHandler));
        ASSERT_TRUE(!tool->handler);
    }
    const auto* alias = registry.getTool("inject_input_event");
    ASSERT_TRUE(alias != nullptr);
    ASSERT_TRUE(static_cast<bool>(alias->boundHandler));
    ASSERT_TRUE(!alias->handler);
}

void test_phase7_live_forwarding_uses_exact_method_and_deadline() {
    auto client = std::make_shared<RecordingClient>();
    const didi::json arguments =
        {{"events", didi::json::array({{{"type", "action"},
                                         {"action_name", "jump"}, {"pressed", true}}})}};
    const auto binding = didi::mcp::resolveAliasBinding("inject_input_event", arguments);
    const auto result = didi::mcp::sendPhase7LiveRequest(binding, arguments, client);
    ASSERT_TRUE(!result.isError);
    ASSERT_EQ(client->requests, 1);
    ASSERT_EQ(client->last_method, "runtime.injectInput");
    ASSERT_EQ(client->last_timeout_ms, 17000);
    ASSERT_EQ(client->last_params["events"][0]["action_name"], "jump");
}

void test_phase7_live_forwarding_preserves_bound_dispatch_and_error_identity() {
    auto client = std::make_shared<LeaseAwareClient>();
    client->raw_client->response = client->wrapper_response;
    const didi::json arguments = {{"events", didi::json::array()}};
    const auto binding = didi::mcp::resolveAliasBinding("inject_input_event", arguments);
    const auto result = didi::mcp::sendPhase7LiveRequest(binding, arguments, client);

    ASSERT_EQ(client->lease_acquisitions, 1);
    ASSERT_EQ(client->wrapper_requests, 0);
    ASSERT_EQ(client->raw_client->requests, 1);
    ASSERT_EQ(client->raw_client->last_method, "runtime.injectInput");
    ASSERT_EQ(client->raw_client->last_timeout_ms, 17000);
    ASSERT_TRUE(result.isError);
    const auto payload = didi::json::parse(result.content.at(0).text);
    ASSERT_EQ(payload["error"]["code"], 409);
    ASSERT_EQ(payload["error"]["data"]["tool"], "inject_input_event");
    ASSERT_EQ(payload["error"]["data"]["retryable"], false);
}

// Break caught: the forwarder sends through the lease's raw client instead of
// the lease, so the session token is never attached and the extension answers
// 401 to every Phase 7 request. Nothing caught this for as long as no Phase 7
// tool was implemented -- the fakes here were the only callers, and they do not
// authenticate. The first delivered tool failed on its first real call.
void test_phase7_live_forwarding_authenticates_with_the_session_token() {
    auto client = std::make_shared<LeaseAwareClient>();
    client->raw_client->response = didi::json::object();
    const didi::json arguments = {{"target_node", "/root/Emitter"}};
    const auto binding = didi::mcp::resolveAliasBinding("signal_list_connections", arguments);
    (void)didi::mcp::sendPhase7LiveRequest(binding, arguments, client);

    ASSERT_EQ(client->raw_client->requests, 1);
    const auto& sent = client->raw_client->last_params;
    ASSERT_TRUE(sent.contains("_didi_session_token"));
    ASSERT_EQ(sent["_didi_session_token"], std::string(64, 'a'));
    // The caller's own arguments must survive the injection untouched.
    ASSERT_EQ(sent["target_node"], "/root/Emitter");
}

void test_phase7_live_forwarding_quarantines_exact_malformed_route() {
    auto client = std::make_shared<LeaseAwareClient>();
    client->raw_client->response = didi::json::array({"malformed"});
    const auto binding = didi::mcp::resolveAliasBinding("signal_connect", didi::json::object());
    const auto result = didi::mcp::sendPhase7LiveRequest(
        binding, didi::json::object(), client);

    ASSERT_TRUE(result.isError);
    ASSERT_EQ(client->wrapper_requests, 0);
    ASSERT_EQ(client->raw_client->requests, 1);
    ASSERT_EQ(client->quarantines, 1);
    ASSERT_EQ(client->quarantined_generation, client->generation);
    const auto payload = didi::json::parse(result.content.at(0).text);
    ASSERT_EQ(payload["error"]["code"], 500);
    ASSERT_EQ(payload["error"]["data"]["tool"], "signal_connect");
    ASSERT_EQ(payload["error"]["data"]["route_quarantine"], true);
}

void test_phase7_live_forwarding_rejects_malformed_error_members() {
    const std::array<didi::json, 4> malformed_errors = {
        didi::json("error"), didi::json(nullptr), didi::json::array(),
        didi::json{{"code", 409}}};
    for (const auto& malformed_error : malformed_errors) {
        auto client = std::make_shared<LeaseAwareClient>();
        client->raw_client->response = didi::json{{"error", malformed_error}};
        const auto binding = didi::mcp::resolveAliasBinding(
            "signal_connect", didi::json::object());
        const auto result = didi::mcp::sendPhase7LiveRequest(
            binding, didi::json::object(), client);

        ASSERT_TRUE(result.isError);
        ASSERT_EQ(client->wrapper_requests, 0);
        ASSERT_EQ(client->raw_client->requests, 1);
        ASSERT_EQ(client->quarantines, 1);
        ASSERT_EQ(client->quarantined_generation, client->generation);
        const auto payload = didi::json::parse(result.content.at(0).text);
        ASSERT_EQ(payload["error"]["code"], 500);
        ASSERT_EQ(payload["error"]["message"], "extension_protocol_error");
        ASSERT_EQ(payload["error"]["data"]["tool"], "signal_connect");
    }
}

void test_phase7_live_forwarding_fails_closed_without_managed_lease() {
    auto client = std::make_shared<MissingLeaseClient>();
    const auto binding = didi::mcp::resolveAliasBinding(
        "runtime_read_profiler", didi::json::object());
    const auto result = didi::mcp::sendPhase7LiveRequest(
        binding, didi::json::object(), client);

    ASSERT_TRUE(result.isError);
    ASSERT_EQ(client->lease_acquisitions, 1);
    ASSERT_EQ(client->requests, 0);
    const auto payload = didi::json::parse(result.content.at(0).text);
    ASSERT_EQ(payload["error"]["code"], 503);
    ASSERT_EQ(payload["error"]["data"]["tool"], "runtime_read_profiler");
}

void test_phase7_blockers_never_acquire_or_dispatch_a_route() {
    const std::array<const char*, 3> blockers = {
        "physics_simulate_step", "nav_bake_mesh", "runtime_get_call_stack"};
    for (const auto* blocker : blockers) {
        auto client = std::make_shared<LeaseAwareClient>();
        const auto binding = didi::mcp::resolveAliasBinding(
            blocker, didi::json::object());
        const auto result = didi::mcp::sendPhase7LiveRequest(
            binding, didi::json::object(), client);

        ASSERT_TRUE(result.isError);
        ASSERT_EQ(client->lease_acquisitions, 0);
        ASSERT_EQ(client->wrapper_requests, 0);
        ASSERT_EQ(client->raw_client->requests, 0);
        const auto payload = didi::json::parse(result.content.at(0).text);
        ASSERT_EQ(payload["error"]["code"], 501);
        ASSERT_EQ(payload["error"]["data"]["tool"], blocker);
    }
}

void test_phase7_live_forwarding_enforces_exact_serialized_caps() {
    struct Row {
        const char* invoked;
        const char* canonical;
        const char* method;
        size_t cap;
    };
    const std::array<Row, 3> rows = {{
        {"signal_list_connections", "signal_list_connections", "signal.listConnections",
         64u * 1024u},
        {"tilemap_get_used_rect", "tilemap_get_used_rect", "tilemap.getUsedRect",
         16u * 1024u},
        {"runtime_read_profiler", "runtime_read_profiler", "runtime.readProfiler",
         256u * 1024u},
    }};
    for (const auto& row : rows) {
        auto client = std::make_shared<LeaseAwareClient>();
        client->raw_client->response = didi::json{{"blob", std::string(row.cap, 'x')}};
        const auto binding = didi::mcp::resolveAliasBinding(
            row.invoked, didi::json::object());
        const auto result = didi::mcp::sendPhase7LiveRequest(
            binding, didi::json::object(), client);

        ASSERT_TRUE(result.isError);
        ASSERT_EQ(client->wrapper_requests, 0);
        ASSERT_EQ(client->raw_client->requests, 1);
        ASSERT_EQ(client->quarantines, 1);
        ASSERT_EQ(client->quarantined_generation, client->generation);
        const auto payload = didi::json::parse(result.content.at(0).text);
        ASSERT_EQ(payload["error"]["code"], 413);
        ASSERT_EQ(payload["error"]["data"]["tool"], row.invoked);
        ASSERT_EQ(payload["error"]["data"]["limit_bytes"], row.cap);
        ASSERT_EQ(payload["error"]["data"]["route_quarantine"], true);
    }
}

struct RegisterPhase7ContractTests {
    RegisterPhase7ContractTests() {
        registerTest("Phase7Contract.AllTenAliasBindings",
                     test_phase7_all_ten_alias_bindings_are_exact);
        registerTest("phase7 generated schemas", test_phase7_generated_schemas);
        registerTest("Phase7Contract.LiveForwardingDeadline",
                     test_phase7_live_forwarding_uses_exact_method_and_deadline);
        registerTest("Phase7Contract.LiveForwardingBoundDispatch",
                     test_phase7_live_forwarding_preserves_bound_dispatch_and_error_identity);
        registerTest("Phase7Contract.LiveForwardingAuthenticates",
                     test_phase7_live_forwarding_authenticates_with_the_session_token);
        registerTest("Phase7Contract.LiveForwardingMalformedQuarantine",
                     test_phase7_live_forwarding_quarantines_exact_malformed_route);
        registerTest("Phase7Contract.LiveForwardingMalformedErrorMembers",
                     test_phase7_live_forwarding_rejects_malformed_error_members);
        registerTest("Phase7Contract.LiveForwardingMissingManagedLease",
                     test_phase7_live_forwarding_fails_closed_without_managed_lease);
        registerTest("Phase7Contract.BlockersNeverDispatch",
                     test_phase7_blockers_never_acquire_or_dispatch_a_route);
        registerTest("Phase7Contract.LiveForwardingSerializedCaps",
                     test_phase7_live_forwarding_enforces_exact_serialized_caps);
    }
} g_registerPhase7ContractTests;

} // namespace
