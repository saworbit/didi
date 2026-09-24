#include "didi/common/json.hpp"
#include "didi/gdextension/editor_hook.hpp"
#include "didi/mcp/mutation_safety.hpp"
#include "didi/mcp/tool_registry.hpp"
#include "didi/runtime/audio_requests.hpp"
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
using didi::runtime::parseAudioAddBusRequest;

json with(const char* key, json value) {
    json copy = {{"name", "Music"}};
    copy[key] = std::move(value);
    return copy;
}

std::string textOf(const didi::mcp::CallToolResult& result) {
    for (const auto& item : result.content) {
        if (item.type == "text") return item.text;
    }
    return {};
}

// AudioServer.set_bus_name never refuses a name. It kept every one of these on
// 4.5.1, 4.6.2 and 4.7.2 (tools/vibe/probes/audio_bus_engine.py), so each rule
// here is Didi's, and each is there because the name it refuses is one a person
// cannot tell apart from another in the Audio panel.
void request_validation() {
    auto plain = parseAudioAddBusRequest({{"name", "Music"}});
    ASSERT_TRUE(plain.isOk());
    ASSERT_EQ(plain.value().name, std::string("Music"));
    // The engine gives a new bus an empty send, which routes as Master does.
    // This spells it.
    ASSERT_EQ(plain.value().send, std::string("Master"));
    ASSERT_TRUE(!plain.value().volume_db && !plain.value().mute && !plain.value().solo);
    ASSERT_TRUE(!plain.value().preview);

    auto full = parseAudioAddBusRequest(
        {{"name", "SFX"}, {"send", "Music"}, {"volume_db", -6.0}, {"mute", true}, {"solo", false}});
    ASSERT_TRUE(full.isOk());
    ASSERT_EQ(full.value().send, std::string("Music"));
    ASSERT_TRUE(full.value().volume_db && *full.value().volume_db == -6.0);
    ASSERT_TRUE(full.value().mute && *full.value().mute);
    ASSERT_TRUE(full.value().solo && !*full.value().solo);
    // The engine keeps and saves all of these, and Didi reads them back since
    // #934, so they are ordinary names.
    for (const std::string name :
         {std::string("Music And Voice"), std::string("Say \"hi\""), std::string("back\\slash"),
          std::string("a/b"), std::string("[bus]"), std::string("&\"x\""),
          std::string("\xC3\x9Cn\xC3\xAF"), std::string("123"), std::string(256, 'x')}) {
        ASSERT_TRUE(parseAudioAddBusRequest({{"name", name}}).isOk());
    }
    // Set by the server's dry-run probe; the published schema does not carry it.
    ASSERT_TRUE(parseAudioAddBusRequest(with("preview", true)).value().preview);

    for (const auto& bad : {
             json::object(),
             json{{"name", 7}},
             json{{"name", ""}},
             json{{"name", "   "}},
             json{{"name", std::string(257, 'x')}},
             json{{"name", "tab\there"}},
             json{{"name", "new\nline"}},
             json{{"name", std::string("a\0b", 3)}},
             json{{"name", "a\x7f"}},
             with("send", ""),
             with("send", 1),
             with("send", std::string(257, 's')),
             with("volume_db", "loud"),
             with("volume_db", -80.5),
             with("volume_db", 24.5),
             with("mute", 1),
             with("solo", "true"),
             with("preview", "yes"),
             with("position", 1),
             with("dry_run", true)}) {
        auto parsed = parseAudioAddBusRequest(bad);
        ASSERT_TRUE(parsed.isErr());
        ASSERT_EQ(parsed.error().code, 400);
        // Which argument, because data.parameter is what a caller branches on.
        ASSERT_TRUE(parsed.error().data.contains("parameter"));
    }
    ASSERT_TRUE(parseAudioAddBusRequest(with("volume_db", -80)).isOk());
    ASSERT_TRUE(parseAudioAddBusRequest(with("volume_db", 24)).isOk());

    // A leading or trailing space is refused with the name it meant.
    for (const auto* spaced : {" Music", "Music ", "  Music  "}) {
        auto parsed = parseAudioAddBusRequest({{"name", spaced}});
        ASSERT_TRUE(parsed.isErr());
        ASSERT_EQ(parsed.error().data["parameter"], json("name"));
        ASSERT_EQ(parsed.error().data["retry_with"]["name"], json("Music"));
    }
    // The empty send routes as Master does, so it is refused toward Master.
    auto empty_send = parseAudioAddBusRequest(with("send", ""));
    ASSERT_EQ(empty_send.error().data["retry_with"]["send"], json("Master"));
}

