#include "didi/common/json.hpp"
#include "didi/gdextension/editor_hook.hpp"
#include "didi/mcp/mutation_safety.hpp"
#include "didi/mcp/tool_registry.hpp"
#include "didi/runtime/animation_requests.hpp"
#include "didi/runtime/session_kind_policy.hpp"
#include "didi/tools/resolved_tool_binding.hpp"

#include <functional>
#include <stdexcept>
#include <string>

#define ASSERT_TRUE(cond) if (!(cond)) throw std::runtime_error("Assertion failed: " #cond);
#define ASSERT_EQ(a, b) ASSERT_TRUE((a) == (b))

void registerTest(const std::string& name, std::function<void()> fn);

namespace {

using didi::json;
using didi::runtime::parseAnimAddLibraryRequest;

json base() {
    return {{"animation_player_path", "/root/Menu/Player"},
            {"library_path", "res://animations/menu.tres"}};
}

json with(const char* key, json value) {
    auto copy = base();
    copy[key] = std::move(value);
    return copy;
}

std::string textOf(const didi::mcp::CallToolResult& result) {
    for (const auto& item : result.content) {
        if (item.type == "text") return item.text;
    }
    return {};
}

// Every shape the bridge refuses before it touches the engine. The four name
// characters are the ones Godot refuses with ERR_INVALID_PARAMETER and an
// error line in the editor's log, measured on 4.5.1, 4.6.2 and 4.7.2, so each
// has to be refused here by name rather than there by code.
void request_validation() {
    auto plain = parseAnimAddLibraryRequest(base());
    ASSERT_TRUE(plain.isOk());
    ASSERT_EQ(plain.value().library_name, std::string());
    ASSERT_TRUE(!plain.value().preview);

    auto named = parseAnimAddLibraryRequest(with("library_name", "ui"));
    ASSERT_TRUE(named.isOk());
    ASSERT_EQ(named.value().library_name, std::string("ui"));
    ASSERT_TRUE(parseAnimAddLibraryRequest(with("library_path", "res://menu.res")).isOk());
    // Accepted by the engine on all three lines, so accepted here: a space,
    // a dot, a closing bracket and non-ASCII text are all ordinary names.
    for (const std::string name : {std::string("a b"), std::string("a.b"), std::string("a]b"),
                                   std::string("\xC3\xA9"), std::string(256, 'n')}) {
        ASSERT_TRUE(parseAnimAddLibraryRequest(with("library_name", name)).isOk());
    }
    // The server's dry-run probe sets this; nothing else can reach it.
    ASSERT_TRUE(parseAnimAddLibraryRequest(with("preview", true)).value().preview);
    // Off unless asked for: reloading discards changes made to the editor's
    // copy and not saved.
    ASSERT_TRUE(!plain.value().reload_from_disk);
    ASSERT_TRUE(parseAnimAddLibraryRequest(with("reload_from_disk", true)).value().reload_from_disk);

    for (const auto& bad : {
             json::object(),
             json{{"animation_player_path", "/root/Menu/Player"}},
             json{{"library_path", "res://menu.tres"}},
             with("animation_player_path", ""),
             with("animation_player_path", std::string(1025, 'p')),
             with("animation_player_path", 7),
             with("library_path", ""),
             with("library_path", "animations/menu.tres"),
             with("library_path", "C:/animations/menu.tres"),
             with("library_path", "res://menu.anim"),
             with("library_path", "res://menu.tscn"),
             with("library_path", "res://" + std::string(1020, 'l') + ".tres"),
             with("library_name", 3),
             with("library_name", std::string(257, 'n')),
             with("library_name", std::string("a\0b", 3)),
             with("preview", "yes"),
             with("reload_from_disk", 1),
             with("reload_from_disk", "true"),
             with("replace", true),
             with("dry_run", false)}) {
        auto parsed = parseAnimAddLibraryRequest(bad);
        ASSERT_TRUE(parsed.isErr());
        ASSERT_EQ(parsed.error().code, 400);
    }
    for (const auto* name : {"a/b", "a:b", "a,b", "a[b"}) {
        auto parsed = parseAnimAddLibraryRequest(with("library_name", name));
        ASSERT_TRUE(parsed.isErr());
        ASSERT_EQ(parsed.error().code, 400);
        // Which character, so the caller does not have to guess among four.
        const std::string refused(1, name[1]);
        ASSERT_TRUE(parsed.error().message.find("'" + refused + "'") != std::string::npos);
    }
}

// Live only, editor only, an undoable mutation that adds and never replaces.
// Being wrong about any of these misleads a host: readOnlyHint decides what
// runs unasked, destructiveHint what needs a person, and idempotentHint what
// is retried after a timeout, and a retry here answers 409.
void registration() {
    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();
    const auto* tool = registry.getTool("anim_add_library");
    ASSERT_TRUE(tool != nullptr);
    ASSERT_TRUE(tool->capability.implemented);
    const auto published = tool->toJson();
    ASSERT_EQ(published["_meta"]["didi"]["executionModes"], json::array({"live"}));
    ASSERT_EQ(published["title"], json("Add an animation library"));
    ASSERT_EQ(published["annotations"]["readOnlyHint"], json(false));
    ASSERT_EQ(published["annotations"]["destructiveHint"], json(false));
    ASSERT_EQ(published["annotations"]["idempotentHint"], json(false));
    ASSERT_EQ(published["annotations"]["openWorldHint"], json(false));

    const auto& schema = tool->inputSchema;
    ASSERT_EQ(schema["required"], json::array({"animation_player_path", "library_path"}));
    ASSERT_EQ(schema["additionalProperties"], json(false));
    ASSERT_TRUE(schema["properties"].contains("dry_run"));
    ASSERT_TRUE(!schema["properties"].contains("confirmation_token"));
    ASSERT_TRUE(!schema["properties"].contains("preview"));
    ASSERT_EQ(schema["properties"]["reload_from_disk"]["type"], json("boolean"));
    ASSERT_EQ(schema["properties"]["reload_from_disk"]["default"], json(false));
    for (const auto* parameter : {"animation_player_path", "library_path", "library_name",
                                  "reload_from_disk"}) {
        ASSERT_TRUE(schema["properties"][parameter].contains("description"));
    }

    didi::mcp::ResolvedToolBinding binding;
    binding.canonical_name = "anim_add_library";
    binding.policy_source = "anim_add_library";
    ASSERT_TRUE(didi::mcp::MutationSafety::isMutation(binding));
    ASSERT_TRUE(!didi::mcp::MutationSafety::requiresConfirmation(binding, base()));
    ASSERT_TRUE(didi::runtime::livePolicyForTool("anim_add_library") ==
                didi::runtime::LiveSessionKindPolicy::editor_only);
    ASSERT_TRUE(didi::runtime::livePolicyForMethod("anim.addLibrary") ==
                didi::runtime::LiveSessionKindPolicy::editor_only);
}

// With no editor there is nothing to add a library to. The call is refused
// rather than answered, and the dry run is the safety envelope's own preview:
// it reaches no engine, so it must say it read nothing rather than describe a
// plan, and an additive undoable change asks for no token.
void no_editor_no_call() {
    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();
    registry.setIpcClient(nullptr);
    const auto refused_call = registry.callTool("anim_add_library", base());
    ASSERT_TRUE(refused_call.isError);
    ASSERT_TRUE(textOf(refused_call).find("\"status\":\"success\"") == std::string::npos);

    const auto preview = registry.callTool("anim_add_library", with("dry_run", true));
    ASSERT_TRUE(!preview.isError);
    const auto payload = json::parse(textOf(preview));
    ASSERT_EQ(payload["mutation_preview"]["tool"], json("anim_add_library"));
    ASSERT_EQ(payload["mutation_preview"]["target_read"], json(false));
    ASSERT_EQ(payload["mutation_preview"]["changes"][0]["kind"], json("unverified_mutation"));
    ASSERT_EQ(payload["mutation_preview"]["requires_confirmation"], json(false));
    ASSERT_TRUE(!payload["mutation_preview"].contains("confirmation_token") ||
                payload["mutation_preview"]["confirmation_token"].is_null());

    // A game session is refused at the hook, before the bridge: a library
    // added to a running game is gone when it stops.
    auto& hook = didi::godot::EditorHook::instance();
    hook.cancelPendingCommands("test reset");
    didi::godot::EditorHookTestAccess::setSessionKind(hook, didi::runtime::SessionKind::game);
    auto refused = didi::godot::EditorHookTestAccess::executeOnMainThread(hook, "anim.addLibrary", base());
    didi::godot::EditorHookTestAccess::setSessionKind(hook, std::nullopt);
    ASSERT_TRUE(refused.contains("error"));
    ASSERT_EQ(refused["error"]["code"], json(409));
    ASSERT_EQ(refused["error"]["data"]["code"], json("session_kind_rejected"));
}

struct Register {
    Register() {
        registerTest("AnimAddLibrary.RequestValidation", request_validation);
        registerTest("AnimAddLibrary.Registration", registration);
        registerTest("AnimAddLibrary.NoEditorNoCall", no_editor_no_call);
    }
} registrar;

} // namespace
