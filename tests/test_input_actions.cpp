#include "didi/mcp/tool_registry.hpp"
#include "didi/offline/input_actions.hpp"
#include "didi/offline/resource_indexer.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <stdexcept>
#include <string>

#define ASSERT_TRUE(cond) if (!(cond)) throw std::runtime_error("Assertion failed: " #cond);
#define ASSERT_EQ(a, b) ASSERT_TRUE((a) == (b))

void registerTest(const std::string& name, std::function<void()> fn);

namespace {

using didi::json;
using didi::offline::parseInputEventDescriptor;

class ScopedInputProject final {
public:
    ScopedInputProject()
        : m_original(std::filesystem::current_path()),
          m_root(std::filesystem::temp_directory_path() /
                 ("didi-input-actions-" +
                  std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))) {
        std::filesystem::create_directories(m_root);
        std::filesystem::current_path(m_root);
        std::ofstream("project.godot", std::ios::binary)
            << "config_version=5\n\n[application]\n\nconfig/name=\"input\"\n";
        didi::offline::ResourceIndexer::invalidateSharedIndex();
    }
    ~ScopedInputProject() {
        std::error_code error;
        std::filesystem::current_path(m_original, error);
        std::filesystem::remove_all(m_root, error);
    }
    std::string manifest() const {
        std::ifstream input(m_root / "project.godot", std::ios::binary);
        std::ostringstream buffer;
        buffer << input.rdbuf();
        return buffer.str();
    }

private:
    std::filesystem::path m_original;
    std::filesystem::path m_root;
};

std::string refusal(const json& descriptor) {
    auto parsed = parseInputEventDescriptor(descriptor);
    ASSERT_TRUE(parsed.isErr());
    ASSERT_EQ(parsed.error().code, 400);
    return parsed.error().message;
}

json payloadOf(const didi::mcp::CallToolResult& result) {
    return json::parse(result.content[0].text);
}

// The rules the bridge has always applied to an event, now in one place so the
// offline route cannot take what the live one refuses (#1379).
void descriptor_rules() {
    auto key = parseInputEventDescriptor({{"type", "key"}, {"physical_keycode", 87}});
    ASSERT_TRUE(key.isOk());
    ASSERT_EQ(key.value().class_name, "InputEventKey");
    ASSERT_EQ(key.value().device, -1);
    ASSERT_EQ(*key.value().physical_keycode, 87);
    ASSERT_TRUE(!key.value().keycode.has_value());
    auto aliased = parseInputEventDescriptor({{"type", "key"}, {"keycode", 4194305}, {"shift", true}, {"device", 2}});
    ASSERT_TRUE(aliased.isOk());
    ASSERT_EQ(*aliased.value().shift_pressed, true);
    ASSERT_EQ(aliased.value().device, 2);
    ASSERT_TRUE(!aliased.value().alt_pressed.has_value());
    auto motion = parseInputEventDescriptor({{"type", "joypad_motion"}, {"axis", 1}, {"axis_value", -1.0}});
    ASSERT_TRUE(motion.isOk());
    ASSERT_EQ(motion.value().class_name, "InputEventJoypadMotion");

    ASSERT_TRUE(refusal({{"type", "key"}, {"physical_keycode", 87}, {"position", 1}}).find(
                    "Unknown property 'position'") != std::string::npos);
    ASSERT_TRUE(refusal({{"type", "touch"}}).find("Unsupported input event type: touch") == 0);
    ASSERT_TRUE(refusal({{"type", "key"}}).find("requires keycode") != std::string::npos);
    ASSERT_EQ(refusal({{"type", "key"}, {"keycode", 0}}), "keycode must be a positive integer");
    ASSERT_TRUE(refusal({{"type", "key"}, {"keycode", 65}, {"shift", true}, {"shift_pressed", false}})
                    .find("same modifier") != std::string::npos);
    ASSERT_EQ(refusal({{"type", "key"}, {"keycode", 65}, {"device", -2}}),
              "Input event device must be an integer >= -1");
    ASSERT_EQ(refusal({{"type", "mouse_button"}, {"button_index", 10}}),
              "button_index is outside the supported range");
    ASSERT_EQ(refusal({{"type", "joypad_button"}}), "button_index is required and must be an integer");
    ASSERT_EQ(refusal({{"type", "joypad_motion"}, {"axis", 10}, {"axis_value", 0.5}}),
              "axis is required and must be in 0..9");
    ASSERT_EQ(refusal({{"type", "joypad_motion"}, {"axis", 1}, {"axis_value", 2.0}}),
              "axis_value is required and must be finite within -1.0..1.0");
    ASSERT_EQ(refusal("key"), "Each input event must be an object with a string type");
    ASSERT_TRUE(didi::offline::validateInputActionName("jump").isOk());
    ASSERT_TRUE(didi::offline::validateInputActionName("").isErr());
    ASSERT_TRUE(didi::offline::validateInputActionName("two words").isErr());
}

