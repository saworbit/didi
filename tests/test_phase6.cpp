#include "didi/common/json_bounds.hpp"
#include "didi/common/project_path.hpp"
#include "didi/common/scene_node_path.hpp"
#include "didi/gdextension/gdextension_ipc.hpp"
#include "didi/gdextension/session_host.hpp"
#include "didi/mcp/mutation_safety.hpp"
#include "didi/mcp/tool_registry.hpp"
#include "didi/runtime/session_lock.hpp"

#include <filesystem>
#include <algorithm>
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

// The same fake, keeping the one thing the handshake deadline is about.
class RecordingHandshakeClient final : public didi::ipc::IIpcClient {
public:
    RecordingHandshakeClient(didi::runtime::SessionDescriptor descriptor, int& handshake_timeout_ms)
        : descriptor(std::move(descriptor)), timeout(handshake_timeout_ms) {}
    bool connect(const std::string& endpoint, int) override {
        connected = endpoint == descriptor.endpoint;
        return connected;
    }
    void disconnect() override { connected = false; }
    bool isConnected() const override { return connected; }
    didi::Result<didi::json> sendRequest(const std::string& method, const didi::json&,
                                         int timeout_ms) override {
        if (!connected) return didi::Error::notConnected();
        if (method != "session.handshake") return didi::json::object();
        timeout = timeout_ms;
        auto response = descriptor.toJson();
        response["status"] = "ok";
        return response;
    }

private:
    didi::runtime::SessionDescriptor descriptor;
    int& timeout;
    bool connected{false};
};

// Restores the shipped idle-recycle numbers however the test leaves.
struct ScopedIdleRecycleOverride {
    ScopedIdleRecycleOverride(int server_recycle_ms, int client_reuse_ms) {
        didi::ipc::testing::setIdleRecycleOverridesForTesting(server_recycle_ms, client_reuse_ms);
    }
    ~ScopedIdleRecycleOverride() {
        didi::ipc::testing::clearIdleRecycleOverridesForTesting();
    }
};