// Vibe session nineteen sent audio_add_bus what copy and paste delivers, and
// the ASCII rules let most of it through: a no-break space and an ideographic
// space at the ends, a zero width space alone and in front of "Music", U+0085
// and U+2028, and a byte-order mark the engine's UTF-8 reader then dropped. The
// Audio panel shows every one of those as a name it cannot be told apart from,
// or as no name at all.
void names_as_a_person_sees_them() {
    const auto refusedWith = [](const std::string& name) {
        auto parsed = parseAudioAddBusRequest({{"name", name}});
        ASSERT_TRUE(parsed.isErr());
        ASSERT_EQ(parsed.error().code, 400);
        ASSERT_EQ(parsed.error().data["parameter"], json("name"));
        return parsed.error();
    };
    const std::string nbsp = "\xC2\xA0";
    const std::string ideographic_space = "\xE3\x80\x80";
    const std::string zero_width_space = "\xE2\x80\x8B";
    const std::string byte_order_mark = "\xEF\xBB\xBF";

    // At either end, refused with the name it meant and the character named,
    // because the character is the one thing the caller cannot see.
    const struct { std::string name; std::string meant; const char* character; } edges[] = {
        {nbsp + "Voice", "Voice", "U+00A0"},
        {"Voice" + ideographic_space, "Voice", "U+3000"},
        {zero_width_space + "Music", "Music", "U+200B"},
        {byte_order_mark + "Music", "Music", "U+FEFF"},
        {"Music" + nbsp + " ", "Music", "U+0020"},
    };
    for (const auto& edge : edges) {
        const auto error = refusedWith(edge.name);
        ASSERT_EQ(error.data["retry_with"]["name"], json(edge.meant));
        ASSERT_EQ(error.data["character"], json(edge.character));
    }
    // Nothing but blank, which the panel shows as a bus with no name.
    for (const auto& blank : {zero_width_space, nbsp + ideographic_space, byte_order_mark + " "}) {
        const auto error = refusedWith(blank);
        ASSERT_TRUE(!error.data.contains("retry_with"));
    }
    // Controls anywhere: C1, the line and paragraph separators, and a
    // right-to-left override that redraws the rest of the name backwards.
    for (const auto& control : {std::string("Voice\xC2\x85"), std::string("Vo\xE2\x80\xA8ice"),
                                std::string("Voice\xE2\x80\xA9"), std::string("a\xE2\x80\xAE" "cisuM")}) {
        const auto error = refusedWith(control);
        ASSERT_TRUE(error.message.find("control character") != std::string::npos);
        ASSERT_TRUE(error.data.contains("character"));
    }
    // Still ordinary: a no-break space inside a name, an emoji ending in its
    // variation selector, and a family emoji held together by zero width
    // joiners. Only the ends and nothing-but-blank are refused.
    for (const std::string name : {"Sound" + nbsp + "FX", std::string("Music \xE2\x9D\xA4\xEF\xB8\x8F"),
                                   std::string("\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA7")}) {
        ASSERT_TRUE(parseAudioAddBusRequest({{"name", name}}).isOk());
    }

    // The bound is in characters, the unit the schema's maxLength counts in.
    // 86 of U+97F3 is 258 bytes and was refused as over a 256-byte bound.
    std::string wide;
    for (int index = 0; index < 256; ++index) wide += "\xE9\x9F\xB3";
    ASSERT_TRUE(parseAudioAddBusRequest({{"name", wide}}).isOk());
    ASSERT_TRUE(parseAudioAddBusRequest({{"name", "Music"}, {"send", wide}}).isOk());
    const auto too_wide = refusedWith(wide + "\xE9\x9F\xB3");
    ASSERT_TRUE(too_wide.message.find("256 characters") != std::string::npos);
}

