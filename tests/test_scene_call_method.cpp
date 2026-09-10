#include "didi/common/json.hpp"
#include "didi/common/types.hpp"
#include "didi/mcp/mutation_safety.hpp"
#include "didi/mcp/tool_registry.hpp"
#include "didi/tools/resolved_tool_binding.hpp"

#include <functional>
#include <stdexcept>
#include <string>

#define ASSERT_TRUE(cond) if (!(cond)) throw std::runtime_error("Assertion failed: " #cond);
#define ASSERT_EQ(a, b) ASSERT_TRUE((a) == (b))
#define ASSERT_FALSE(cond) ASSERT_TRUE(!(cond))

void registerTest(const std::string& name, std::function<void()> fn);

namespace didi {
namespace mcp {
CallToolResult handleSceneCallMethod(const json& args, std::shared_ptr<ipc::IIpcClient> ipc);
}
}

namespace {

using didi::json;

// No engine, so every one of these is refused before a request is built. That
// is the point: the offline path must never look like a call that happened.
didi::mcp::CallToolResult call(const json& args) {
    return didi::mcp::handleSceneCallMethod(args, nullptr);
}

std::string textOf(const didi::mcp::CallToolResult& result) {
    for (const auto& item : result.content) {
        if (item.type == "text") return item.text;
    }
    return {};
}

json baseArguments() {
    return {{"target_node", "/root/LevelRoot"}, {"method_name", "bake"}};
}

// Offline the tool refuses and says why, rather than pretending a method ran.
// A method body is project code in the editor's process; there is nothing to
// run without one.
void refuses_offline_and_names_the_reason() {
    const auto result = call(baseArguments());
    ASSERT_TRUE(result.isError);
    const auto text = textOf(result);
    ASSERT_TRUE(text.find("offline") != std::string::npos);
    ASSERT_TRUE(text.find("no offline meaning") != std::string::npos);
}

// Break caught: the underscore rule living only in the bridge. It has to refuse
// here too, or a caller with no editor attached learns the rule a request later
// than it could have.
void refuses_a_leading_underscore_before_reaching_an_engine() {
    auto args = baseArguments();
    args["method_name"] = "_ready";
    const auto result = call(args);
    ASSERT_TRUE(result.isError);
    const auto text = textOf(result);
    ASSERT_TRUE(text.find("_ready") != std::string::npos);
    ASSERT_TRUE(text.find("underscore") != std::string::npos);
    // It must not read as "the editor is offline": that would send the caller
    // to start Godot for a request that will be refused either way.
    ASSERT_TRUE(text.find("Launch Godot") == std::string::npos);
}

void refuses_a_missing_or_oversized_target() {
    ASSERT_TRUE(call({{"method_name", "bake"}}).isError);
    ASSERT_TRUE(call({{"target_node", ""}, {"method_name", "bake"}}).isError);
    ASSERT_TRUE(call({{"target_node", std::string(1025, 'a')}, {"method_name", "bake"}}).isError);
    ASSERT_TRUE(call({{"target_node", "/root/A"}}).isError);
    ASSERT_TRUE(call({{"target_node", "/root/A"}, {"method_name", ""}}).isError);
    ASSERT_TRUE(call({{"target_node", "/root/A"}, {"method_name", std::string(129, 'b')}}).isError);
}

void refuses_arguments_it_cannot_pass() {
    auto args = baseArguments();
    args["arguments"] = "not an array";
    ASSERT_TRUE(call(args).isError);

    args["arguments"] = json::array();
    for (int index = 0; index < 9; ++index) args["arguments"].push_back(index);
    const auto too_many = call(args);
    ASSERT_TRUE(too_many.isError);
    ASSERT_TRUE(textOf(too_many).find("at most 8") != std::string::npos);

    // Eight arguments is the limit, not one past it, and the refusal that
    // follows must be about the editor rather than about the count.
    args["arguments"] = json::array();
    for (int index = 0; index < 8; ++index) args["arguments"].push_back(index);
    ASSERT_TRUE(textOf(call(args)).find("at most 8") == std::string::npos);

    args["arguments"] = json::array({std::string(9 * 1024, 'x')});
    const auto too_large = call(args);
    ASSERT_TRUE(too_large.isError);
    ASSERT_TRUE(textOf(too_large).find("8 KiB") != std::string::npos);
}

void refuses_a_timeout_outside_its_range() {
    for (const auto value : {0, 121, -1}) {
        auto args = baseArguments();
        args["timeout_seconds"] = value;
        const auto result = call(args);
        ASSERT_TRUE(result.isError);
        ASSERT_TRUE(textOf(result).find("timeout_seconds") != std::string::npos);
    }
    auto args = baseArguments();
    args["timeout_seconds"] = 120;
    ASSERT_TRUE(textOf(call(args)).find("timeout_seconds") == std::string::npos);
}

// It runs a method body nobody here can read, so it is a mutation and it always
// wants a confirmation token. Being wrong about either would let an agent run
// project code on a dry run, or without meaning to.
void is_a_confirmed_mutation() {
    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();
    const auto* definition = registry.getTool("scene_call_method");
    ASSERT_TRUE(definition != nullptr);

    didi::mcp::ResolvedToolBinding binding;
    binding.canonical_name = "scene_call_method";
    binding.policy_source = "scene_call_method";
    ASSERT_TRUE(didi::mcp::MutationSafety::isMutation(binding));
    ASSERT_TRUE(didi::mcp::MutationSafety::canRequireConfirmation(binding));
    // Always, not on a flag: an ordinary call with no opt-in still asks.
    ASSERT_TRUE(didi::mcp::MutationSafety::requiresConfirmation(binding, baseArguments()));
}

struct Register {
    Register() {
        registerTest("SceneCallMethod.OfflineRefusal", refuses_offline_and_names_the_reason);
        registerTest("SceneCallMethod.UnderscoreRefusedEarly",
                     refuses_a_leading_underscore_before_reaching_an_engine);
        registerTest("SceneCallMethod.TargetBounds", refuses_a_missing_or_oversized_target);
        registerTest("SceneCallMethod.ArgumentBounds", refuses_arguments_it_cannot_pass);
        registerTest("SceneCallMethod.TimeoutBounds", refuses_a_timeout_outside_its_range);
        registerTest("SceneCallMethod.ConfirmedMutation", is_a_confirmed_mutation);
    }
} registrar;

} // namespace
