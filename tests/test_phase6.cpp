#include "didi/common/project_path.hpp"
#include "didi/gdextension/session_host.hpp"
#include "didi/mcp/mutation_safety.hpp"
#include "didi/mcp/tool_registry.hpp"
#include "didi/runtime/session_lock.hpp"

#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>

void registerTest(const std::string& name, std::function<void()> fn);

#define TEST(suite, name) \
    void test_##suite##_##name(); \
    struct Register_##suite##_##name { \
        Register_##suite##_##name() { registerTest(#suite "." #name, test_##suite##_##name); } \
    } g_register_##suite##_##name; \
    void test_##suite##_##name()

#define ASSERT_TRUE(cond) \
    if (!(cond)) { \
        std::cerr << "Assertion failed: (" #cond ") at " << __FILE__ << ":" << __LINE__ << std::endl; \
        throw std::runtime_error("Assertion failed: " #cond); \
    }

#define ASSERT_FALSE(cond) ASSERT_TRUE(!(cond))
#define ASSERT_EQ(a, b) ASSERT_TRUE((a) == (b))
#define ASSERT_NE(a, b) ASSERT_TRUE((a) != (b))

namespace {

class Phase6RuntimeClient final : public didi::ipc::IIpcClient {
public:
    explicit Phase6RuntimeClient(didi::runtime::SessionDescriptor descriptor)
        : descriptor(std::move(descriptor)) {}
    bool connect(const std::string& endpoint, int) override {
        connected = endpoint == descriptor.endpoint;
        return connected;
    }
    void disconnect() override { connected = false; }
    bool isConnected() const override { return connected; }
    didi::Result<didi::json> sendRequest(const std::string& method,
                                         const didi::json&, int) override {
        if (!connected) return didi::Error::notConnected();
        if (method == "session.handshake") {
            auto response = descriptor.toJson();
            response["status"] = "ok";
            return response;
        }
        return didi::json::object();
    }

private:
    didi::runtime::SessionDescriptor descriptor;
    bool connected{false};
};

class CountingMutationClient final : public didi::ipc::IIpcClient {
public:
    bool connect(const std::string&, int) override { return true; }
    void disconnect() override {}
    bool isConnected() const override { return true; }
    didi::Result<didi::json> sendRequest(const std::string&, const didi::json&, int) override {
        ++requests;
        return didi::json::object();
    }
    int requests{0};
};

class ScopedPhase6Directory {
public:
    explicit ScopedPhase6Directory(const std::string& name)
        : root(std::filesystem::temp_directory_path() / ("didi-phase6-" + name)) {
        std::error_code error;
        std::filesystem::remove_all(root, error);
        std::filesystem::create_directories(root);
    }

    ~ScopedPhase6Directory() {
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }

    std::filesystem::path root;
};

didi::mcp::MutationContext offlineContext(const std::filesystem::path& root) {
    didi::mcp::MutationContext context;
    context.project_root = root.string();
    context.execution_mode = "offline_fallback";
    return context;
}

didi::json dryRun(didi::json arguments) {
    arguments["dry_run"] = true;
    return arguments;
}

didi::mcp::MutationDecision evaluateBinding(didi::mcp::MutationSafety& safety,
                                            std::string_view invoked_name,
                                            const didi::json& arguments,
                                            const didi::mcp::MutationContext& context) {
    const auto binding = didi::mcp::resolveAliasBinding(invoked_name, arguments);
    return safety.evaluate(binding, arguments, context);
}

} // namespace

TEST(Phase6, ExplicitProjectRootRequiresGodotProject) {
    ScopedPhase6Directory directory("project-root");
    ASSERT_TRUE(didi::paths::resolveExplicitProjectRoot("").isErr());
    ASSERT_TRUE(didi::paths::resolveExplicitProjectRoot(directory.root.string()).isErr());

    std::ofstream(directory.root / "project.godot") << "[application]\nconfig/name=\"Phase6\"\n";
    const auto resolved = didi::paths::resolveExplicitProjectRoot(directory.root.string());
    ASSERT_TRUE(resolved.isOk());
    ASSERT_EQ(resolved.value(), std::filesystem::weakly_canonical(directory.root));
}

TEST(Phase6, ProjectEndpointKeysAreStableAndIsolated) {
    ScopedPhase6Directory first("endpoint-a");
    ScopedPhase6Directory second("endpoint-b");
    const auto first_key = didi::paths::projectEndpointKey(first.root);
    ASSERT_EQ(first_key.size(), 16u);
    ASSERT_EQ(first_key, didi::paths::projectEndpointKey(first.root / "."));
    ASSERT_NE(first_key, didi::paths::projectEndpointKey(second.root));

    didi::godot::SessionHost host;
    ASSERT_TRUE(host.prepare("editor", first.root.string()).isOk());
    const auto descriptor = host.descriptor();
    ASSERT_TRUE(descriptor.has_value());
    ASSERT_TRUE(descriptor->endpoint.find(first_key) != std::string::npos);
    ASSERT_TRUE(didi::runtime::SessionDescriptor::fromJson(descriptor->toJson(true)).isOk());
    host.stop();
}

TEST(Phase6, RuntimeSessionLockIsExclusiveAndReleasedByRaii) {
    ScopedPhase6Directory directory("session-lock");
    const auto path = directory.root / "session.lock";
    const didi::json first_owner = {{"client_id", "first"}, {"project_path", directory.root.string()}};
    const didi::json second_owner = {{"client_id", "second"}, {"project_path", directory.root.string()}};

    auto first = didi::runtime::RuntimeSessionLock::acquire(path, first_owner);
    ASSERT_TRUE(first.isOk());
    auto rejected = didi::runtime::RuntimeSessionLock::acquire(path, second_owner);
    ASSERT_TRUE(rejected.isErr());
    ASSERT_EQ(rejected.error().code, 423);

    first.value().reset();
    auto replacement = didi::runtime::RuntimeSessionLock::acquire(path, second_owner);
    ASSERT_TRUE(replacement.isOk());
}

TEST(Phase6, RuntimeSessionClientsEnforceOneOwnerAndRecoverAfterDetach) {
    ScopedPhase6Directory directory("client-lock");
    const auto session_directory = directory.root / "sessions";
    std::filesystem::create_directories(session_directory);
#if defined(_WIN32)
    _putenv_s("DIDI_SESSION_DIR", session_directory.string().c_str());
#else
    setenv("DIDI_SESSION_DIR", session_directory.string().c_str(), 1);
#endif
    didi::godot::SessionHost host;
    ASSERT_TRUE(host.prepare("editor", directory.root.string()).isOk());
    ASSERT_TRUE(host.publish().isOk());
    const auto descriptor = host.descriptor();
    ASSERT_TRUE(descriptor.has_value());
    auto factory = [descriptor] {
        return std::make_unique<Phase6RuntimeClient>(*descriptor);
    };
    auto first = didi::runtime::createRuntimeSessionClient(directory.root.string(), factory);
    auto second = didi::runtime::createRuntimeSessionClient(directory.root.string(), factory);
    ASSERT_TRUE(first->attachSession(descriptor->session_id).isOk());
    ASSERT_TRUE(first->attachSession(descriptor->session_id).isOk());
    const auto locked = second->attachSession(descriptor->session_id);
    ASSERT_TRUE(locked.isErr());
    ASSERT_EQ(locked.error().code, 423);
    first->disconnect();
    ASSERT_TRUE(second->attachSession(descriptor->session_id).isOk());
    second->disconnect();
    host.stop();
#if defined(_WIN32)
    _putenv_s("DIDI_SESSION_DIR", "");
#else
    unsetenv("DIDI_SESSION_DIR");
#endif
}

TEST(Phase6, MutationPreviewIsNonExecutingAndBindsSingleUseConfirmation) {
    ScopedPhase6Directory directory("mutation-token");
    int64_t now = 1'000;
    int token_index = 0;
    didi::mcp::MutationSafety safety(
        [&] { return now; },
        [&] { return token_index++ == 0 ? std::string(64, 'a') : std::string(64, 'b'); });
    const auto context = offlineContext(directory.root);

    auto preview = evaluateBinding(safety, "editor_reload_project", {{"dry_run", true}}, context);
    ASSERT_FALSE(preview.execute);
    ASSERT_FALSE(preview.is_error);
    ASSERT_TRUE(preview.payload["mutation_preview"]["requires_confirmation"]);
    const auto token = preview.payload["mutation_preview"]["confirmation_token"].get<std::string>();
    ASSERT_EQ(token, std::string(64, 'a'));

    auto missing = evaluateBinding(safety, "editor_reload_project", didi::json::object(), context);
    ASSERT_FALSE(missing.execute);
    ASSERT_TRUE(missing.is_error);
    ASSERT_EQ(missing.payload["error"]["code"], 428);

    auto confirmed = evaluateBinding(safety,
        "editor_reload_project", {{"confirmation_token", token}}, context);
    ASSERT_TRUE(confirmed.execute);
    ASSERT_TRUE(confirmed.arguments.empty());

    auto replay = evaluateBinding(safety,
        "editor_reload_project", {{"confirmation_token", token}}, context);
    ASSERT_FALSE(replay.execute);
    ASSERT_TRUE(replay.is_error);
    ASSERT_EQ(replay.payload["error"]["code"], 409);
}

TEST(Phase6, MutationConfirmationRejectsTamperingExpiryAndContextChanges) {
    ScopedPhase6Directory directory("mutation-binding");
    int64_t now = 10'000;
    int token_index = 0;
    didi::mcp::MutationSafety safety(
        [&] { return now; },
        [&] { return std::string(63, 'c') + static_cast<char>('0' + token_index++); });
    auto context = offlineContext(directory.root);
    const didi::json arguments = {{"file_path", "res://player.gd"}, {"method_name", "tick"}};

    auto preview = evaluateBinding(safety, "script_patch_method", dryRun(arguments), context);
    const auto token = preview.payload["mutation_preview"]["confirmation_token"].get<std::string>();
    auto changed_arguments = arguments;
    changed_arguments["method_name"] = "other";
    changed_arguments["confirmation_token"] = token;
    auto tampered = evaluateBinding(safety, "script_patch_method", changed_arguments, context);
    ASSERT_TRUE(tampered.is_error);
    ASSERT_EQ(tampered.payload["error"]["code"], 409);

    auto context_preview = evaluateBinding(safety, "script_patch_method", dryRun(arguments), context);
    const auto context_token = context_preview.payload["mutation_preview"]["confirmation_token"].get<std::string>();
    auto other_context = context;
    other_context.project_root += "-other";
    auto context_changed = arguments;
    context_changed["confirmation_token"] = context_token;
    auto rejected_context = evaluateBinding(safety, "script_patch_method", context_changed, other_context);
    ASSERT_TRUE(rejected_context.is_error);

    auto expiry_preview = evaluateBinding(safety, "script_patch_method", dryRun(arguments), context);
    const auto expiry_token = expiry_preview.payload["mutation_preview"]["confirmation_token"].get<std::string>();
    now += didi::mcp::MutationSafety::kConfirmationTtlMs + 1;
    auto expired_arguments = arguments;
    expired_arguments["confirmation_token"] = expiry_token;
    auto expired = evaluateBinding(safety, "script_patch_method", expired_arguments, context);
    ASSERT_TRUE(expired.is_error);
    ASSERT_EQ(expired.payload["error"]["code"], 410);
}

TEST(Phase6, MutationSchemasAdvertiseDryRunAndProtectedConfirmation) {
    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();
    const auto* reload = registry.getTool("editor_reload_project");
    const auto* scene_set = registry.getTool("scene_set_property");
    const auto* hierarchy = registry.getTool("scene_get_hierarchy");
    ASSERT_TRUE(reload != nullptr && scene_set != nullptr && hierarchy != nullptr);
    ASSERT_EQ(reload->inputSchema["properties"]["dry_run"]["type"], "boolean");
    ASSERT_EQ(reload->inputSchema["properties"]["confirmation_token"]["type"], "string");
    ASSERT_EQ(scene_set->inputSchema["properties"]["dry_run"]["type"], "boolean");
    ASSERT_TRUE(!scene_set->inputSchema["properties"].contains("confirmation_token"));
    ASSERT_TRUE(!hierarchy->inputSchema.value("properties", didi::json::object()).contains("dry_run"));
}

TEST(Phase6, LegacyMutationAliasesAdvertiseDryRunAndAcceptPreviews) {
    // Break caught: mutate_scene_tree was missing from the mutation table, so it
    // advertised no dry_run and rejected a preview pass with 400.
    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();

    ScopedPhase6Directory directory("legacy-mutation-aliases");
    didi::mcp::MutationSafety safety;
    const auto context = offlineContext(directory.root);

    for (const char* name : {"mutate_scene_tree", "instantiate_asset", "inject_input_event"}) {
        const auto* tool = registry.getTool(name);
        ASSERT_TRUE(tool != nullptr);
        ASSERT_EQ(tool->inputSchema["properties"]["dry_run"]["type"], "boolean");

        const auto decision = evaluateBinding(safety, name, dryRun(didi::json::object()), context);
        ASSERT_FALSE(decision.is_error);
        ASSERT_FALSE(decision.execute);
    }
}

TEST(Phase6, RegistryDryRunNeverDispatchesMutationHandler) {
    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();
    auto client = std::make_shared<CountingMutationClient>();
    registry.setIpcClient(client);
    const auto result = registry.callTool(
        "scene_set_property",
        {{"target_node", "/root/Player"}, {"property", "visible"},
         {"value", false}, {"dry_run", true}});
    ASSERT_FALSE(result.isError);
    ASSERT_EQ(client->requests, 0);
    registry.setIpcClient(nullptr);
}

TEST(Phase6, AliasConfirmationTokensRejectCrossNameUseBothDirections) {
    ScopedPhase6Directory directory("alias-token-identity");
    int token_index = 0;
    didi::mcp::MutationSafety safety(
        [] { return int64_t{50'000}; },
        [&] { return std::string(63, 'd') + static_cast<char>('0' + token_index++); });
    const auto context = offlineContext(directory.root);
    const didi::json arguments = {
        {"file_path", "res://player.gd"}, {"method_name", "tick"},
        {"new_definition", "func tick():\n\tpass"}
    };

    const auto canonical_preview = evaluateBinding(
        safety, "script_patch_method", dryRun(arguments), context);
    ASSERT_FALSE(canonical_preview.is_error);
    ASSERT_EQ(canonical_preview.payload["mutation_preview"]["tool"],
              "script_patch_method");
    const auto canonical_token = canonical_preview.payload["mutation_preview"]
        ["confirmation_token"].get<std::string>();
    auto alias_use = arguments;
    alias_use["confirmation_token"] = canonical_token;
    const auto alias_rejected = evaluateBinding(
        safety, "patch_script_symbols", alias_use, context);
    ASSERT_TRUE(alias_rejected.is_error);
    ASSERT_EQ(alias_rejected.payload["tool"], "patch_script_symbols");
    ASSERT_EQ(alias_rejected.payload["error"]["code"], 409);

    const auto alias_preview = evaluateBinding(
        safety, "patch_script_symbols", dryRun(arguments), context);
    ASSERT_FALSE(alias_preview.is_error);
    ASSERT_EQ(alias_preview.payload["mutation_preview"]["tool"],
              "patch_script_symbols");
    const auto alias_token = alias_preview.payload["mutation_preview"]
        ["confirmation_token"].get<std::string>();
    auto canonical_use = arguments;
    canonical_use["confirmation_token"] = alias_token;
    const auto canonical_rejected = evaluateBinding(
        safety, "script_patch_method", canonical_use, context);
    ASSERT_TRUE(canonical_rejected.is_error);
    ASSERT_EQ(canonical_rejected.payload["tool"], "script_patch_method");
    ASSERT_EQ(canonical_rejected.payload["error"]["code"], 409);
}

TEST(Phase6, RuntimeInputAliasDryRunKeepsInvokedIdentity) {
    ScopedPhase6Directory directory("input-alias-identity");
    didi::mcp::MutationSafety safety;
    auto context = offlineContext(directory.root);
    context.execution_mode = "live";
    context.session_id = "0123456789abcdef0123456789abcdef";
    context.route_generation = 7;
    const didi::json arguments = {
        {"events", didi::json::array({{{"type", "action"},
                                        {"action_name", "jump"}, {"pressed", true}}})}
    };
    const auto canonical = evaluateBinding(
        safety, "runtime_inject_input", dryRun(arguments), context);
    const auto alias = evaluateBinding(
        safety, "inject_input_event", dryRun(arguments), context);
    ASSERT_FALSE(canonical.is_error);
    ASSERT_FALSE(alias.is_error);
    ASSERT_EQ(canonical.payload["mutation_preview"]["tool"], "runtime_inject_input");
    ASSERT_EQ(alias.payload["mutation_preview"]["tool"], "inject_input_event");
    ASSERT_NE(canonical.payload["mutation_preview"]["binding_hash"],
              alias.payload["mutation_preview"]["binding_hash"]);
    ASSERT_EQ(canonical.payload["mutation_preview"]["arguments"],
              alias.payload["mutation_preview"]["arguments"]);
}