// The engine named the bus differently from the name it was given. This said
// "another bus took the name meanwhile" with retryable: true for a leading
// byte-order mark the engine had dropped, a name no bus held, so every retry
// failed the same way. Which case it is decides both the words and the code.
void name_changed_by_the_engine() {
    const auto raced = didi::runtime::busNameChangedRefusal("Music", "Music 2", true);
    ASSERT_EQ(raced.code, 409);
    ASSERT_EQ(raced.data["code"], json("bus_name_in_use"));
    ASSERT_EQ(raced.data["retryable"], json(false));
    ASSERT_TRUE(raced.message.find("audio_configure_bus") != std::string::npos);

    const auto changed = didi::runtime::busNameChangedRefusal("\xEF\xBB\xBF" "Ambience", "Ambience", false);
    ASSERT_EQ(changed.code, 409);
    ASSERT_EQ(changed.data["code"], json("bus_name_changed_by_engine"));
    ASSERT_EQ(changed.data["stored_as"], json("Ambience"));
    ASSERT_EQ(changed.data["retryable"], json(false));
    ASSERT_TRUE(changed.message.find("meanwhile") == std::string::npos);
}

// Live only, editor only, a mutation that adds and never replaces, with a dry
// run and no token. Being wrong about any of these misleads a host:
// readOnlyHint decides what runs unasked, destructiveHint what needs a person,
// and idempotentHint what is retried after a timeout, and a retry here is a 409.
void registration() {
    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();
    const auto* tool = registry.getTool("audio_add_bus");
    ASSERT_TRUE(tool != nullptr);
    ASSERT_TRUE(tool->capability.implemented);
    const auto published = tool->toJson();
    ASSERT_EQ(published["_meta"]["didi"]["executionModes"], json::array({"live"}));
    ASSERT_EQ(published["title"], json("Add an audio bus"));
    ASSERT_EQ(published["annotations"]["readOnlyHint"], json(false));
    ASSERT_EQ(published["annotations"]["destructiveHint"], json(false));
    ASSERT_EQ(published["annotations"]["idempotentHint"], json(false));
    ASSERT_EQ(published["annotations"]["openWorldHint"], json(false));

    const auto& schema = tool->inputSchema;
    ASSERT_EQ(schema["required"], json::array({"name"}));
    ASSERT_EQ(schema["additionalProperties"], json(false));
    ASSERT_TRUE(schema["properties"].contains("dry_run"));
    ASSERT_TRUE(!schema["properties"].contains("confirmation_token"));
    ASSERT_TRUE(!schema["properties"].contains("preview"));
    // No position: the bus goes on the end, which keeps every send valid.
    ASSERT_TRUE(!schema["properties"].contains("position"));
    ASSERT_EQ(schema["properties"]["send"]["default"], json("Master"));
    ASSERT_EQ(schema["properties"]["volume_db"]["minimum"], json(-80));
    ASSERT_EQ(schema["properties"]["volume_db"]["maximum"], json(24));
    for (const auto* parameter : {"name", "send", "volume_db", "mute", "solo"}) {
        ASSERT_TRUE(schema["properties"][parameter].contains("description"));
    }

    didi::mcp::ResolvedToolBinding binding;
    binding.canonical_name = "audio_add_bus";
    binding.policy_source = "audio_add_bus";
    ASSERT_TRUE(didi::mcp::MutationSafety::isMutation(binding));
    ASSERT_TRUE(!didi::mcp::MutationSafety::requiresConfirmation(binding, json{{"name", "Music"}}));
    ASSERT_TRUE(didi::runtime::livePolicyForTool("audio_add_bus") ==
                didi::runtime::LiveSessionKindPolicy::editor_only);
    ASSERT_TRUE(didi::runtime::livePolicyForMethod("audio.addBus") ==
                didi::runtime::LiveSessionKindPolicy::editor_only);
}

