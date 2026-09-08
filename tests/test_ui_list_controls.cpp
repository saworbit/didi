// ui_list_controls: the standalone contract.
//
// The traversal itself needs a running engine and is verified by the Godot
// integration harness. What is testable here is the half that must never reach
// the engine: argument validation, the exact forwarded request, and the
// registered contract.

#include "didi/mcp/tool_registry.hpp"
#include "didi/mcp/tool_availability.hpp"
#include "didi/common/ipc_channel.hpp"
#include "didi/runtime/session_kind_policy.hpp"

#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

#define ASSERT_TRUE(cond) if (!(cond)) throw std::runtime_error("Assertion failed: " #cond);
#define ASSERT_EQ(a, b) ASSERT_TRUE((a) == (b))
#define ASSERT_FALSE(cond) ASSERT_TRUE(!(cond))

void registerTest(const std::string& name, std::function<void()> fn);

namespace {

using didi::json;

// Records what would have gone to the engine, and answers with the shape the
// bridge really returns so the handler's response check is exercised.
class RecordingControlClient final : public didi::ipc::IIpcClient {
public:
    bool connect(const std::string&, int) override { connected = true; return true; }
    void disconnect() override { connected = false; }
    bool isConnected() const override { return connected; }
    didi::Result<didi::json> sendRequest(const std::string& method, const didi::json& params,
                                         int) override {
        ++calls;
        last_method = method;
        last_params = params;
        if (malformed) return didi::json{{"execution_mode", "live"}};
        return didi::json{{"controls", didi::json::array()},
                          {"returned_count", 0},
                          {"match_count_total", 0},
                          {"execution_mode", "live"},
                          {"is_live_engine", true}};
    }