// The text the offline route writes. Loaded by 4.5.1, 4.6.2 and 4.7.2 and then
// saved by the engine, this action's [input] text is byte for byte what the
// live route saves for the same events.
void action_literal() {
    std::vector<didi::offline::InputEventSpec> events;
    for (const auto& descriptor : {
             json{{"type", "key"}, {"physical_keycode", 87}},
             json{{"type", "key"}, {"keycode", 4194305}, {"shift_pressed", true}, {"device", 2}},
             json{{"type", "key"}, {"unicode", 97}, {"alt", false}},
             json{{"type", "mouse_button"}, {"button_index", 1}},
             json{{"type", "joypad_button"}, {"button_index", 0}},
             json{{"type", "joypad_motion"}, {"axis", 1}, {"axis_value", -1.0}}}) {
        events.push_back(parseInputEventDescriptor(descriptor).value());
    }
    ASSERT_EQ(didi::offline::inputActionLiteral(0.5, events),
              "{\"deadzone\": 0.5, \"events\": ["
              "Object(InputEventKey,\"device\":-1,\"physical_keycode\":87), "
              "Object(InputEventKey,\"device\":2,\"shift_pressed\":true,\"keycode\":4194305), "
              "Object(InputEventKey,\"device\":-1,\"alt_pressed\":false,\"unicode\":97), "
              "Object(InputEventMouseButton,\"device\":-1,\"button_index\":1), "
              "Object(InputEventJoypadButton,\"device\":-1,\"button_index\":0), "
              "Object(InputEventJoypadMotion,\"device\":-1,\"axis\":1,\"axis_value\":-1.0)]}");
    ASSERT_EQ(didi::offline::inputActionLiteral(0.2, {}), "{\"deadzone\": 0.2, \"events\": []}");
}

// Break caught: with no editor attached the tool answered 503, so a new
// project's input map could not be written before anything opened it, though
// project_set_setting writes the same file offline (#1379).
void offline_write() {
    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();
    registry.setIpcClient(nullptr);
    ScopedInputProject project;
    const json call = {{"action", "thrust_main"},
                       {"deadzone", 0.5},
                       {"events", json::array({{{"type", "key"}, {"physical_keycode", 87}}})}};

    const auto preview = registry.callTool("project_set_input_action",
                                           {{"action", "thrust_main"}, {"events", json::array()}, {"dry_run", true}});
    ASSERT_TRUE(!preview.isError);
    ASSERT_TRUE(project.manifest().find("[input]") == std::string::npos);

    const auto first = registry.callTool("project_set_input_action", call);
    ASSERT_TRUE(!first.isError);
    const auto written = payloadOf(first);
    ASSERT_EQ(written["execution_mode"], "offline_fallback");
    ASSERT_EQ(written["persisted"], true);
    ASSERT_EQ(written["defined_by_project"], true);
    ASSERT_EQ(written["replaced_existing"], false);
    ASSERT_EQ(written["event_count"], 1);
    const std::string literal =
        "{\"deadzone\": 0.5, \"events\": [Object(InputEventKey,\"device\":-1,\"physical_keycode\":87)]}";
    ASSERT_EQ(written["value_written"], literal);
    ASSERT_TRUE(project.manifest().find("[input]\n\nthrust_main=" + literal) != std::string::npos);
    ASSERT_TRUE(project.manifest().find("config/name=\"input\"") != std::string::npos);

    // It is there now, so a second call needs replace, as the live one does.
    const auto again = registry.callTool("project_set_input_action", call);
    ASSERT_TRUE(again.isError);
    ASSERT_EQ(payloadOf(again)["error"]["data"]["code"], "already_exists");
    ASSERT_EQ(payloadOf(again)["error"]["data"]["retry_with"]["replace"], true);
    auto replaced_call = call;
    replaced_call["replace"] = true;
    replaced_call["events"] = json::array({{{"type", "joypad_button"}, {"button_index", 0}}});
    const auto replaced = registry.callTool("project_set_input_action", replaced_call);
    ASSERT_TRUE(!replaced.isError);
    ASSERT_EQ(payloadOf(replaced)["replaced_existing"], true);
    ASSERT_EQ(payloadOf(replaced)["previous_value"], literal);
    ASSERT_TRUE(project.manifest().find("InputEventJoypadButton") != std::string::npos);
    ASSERT_TRUE(project.manifest().find("physical_keycode") == std::string::npos);

    // An engine's own ui_* action counts as there, as it does live.
    const auto engine_default = registry.callTool(
        "project_set_input_action", {{"action", "ui_accept"}, {"events", json::array({{{"type", "key"}, {"keycode", 32}}})}});
    ASSERT_TRUE(engine_default.isError);
    ASSERT_EQ(payloadOf(engine_default)["error"]["data"]["engine_default"], true);

    // A bad event names its entry and writes nothing.
    const auto before = project.manifest();
    const auto bad = registry.callTool(
        "project_set_input_action",
        {{"action", "fire"}, {"events", json::array({{{"type", "key"}, {"keycode", 65}},
                                                     {{"type", "key"}, {"keycode", 66}, {"shift", true},
                                                      {"shift_pressed", false}}})}});
    ASSERT_TRUE(bad.isError);
    ASSERT_TRUE(payloadOf(bad)["error"]["message"].get<std::string>().find(
                    "Argument 'events' entry 1: Input event sets 'shift_pressed' and 'shift'") == 0);
    ASSERT_EQ(project.manifest(), before);
}

struct Register {
    Register() {
        registerTest("InputActions.DescriptorRules", descriptor_rules);
        registerTest("InputActions.ActionLiteral", action_literal);
        registerTest("InputActions.OfflineWrite", offline_write);
    }
} registrar;

} // namespace