class CountingMutationClient final : public didi::ipc::IIpcClient {
public:
    bool connect(const std::string&, int) override { return true; }
    void disconnect() override {}
    bool isConnected() const override { return true; }
    didi::Result<didi::json> sendRequest(const std::string& method, const didi::json&,
                                         int) override {
        ++requests;
        methods.push_back(method);
        return didi::json::object();
    }
    int requests{0};
    std::vector<std::string> methods;
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

// The refusal the IPC handler answers an unauthorised peer with used to be
// rebuilt from two of Error's three fields, so anything under `data` was
// dropped on the way out (#890). Nothing was losing a code that day, which is
// exactly why it needed pinning: the loss would have arrived with whichever
// refusal grew one.
TEST(Phase6, SessionRefusalKeepsTheStructuredHalf) {
    const auto refusal = didi::godot::sessionRefusal(
        didi::Error(403, "Runtime session token was rejected",
                    {{"code", "session_token_rejected"}, {"retryable", false}}));

    ASSERT_TRUE(refusal.contains("error"));
    const auto& error = refusal["error"];
    ASSERT_EQ(error["code"].get<int>(), 403);
    ASSERT_EQ(error["message"].get<std::string>(), "Runtime session token was rejected");
    ASSERT_TRUE(error.contains("data"));
    ASSERT_EQ(error["data"]["code"].get<std::string>(), "session_token_rejected");
    ASSERT_FALSE(error["data"]["retryable"].get<bool>());
}

// An Error with no data must produce no `data` key, not a null one. A null is
// not the same as absent to the code that reads this: nlohmann's `value`
// returns the stored null when the key is present, so
// `error.value("data", json::object())` would hand back a null where it asked
// for an object, and every reader of a live failure does exactly that.
TEST(Phase6, SessionRefusalOmitsAbsentDataRatherThanNullingIt) {
    const auto refusal = didi::godot::sessionRefusal(
        didi::Error(503, "Runtime session host is not prepared"));

    const auto& error = refusal["error"];
    ASSERT_FALSE(error.contains("data"));
    ASSERT_TRUE(error.value("data", didi::json::object()).is_object());
}

TEST(Phase6, ExplicitProjectRootRequiresGodotProject) {
    ScopedPhase6Directory directory("project-root");
    ASSERT_TRUE(didi::paths::resolveExplicitProjectRoot("").isErr());
    ASSERT_TRUE(didi::paths::resolveExplicitProjectRoot(directory.root.string()).isErr());

    std::ofstream(directory.root / "project.godot") << "[application]\nconfig/name=\"Phase6\"\n";
    const auto resolved = didi::paths::resolveExplicitProjectRoot(directory.root.string());
    ASSERT_TRUE(resolved.isOk());
    ASSERT_EQ(resolved.value(), std::filesystem::weakly_canonical(directory.root));
}

TEST(Phase6, ExplicitProjectRootRefusesBytesThatAreNotUtf8) {
    // A root the platform cannot decode is answered, not thrown. On Windows the
    // conversion to a native wide path throws std::system_error, which is not a
    // filesystem_error, so the refusal written for this case was never reached
    // and the exception escaped main and fast-failed the process with no output
    // at all (#611). A POSIX path is a byte string and these bytes name a
    // directory that is simply not there, which is the other error below.
    std::string invalid = "projects/pr";
    invalid.push_back(static_cast<char>(0xF3));  // Latin-1 'o acute', not valid UTF-8
    invalid += "jekt";
    const auto resolved = didi::paths::resolveExplicitProjectRoot(invalid);
    ASSERT_TRUE(resolved.isErr());
#if defined(_WIN32)
    ASSERT_TRUE(resolved.error().message.find("UTF-8") != std::string::npos);
#else
    ASSERT_TRUE(resolved.error().message.find("accessible directory") != std::string::npos);
#endif
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

// Break caught: naming a session explicitly skipped the project check that
// auto-selection has always applied, so a server started on project B would
// attach project A's editor and serve its scene tree, with mutations routed
// there too (#387).
TEST(Phase6, AttachRefusesASessionFromAnotherProjectUnlessAsked) {
    ScopedPhase6Directory directory("foreign-project");
    const auto session_directory = directory.root / "sessions";
    const auto other_project = directory.root / "other";
    const auto this_project = directory.root / "mine";
    std::filesystem::create_directories(session_directory);
    std::filesystem::create_directories(other_project);
    std::filesystem::create_directories(this_project);
#if defined(_WIN32)
    _putenv_s("DIDI_SESSION_DIR", session_directory.string().c_str());
#else
    setenv("DIDI_SESSION_DIR", session_directory.string().c_str(), 1);
#endif
    didi::godot::SessionHost host;
    ASSERT_TRUE(host.prepare("editor", other_project.string()).isOk());
    ASSERT_TRUE(host.publish().isOk());
    const auto descriptor = host.descriptor();
    ASSERT_TRUE(descriptor.has_value());
    auto factory = [descriptor] {
        return std::make_unique<Phase6RuntimeClient>(*descriptor);
    };

    auto client = didi::runtime::createRuntimeSessionClient(this_project.string(), factory);
    const auto refused = client->attachSession(descriptor->session_id);
    ASSERT_TRUE(refused.isErr());
    ASSERT_EQ(refused.error().code, 409);
    // The error has to name both paths, or the caller cannot tell which of the
    // two is the one they got wrong.
    ASSERT_TRUE(refused.error().message.find("different project") != std::string::npos);
    ASSERT_TRUE(refused.error().data.contains("session_project_path"));
    ASSERT_TRUE(refused.error().data.contains("server_project_root"));

    // Explicit intent still works, and says loudly what it did.
    const auto allowed = client->attachSession(descriptor->session_id, true);
    ASSERT_TRUE(allowed.isOk());
    ASSERT_TRUE(allowed.value().value("project_mismatch", false));
    ASSERT_TRUE(!allowed.value().value("limitation", std::string()).empty());
    client->disconnect();

    // A session for this server's own project is untouched by any of it.
    didi::godot::SessionHost local;
    ASSERT_TRUE(local.prepare("editor", this_project.string()).isOk());
    ASSERT_TRUE(local.publish().isOk());
    const auto local_descriptor = local.descriptor();
    ASSERT_TRUE(local_descriptor.has_value());
    auto local_factory = [local_descriptor] {
        return std::make_unique<Phase6RuntimeClient>(*local_descriptor);
    };
    auto matched = didi::runtime::createRuntimeSessionClient(this_project.string(), local_factory);
    const auto attached = matched->attachSession(local_descriptor->session_id);
    ASSERT_TRUE(attached.isOk());
    ASSERT_FALSE(attached.value().value("project_mismatch", false));
    matched->disconnect();
}

// Break caught: the attach handshake allowed a flat 3000 ms. A server serves
// one accepted connection at a time and only accepts the next once the one it
// holds has been idle for its recycle window, so a client arriving during that
// window connects and then waits with a request nothing has read. 3000 is over
// the Windows window of 1000 and under the POSIX window of 5000, so the
// runtime_attach_session that runtime_launch --detach documents answered 504
// with outcome_unknown on macOS and Linux and worked on Windows (#782).
//
// The window is set well over the old flat number here, so a deadline that
// goes back to being chosen without reference to it fails this.
TEST(Phase6, AttachGivesTheHandshakeTimeToBeAccepted) {
    ScopedPhase6Directory directory("handshake-deadline");
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

    constexpr int kRecycleMs = 4000;
    ScopedIdleRecycleOverride override_margin(kRecycleMs, 1000);
    int handshake_timeout_ms = 0;
    auto factory = [descriptor, &handshake_timeout_ms] {
        return std::make_unique<RecordingHandshakeClient>(*descriptor, handshake_timeout_ms);
    };
    auto client = didi::runtime::createRuntimeSessionClient(directory.root.string(), factory);
    ASSERT_TRUE(client->attachSession(descriptor->session_id).isOk());
    ASSERT_TRUE(handshake_timeout_ms > kRecycleMs);

    client->disconnect();
    host.stop();
#if defined(_WIN32)
    _putenv_s("DIDI_SESSION_DIR", "");
#else
    unsetenv("DIDI_SESSION_DIR");
#endif
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

    // The 428 is the error a caller is most likely to branch on, because the
    // right response to it is mechanical: dry-run the same arguments, take the
    // token, call again. It used to carry no data at all, so "this needs a
    // token" was recoverable only from the prose (#487).
    const auto& gate_data = missing.payload["error"]["data"];
    ASSERT_EQ(gate_data["code"], "confirmation_required");
    ASSERT_EQ(gate_data["tool"], "editor_reload_project");
    ASSERT_EQ(gate_data["canonical_tool"], "editor_reload_project");
    ASSERT_EQ(gate_data["retryable"], true);
    ASSERT_EQ(gate_data["dry_run_argument"], "dry_run");
    ASSERT_EQ(gate_data["confirmation_argument"], "confirmation_token");

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

TEST(Phase6, RepeatabilityFollowsTheMutationBoundaryAndTheArguments) {
    // Break caught: repeatability read the tool name alone, so a ghost preview
    // asked to accumulate was treated as safe to send twice and drew the same
    // proposal on screen twice.
    const auto repeatable = [](const char* name, didi::json arguments) {
        return didi::mcp::liveCallIsRepeatable(didi::mcp::resolveAliasBinding(name, arguments),
                                               arguments);
    };

    for (const char* name : {"eval_gdscript", "runtime_get_tree", "scene_get_property",
                             "viewport_capture_frame", "tilemap_get_used_rect",
                             "spatial_query_frustum", "shader_list_uniforms"}) {
        ASSERT_TRUE(repeatable(name, didi::json::object()));
    }
    for (const char* name : {"scene_remove_node", "runtime_step", "signal_emit",
                             "tilemap_set_cells", "shader_set_uniform", "editor_save_scene"}) {
        ASSERT_FALSE(repeatable(name, didi::json::object()));
    }

    ASSERT_TRUE(repeatable("editor_render_ghost_preview", didi::json::object()));
    ASSERT_TRUE(repeatable("editor_render_ghost_preview", {{"replace", true}}));
    ASSERT_FALSE(repeatable("editor_render_ghost_preview", {{"replace", false}}));
}

// A bridge that refuses the probe's read the way the real call would refuse
// the mutation, delivered the way the IPC client delivers a bridge error
// envelope: as an Error carrying the bridge's code and data, with the
// identifier a caller branches on under data.code.
class RefusingProbeClient final : public didi::ipc::IIpcClient {
public:
    bool connect(const std::string&, int) override { return true; }
    void disconnect() override {}
    bool isConnected() const override { return true; }
    didi::Result<didi::json> sendRequest(const std::string& method, const didi::json& params,
                                         int) override {
        methods.push_back(method);
        last_params = params;
        return didi::Error(409, "The edited scene does not own this node.",
                           didi::json{{"code", "node_not_owned"}, {"retryable", false}});
    }
    std::vector<std::string> methods;
    didi::json last_params;
};

TEST(Phase6, RegistryDryRunRefusesWhatTheProbeRefuses) {
    // Break caught: the probe passed a 404 through and swallowed every other
    // refusal, so a node the file cannot hold previewed as a planned mutation
    // with the target read (#588). The refusal's data has to survive too,
    // because data.code is what a caller branches on.
    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();
    auto client = std::make_shared<RefusingProbeClient>();
    registry.setIpcClient(client);
    const auto result = registry.callTool(
        "scene_remove_node",
        {{"target_node", "/root/Main/SubInst/Inner"}, {"dry_run", true}});
    ASSERT_TRUE(result.isError);
    const auto payload = didi::json::parse(result.content[0].text);
    ASSERT_EQ(payload["error"]["code"], 409);
    ASSERT_EQ(payload["error"]["data"]["code"], "node_not_owned");
    // The probe named the mutation, so the bridge could run that call's checks.
    ASSERT_EQ(client->last_params["mutation"]["tool"], "scene_remove_node");
    ASSERT_EQ(client->last_params["mutation"]["arguments"]["target_node"],
              "/root/Main/SubInst/Inner");
    ASSERT_TRUE(std::find(client->methods.begin(), client->methods.end(),
                          "scene.removeNode") == client->methods.end());
    registry.setIpcClient(nullptr);
}

TEST(Phase6, RegistryDryRunNeverDispatchesMutationHandler) {
    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();
    auto client = std::make_shared<CountingMutationClient>();
    registry.setIpcClient(client);
    const auto result = registry.callTool(
        "scene_set_property",
        {{"target_node", "/root/Player"}, {"property_name", "visible"},
         {"value", false}, {"dry_run", true}});
    ASSERT_FALSE(result.isError);

    // A preview reads its target now, so it is no longer true that a dry run
    // sends nothing (#417). What has to stay true is that it changes nothing:
    // the only thing it may send is the read-only probe, and never the
    // mutation.
    for (const auto& method : client->methods) {
        ASSERT_TRUE(method == "scene.getProperty");
    }
    ASSERT_TRUE(std::find(client->methods.begin(), client->methods.end(),
                          "scene.setProperty") == client->methods.end());
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

// A token is spent on the mutation it authorises, not on an attempt that never
// got past its own binding check. Erasing on the way in meant one mistyped
// argument killed the token the caller had just previewed (#398).
TEST(Phase6, RejectedConfirmationLeavesTheTokenSpendable) {
    ScopedPhase6Directory directory("token-survives-mismatch");
    didi::mcp::MutationSafety safety;
    const auto context = offlineContext(directory.root);
    const didi::json arguments = {
        {"file_path", "res://player.gd"}, {"method_name", "take_damage"},
        {"new_definition", "func take_damage(): pass"}
    };

    const auto preview = evaluateBinding(
        safety, "script_patch_method", dryRun(arguments), context);
    ASSERT_FALSE(preview.is_error);
    const auto token = preview.payload["mutation_preview"]["confirmation_token"]
                           .get<std::string>();

    // The same token spent on arguments the preview was not taken for.
    auto wrong = arguments;
    wrong["method_name"] = "heal";
    wrong["confirmation_token"] = token;
    const auto mismatch = evaluateBinding(safety, "script_patch_method", wrong, context);
    ASSERT_TRUE(mismatch.is_error);
    ASSERT_EQ(mismatch.payload["error"]["code"], 409);

    // The call the token was minted for still goes through.
    auto exact = arguments;
    exact["confirmation_token"] = token;
    const auto spent = evaluateBinding(safety, "script_patch_method", exact, context);
    ASSERT_FALSE(spent.is_error);

    // And once it is spent it is gone.
    const auto reused = evaluateBinding(safety, "script_patch_method", exact, context);
    ASSERT_TRUE(reused.is_error);
    ASSERT_EQ(reused.payload["error"]["code"], 409);
}

// The gate used to demand an "exact" preview and hand back one that had not
// read anything. It now says what the preview is (#407).
TEST(Phase6, PreviewSaysItOnlyBindsArguments) {
    ScopedPhase6Directory directory("preview-honesty");
    didi::mcp::MutationSafety safety;
    const auto context = offlineContext(directory.root);
    const didi::json arguments = {
        {"file_path", "res://player.gd"}, {"method_name", "tick"},
        {"new_definition", "func tick(): pass"}
    };

    const auto preview = evaluateBinding(
        safety, "script_patch_method", dryRun(arguments), context);
    ASSERT_FALSE(preview.is_error);
    const auto& mutation_preview = preview.payload["mutation_preview"];
    // Evaluated with no probe, which is the case where argument binding really
    // is all the gate can offer. It says so at the top level rather than only
    // in a nested placeholder string, and does not call the entry a planned
    // mutation when nothing was planned (#417).
    ASSERT_EQ(mutation_preview["preview_kind"], "argument_binding");
    ASSERT_EQ(mutation_preview["target_read"], false);
    ASSERT_EQ(mutation_preview["changes"][0]["kind"], "unverified_mutation");
    const auto before =
        mutation_preview["changes"][0]["before"].get<std::string>();
    ASSERT_TRUE(before.find("without opening the target") != std::string::npos);

    // And the refusal that demands it no longer calls it exact.
    const auto ungated = evaluateBinding(safety, "script_patch_method", arguments, context);
    ASSERT_TRUE(ungated.is_error);
    ASSERT_EQ(ungated.payload["error"]["code"], 428);
    const auto message = ungated.payload["error"]["message"].get<std::string>();
    ASSERT_TRUE(message.find("exact") == std::string::npos);
    ASSERT_TRUE(message.find("reads the target") != std::string::npos);
    ASSERT_TRUE(message.find("target_read") != std::string::npos);
}

TEST(Phase6, CallMethodPreviewReadsTheCallNotTheName) {
    // Break caught: scene_call_method's dry run reported the node's `name`
    // property. The same before block came back for a method the script
    // declares, for one it does not, and for every argument list, because the
    // generic node probe reads `name` when the call names no property. A token
    // was then minted for a call that fails with a 422 the preview had all the
    // evidence to see: the script is not a @tool script, so the editor never
    // made an instance of it and there is nothing to run (#463).
    ScopedPhase6Directory directory("call-method-preview");
    didi::mcp::MutationSafety safety;
    auto context = offlineContext(directory.root);
    context.execution_mode = "live";
    context.session_id = "0123456789abcdef0123456789abcdef";

    const didi::json arguments = {
        {"target_node", "/root/Main"}, {"method_name", "take_damage"},
        {"arguments", didi::json::array({5})}
    };
    const auto binding = didi::mcp::resolveAliasBinding("scene_call_method", arguments);

    // What the probe now reports: facts about this call, not a constant.
    didi::mcp::TargetProbe reading = [](const didi::json&, didi::json& before, didi::json&)
        -> std::optional<didi::Error> {
        before = {{"target_node", "/root/Main"}, {"method_name", "take_damage"},
                  {"method_exists", true}, {"script_is_tool", true},
                  {"signature", {{"name", "take_damage"}}}};
        return std::nullopt;
    };
    const auto preview = safety.evaluate(binding, dryRun(arguments), context, reading);
    ASSERT_FALSE(preview.is_error);
    const auto& mutation_preview = preview.payload["mutation_preview"];
    ASSERT_EQ(mutation_preview["preview_kind"], "target_state");
    ASSERT_EQ(mutation_preview["target_read"], true);
    const auto& before = mutation_preview["changes"][0]["before"];
    ASSERT_EQ(before["method_exists"], true);
    ASSERT_EQ(before["script_is_tool"], true);
    ASSERT_EQ(before["method_name"], "take_damage");
    // The field that made the old preview useless: it answered about `name`,
    // which the call has nothing to do with.
    ASSERT_FALSE(before.contains("property_name"));

    // And a call the engine will refuse is refused at preview, rather than
    // coming back shaped like one that will work and carrying a token.
    didi::mcp::TargetProbe refusing = [](const didi::json&, didi::json&, didi::json&)
        -> std::optional<didi::Error> {
        return didi::Error(422, "The node's script is not a @tool script");
    };
    const auto refused = safety.evaluate(binding, dryRun(arguments), context, refusing);
    ASSERT_TRUE(refused.is_error);
    ASSERT_EQ(refused.payload["error"]["code"], 422);
    ASSERT_FALSE(refused.payload.contains("mutation_preview"));
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


// Break caught: every field on the token bound it to the *call* -- the tool,
// the arguments, the project, the session. Nothing bound it to the *world*, so
// a file rewritten between the preview and the confirm was overwritten with a
// change approved against bytes that were no longer there, and the answer was
// indistinguishable from one where nothing had happened (#572).
TEST(Phase6, ConfirmationComparesTheTargetAgainstThePreview) {
    ScopedPhase6Directory directory("stale-confirmation");
    didi::mcp::MutationSafety safety;
    auto context = offlineContext(directory.root);
    const didi::json arguments = {{"file_path", "res://drift.gd"},
                                  {"method_name", "take_damage"},
                                  {"new_definition", "func take_damage() -> void:\n\tpass\n"}};
    const auto binding = didi::mcp::resolveAliasBinding("script_patch_method", arguments);

    // What the file looks like, as the probe would report it. The test moves it
    // rather than the filesystem, because what the gate compares is what the
    // probe read.
    std::string digest = "28510c216b02e9e7";
    uint64_t size = 103;
    didi::mcp::TargetProbe reading = [&](const didi::json&, didi::json& before,
                                         didi::json& subject) -> std::optional<didi::Error> {
        before = {{"exists", true}, {"path", "res://drift.gd"}, {"size_bytes", size},
                  {"content_digest", digest}, {"symbol_exists", true}};
        subject = {{"path", "res://drift.gd"}, {"symbol", "take_damage"}};
        return std::nullopt;
    };

    const auto preview = safety.evaluate(binding, dryRun(arguments), context, reading);
    ASSERT_FALSE(preview.is_error);
    const auto& mutation_preview = preview.payload["mutation_preview"];
    ASSERT_EQ(mutation_preview["target_checked_on_confirm"], true);
    // The subject the change is about, not the argument object over again. Two
    // copies made every preview a little over twice the size of its request
    // (#574).
    const auto& target = mutation_preview["changes"][0]["target"];
    ASSERT_EQ(target["path"], "res://drift.gd");
    ASSERT_EQ(target["symbol"], "take_damage");
    ASSERT_FALSE(target.contains("new_definition"));
    ASSERT_TRUE(mutation_preview["arguments"].contains("new_definition"));
    const auto token = mutation_preview["confirmation_token"].get<std::string>();

    // Something else rewrites the file. Same length, different bytes, which is
    // the ordinary shape of an edit and the case size_bytes alone would pass.
    digest = "0e554dd98e61ba22";
    auto confirmed = arguments;
    confirmed["confirmation_token"] = token;
    const auto drifted = safety.evaluate(binding, confirmed, context, reading);
    ASSERT_TRUE(drifted.is_error);
    ASSERT_EQ(drifted.payload["error"]["code"], 409);
    ASSERT_EQ(drifted.payload["error"]["data"]["target_changed"], true);

    // The token is still there, the same as an argument mismatch, so a caller
    // can look at what changed rather than minting a second one to find out.
    digest = "28510c216b02e9e7";
    const auto spent = safety.evaluate(binding, confirmed, context, reading);
    ASSERT_FALSE(spent.is_error);
    ASSERT_TRUE(spent.execute);

    // And it is spent once.
    const auto replay = safety.evaluate(binding, confirmed, context, reading);
    ASSERT_TRUE(replay.is_error);
    ASSERT_EQ(replay.payload["error"]["code"], 409);

    // A target that has gone is not drift. What this protects is the other
    // writer's work, and there is none left to discard; refusing here would
    // also undo #425, where a token minted while the target was there stays
    // spendable once it is not.
    const auto second = safety.evaluate(binding, dryRun(arguments), context, reading);
    auto vanished = arguments;
    vanished["confirmation_token"] =
        second.payload["mutation_preview"]["confirmation_token"];
    didi::mcp::TargetProbe gone = [](const didi::json&, didi::json& before,
                                     didi::json& subject) -> std::optional<didi::Error> {
        before = {{"exists", false}, {"path", "res://drift.gd"}};
        subject = {{"path", "res://drift.gd"}};
        return std::nullopt;
    };
    const auto still_spendable = safety.evaluate(binding, vanished, context, gone);
    ASSERT_FALSE(still_spendable.is_error);
    ASSERT_TRUE(still_spendable.execute);
}

// A tool with no probe reads nothing, so there is nothing to compare, and the
// preview says which of the two it is rather than leaving a caller to assume
// the check ran.
TEST(Phase6, APreviewThatReadsNothingSaysTheConfirmCannotCompare) {
    ScopedPhase6Directory directory("unprobed-confirmation");
    didi::mcp::MutationSafety safety;
    auto context = offlineContext(directory.root);
    const didi::json arguments = {{"board", "default"}};
    const auto preview = evaluateBinding(safety, "blackboard_clear", dryRun(arguments), context);
    ASSERT_FALSE(preview.is_error);
    const auto& mutation_preview = preview.payload["mutation_preview"];
    ASSERT_EQ(mutation_preview["target_read"], false);
    ASSERT_EQ(mutation_preview["target_checked_on_confirm"], false);
    // No subject of its own, so the reader is pointed at the one copy of the
    // arguments rather than handed a second.
    ASSERT_TRUE(mutation_preview["changes"][0]["target"].is_string());
}

// Break caught: the '..' rule ran in the bridge alone, so a dry run composed a
// preview for a call the server already knew it would reject and the identical
// call without dry_run came back 400. The rule is a property of the argument,
// so it needs no engine, no open scene and no node to apply -- which is exactly
// why the preview path could run it and did not (#571).
TEST(Phase6, ParentRelativeNodePathsAreRefusedWithoutAnEngine) {
    for (const auto* refused : {"..", "../..", "/root/Main/../Main", "../Main",
                                "/root/..", "Main/../Other"}) {
        const auto problem = didi::paths::refuseParentRelativeNodePath(refused);
        ASSERT_TRUE(problem.has_value());
        ASSERT_EQ(problem->code, 400);
        ASSERT_TRUE(problem->message.find("Parent-relative") != std::string::npos);
    }
    // Segments are compared whole, so a node legitimately named with dots is
    // not a parent reference.
    for (const auto* allowed : {"", ".", "/root", "/root/Main", "/root/Main/Child",
                                "/root/..foo", "/root/x..y", "/root/Main/...."}) {
        ASSERT_FALSE(didi::paths::refuseParentRelativeNodePath(allowed).has_value());
    }
}

// The other lever on a preview, and the one that keeps it deliverable. Every
// string argument carries a declared bound since #573, but a tool that takes a
// list of them can still sum past the response budget, and a preview that
// cannot be delivered is worse than one that says what it left out (#574).
TEST(Phase6, APreviewStaysInsideItsResponseBudget) {
    ScopedPhase6Directory directory("preview-response-budget");
    didi::mcp::MutationSafety safety;
    auto context = offlineContext(directory.root);

    // A list of large paths: the shape a per-argument bound cannot catch.
    didi::json paths = didi::json::array();
    for (int index = 0; index < 40; ++index) {
        paths.push_back("res://" + std::string(300000, 'a') + std::to_string(index) + ".tscn");
    }
    const didi::json arguments = {{"paths", paths}};
    const auto preview =
        evaluateBinding(safety, "project_apply_changes", dryRun(arguments), context);
    ASSERT_FALSE(preview.is_error);
    const auto& mutation_preview = preview.payload["mutation_preview"];

    // Published whether or not it was reached, and reached here.
    ASSERT_EQ(mutation_preview["max_response_bytes"].get<size_t>(), didi::kMaxToolResponseBytes);
    ASSERT_EQ(mutation_preview["truncated"], true);
    ASSERT_TRUE(preview.payload.dump().size() <= didi::kMaxToolResponseBytes);

    // What was dropped says it was dropped, and how big it was. An elision that
    // says nothing is worse than the size it saves, because the preview is the
    // artifact a person approves.
    const auto shown = mutation_preview["arguments"].dump();
    ASSERT_TRUE(shown.find("[elided:") != std::string::npos);
    ASSERT_TRUE(shown.find("bytes") != std::string::npos);

    // An ordinary preview is untouched and says so.
    const didi::json small = {{"paths", didi::json::array({"res://main.tscn"})}};
    const auto ordinary =
        evaluateBinding(safety, "project_apply_changes", dryRun(small), context);
    ASSERT_FALSE(ordinary.is_error);
    const auto& ordinary_preview = ordinary.payload["mutation_preview"];
    ASSERT_EQ(ordinary_preview["truncated"], false);
    ASSERT_EQ(ordinary_preview["max_response_bytes"].get<size_t>(),
              didi::kMaxToolResponseBytes);
    ASSERT_EQ(ordinary_preview["arguments"]["paths"][0], "res://main.tscn");
}