    bool connected{true};
    bool malformed{false};
    int calls{0};
    std::string last_method;
    didi::json last_params;
};

didi::mcp::ToolRegistry& freshRegistry(std::shared_ptr<RecordingControlClient> client) {
    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();
    registry.setIpcClient(std::move(client));
    return registry;
}

void test_registered_contract() {
    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();
    const auto* tool = registry.getTool("ui_list_controls");
    ASSERT_TRUE(tool != nullptr);

    // Live only. A .tscn holds anchors and offsets, not the rectangle they
    // resolve to, so an offline answer would be a fabricated one.
    ASSERT_EQ(tool->capability.modes, std::vector<std::string>({"live"}));
    ASSERT_TRUE(tool->capability.implemented);

    // A read: no dry run, no confirmation, and it cannot run project code.
    ASSERT_TRUE(tool->annotations.read_only);
    ASSERT_FALSE(tool->annotations.destructive);
    ASSERT_FALSE(tool->annotations.open_world);

    // Nothing is required: the useful default is "show me the controls".
    ASSERT_FALSE(tool->inputSchema.contains("required"));
    const auto& properties = tool->inputSchema["properties"];
    ASSERT_EQ(properties["max_results"]["minimum"], 1);
    ASSERT_EQ(properties["max_results"]["maximum"], 256);
    ASSERT_EQ(properties["max_results"]["default"], 64);
    ASSERT_TRUE(properties["visible_only"]["default"].get<bool>());
    ASSERT_TRUE(properties["include_text"]["default"].get<bool>());
    ASSERT_EQ(properties["class_filter"]["maxItems"], 16);
    ASSERT_EQ(properties["root_path"]["maxLength"], 1024);
}

// Three gates decide whether a game session may call this, and they have to
// agree: the tool policy the standalone checks, the method policy, and the
// editor hook's admission list. Only the hook was updated first, so the tool
// was documented as editor-or-game and refused in a game with a 409 -- found by
// running the fixture as a game rather than as an editor.
void test_a_game_session_is_allowed_by_every_gate() {
    using didi::runtime::LiveSessionKindPolicy;
    ASSERT_TRUE(didi::runtime::livePolicyForTool("ui_list_controls") ==
                LiveSessionKindPolicy::editor_or_game);
    ASSERT_TRUE(didi::runtime::livePolicyForMethod("ui.listControls") ==
                LiveSessionKindPolicy::editor_or_game);

    // The function tools/list and the Control Room both answer from.
    ASSERT_TRUE(didi::mcp::liveAllowedFor("ui_list_controls", false, "game"));
    ASSERT_TRUE(didi::mcp::liveAllowedFor("ui_list_controls", false, "editor"));

    // And the neighbouring tool stays editor-only: it needs an editor viewport,
    // and widening this one must not widen that one.
    ASSERT_FALSE(didi::mcp::liveAllowedFor("ui_hit_test", false, "game"));
    ASSERT_TRUE(didi::mcp::liveAllowedFor("ui_hit_test", false, "editor"));
}

void test_forwards_the_exact_request() {
    auto client = std::make_shared<RecordingControlClient>();
    auto& registry = freshRegistry(client);
    const json arguments = {{"root_path", "/root/Ui"}, {"max_results", 8},
                            {"visible_only", false}, {"include_text", false},
                            {"class_filter", json::array({"Button", "Label"})}};
    const auto result = registry.callTool("ui_list_controls", arguments);
    ASSERT_FALSE(result.isError);
    ASSERT_EQ(client->last_method, "ui.listControls");
    // Verbatim. The bridge validates again, and a handler that reshaped the
    // request would make the two disagree about what was asked.
    ASSERT_EQ(client->last_params, arguments);
    registry.setIpcClient(nullptr);
}

void test_bad_arguments_never_reach_the_engine() {
    auto client = std::make_shared<RecordingControlClient>();
    auto& registry = freshRegistry(client);

    const std::vector<json> rejected = {
        {{"max_results", 0}},
        {{"max_results", 257}},
        {{"max_results", "many"}},
        {{"max_results", 1.5}},
        {{"visible_only", "yes"}},
        {{"include_text", 1}},
        {{"root_path", 42}},
        {{"root_path", std::string(1025, 'a')}},
        {{"class_filter", json::array()}},
        {{"class_filter", "Button"}},
        {{"class_filter", json::array({"Button", ""})}},
        {{"class_filter", json::array({std::string(65, 'c')})}},
    };
    for (const auto& arguments : rejected) {
        const auto result = registry.callTool("ui_list_controls", arguments);
        ASSERT_TRUE(result.isError);
    }
    // Seventeen class names is one too many.
    json too_many = json::array();
    for (int i = 0; i < 17; ++i) too_many.push_back("Button");
    ASSERT_TRUE(registry.callTool("ui_list_controls", {{"class_filter", too_many}}).isError);

    ASSERT_EQ(client->calls, 0);
    registry.setIpcClient(nullptr);
}

void test_disconnected_is_refused_rather_than_faked() {
    auto client = std::make_shared<RecordingControlClient>();
    auto& registry = freshRegistry(client);
    client->connected = false;
    const auto result = registry.callTool("ui_list_controls", json::object());
    ASSERT_TRUE(result.isError);
    ASSERT_EQ(client->calls, 0);
    registry.setIpcClient(nullptr);
}

void test_a_malformed_engine_answer_is_an_error() {
    auto client = std::make_shared<RecordingControlClient>();
    auto& registry = freshRegistry(client);
    client->malformed = true;
    // An engine that answers without a controls array has not answered this
    // question, and reporting zero controls would be a fabricated fact.
    ASSERT_TRUE(registry.callTool("ui_list_controls", json::object()).isError);
    registry.setIpcClient(nullptr);
}

struct Register {
    Register() {
        registerTest("UiListControls.RegisteredContract", test_registered_contract);
        registerTest("UiListControls.GameSessionAllowedByEveryGate",
                     test_a_game_session_is_allowed_by_every_gate);
        registerTest("UiListControls.ForwardsTheExactRequest", test_forwards_the_exact_request);
        registerTest("UiListControls.BadArgumentsNeverReachTheEngine",
                     test_bad_arguments_never_reach_the_engine);
        registerTest("UiListControls.DisconnectedIsRefused",
                     test_disconnected_is_refused_rather_than_faked);
        registerTest("UiListControls.MalformedEngineAnswerIsAnError",
                     test_a_malformed_engine_answer_is_an_error);
    }
} g_registerUiListControlsTests;

}  // namespace