// With no editor there is no layout to add to. The call says where to go, a
// bad name is refused before anything else, and a game session is refused at
// the hook: a bus added to a running game is in no file and gone when it stops.
void gated() {
    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();
    registry.setIpcClient(nullptr);

    const auto offline = registry.callTool("audio_add_bus", {{"name", "Music"}});
    ASSERT_TRUE(offline.isError);
    ASSERT_TRUE(textOf(offline).find("audio_list_buses") != std::string::npos);
    ASSERT_TRUE(textOf(offline).find("\"status\":\"success\"") == std::string::npos);

    // The name rules need no engine, so they answer first and say which.
    const auto bad_name = registry.callTool("audio_add_bus", {{"name", "Music "}});
    ASSERT_TRUE(bad_name.isError);
    ASSERT_TRUE(textOf(bad_name).find("space") != std::string::npos);

    auto& hook = didi::godot::EditorHook::instance();
    hook.cancelPendingCommands("test reset");
    didi::godot::EditorHookTestAccess::setSessionKind(hook, didi::runtime::SessionKind::game);
    auto refused = didi::godot::EditorHookTestAccess::executeOnMainThread(
        hook, "audio.addBus", json{{"name", "Music"}});
    didi::godot::EditorHookTestAccess::setSessionKind(hook, std::nullopt);
    ASSERT_TRUE(refused.contains("error"));
    ASSERT_EQ(refused["error"]["code"], json(409));
    ASSERT_EQ(refused["error"]["data"]["code"], json("session_kind_rejected"));
}

// The dry run is the safety envelope's own preview. With no editor it reaches
// no engine, so it must say it read nothing rather than describe a plan; it
// still refuses a name the call would refuse, because a preview of a call that
// cannot succeed is a plan nobody can follow.
void dry_run_reaches_no_engine() {
    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();
    registry.setIpcClient(nullptr);

    const auto preview = registry.callTool("audio_add_bus", {{"name", "Music"}, {"dry_run", true}});
    ASSERT_TRUE(!preview.isError);
    const auto payload = json::parse(textOf(preview));
    ASSERT_EQ(payload["mutation_preview"]["tool"], json("audio_add_bus"));
    ASSERT_EQ(payload["mutation_preview"]["target_read"], json(false));
    ASSERT_EQ(payload["mutation_preview"]["requires_confirmation"], json(false));
    ASSERT_TRUE(!payload["mutation_preview"].contains("confirmation_token") ||
                payload["mutation_preview"]["confirmation_token"].is_null());

    for (const auto& bad : {json{{"name", " Music"}, {"dry_run", true}},
                            json{{"name", "tab\there"}, {"dry_run", true}},
                            json{{"name", "Music"}, {"send", ""}, {"dry_run", true}}}) {
        const auto refused = registry.callTool("audio_add_bus", bad);
        ASSERT_TRUE(refused.isError);
        ASSERT_TRUE(textOf(refused).find("mutation_preview") == std::string::npos);
    }
}

struct Register {
    Register() {
        registerTest("AudioAddBus.RequestValidation", request_validation);
        registerTest("AudioAddBus.NamesAsAPersonSeesThem", names_as_a_person_sees_them);
        registerTest("AudioAddBus.NameChangedByTheEngine", name_changed_by_the_engine);
        registerTest("AudioAddBus.Registration", registration);
        registerTest("AudioAddBus.Gated", gated);
        registerTest("AudioAddBus.DryRunReachesNoEngine", dry_run_reaches_no_engine);
    }
} registrar;

} // namespace
