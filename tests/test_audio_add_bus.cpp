#include "didi/common/ipc_channel.hpp"
#include "didi/common/json.hpp"
#include "didi/gdextension/editor_hook.hpp"
#include "didi/offline/audio_bus_layout.hpp"
#include "didi/mcp/mutation_safety.hpp"
#include "didi/mcp/tool_registry.hpp"
#include "didi/runtime/audio_requests.hpp"
#include "didi/runtime/session_kind_policy.hpp"
#include "didi/tools/resolved_tool_binding.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

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
    // A no-break space is a space, not an invisible character, and a zero
    // width space is invisible; the sentence says which one was sent.
    ASSERT_TRUE(refusedWith(nbsp + "Voice").message.find("a space (U+00A0)") != std::string::npos);
    ASSERT_TRUE(refusedWith(zero_width_space + "Music").message.find("an invisible character (U+200B)") !=
                std::string::npos);
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

// An attached editor, answered from a script: the bus the bridge reports and
// the file it says the editor writes.
class ScriptedEditor final : public didi::ipc::IIpcClient {
public:
    bool connect(const std::string&, int) override { return true; }
    void disconnect() override {}
    bool isConnected() const override { return true; }
    didi::Result<json> sendRequest(const std::string& method, const json&, int) override {
        methods.push_back(method);
        return answer;
    }
    json answer;
    std::vector<std::string> methods;
};

class ScopedLayoutProject {
public:
    explicit ScopedLayoutProject(const std::string& project_godot)
        : original(std::filesystem::current_path()),
          root(std::filesystem::temp_directory_path() / "didi-audio-layout-obstacles") {
        clear();
        std::filesystem::create_directories(root);
        std::filesystem::current_path(root);
        std::ofstream("project.godot") << project_godot;
    }
    ~ScopedLayoutProject() {
        std::error_code error;
        std::filesystem::current_path(original, error);
        clear();
    }

private:
    void clear() {
        std::error_code error;
        // A read-only file cannot be removed on Windows until it is writable.
        for (const auto& entry : std::filesystem::recursive_directory_iterator(root, error)) {
            std::filesystem::permissions(entry.path(), std::filesystem::perms::owner_write,
                                         std::filesystem::perm_options::add, error);
        }
        std::filesystem::remove_all(root, error);
    }
    std::filesystem::path original;
    std::filesystem::path root;
};

json payloadOfCall(const char* tool, const json& arguments) {
    const auto result = didi::mcp::ToolRegistry::instance().callTool(tool, arguments);
    return json::parse(textOf(result));
}

// What keeps the editor's own write from arriving, found by vibe session
// nineteen on 4.7.2. A read-only layout file: the editor printed "Safe save
// failed", and audio_add_bus waited two seconds and said the editor writes the
// layout "on its own schedule", while audio_configure_bus said the change
// reaches the project on disk. And a setting moved underneath the editor, which
// keeps writing the file it opened: the tool read the file the setting named,
// reported layout_written: false for a bus the editor had written, and named
// the wrong file as layout_path.
void layout_write_obstacles() {
    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();
    auto editor = std::make_shared<ScriptedEditor>();
    registry.setIpcClient(editor);

    {
        // The setting names a file the editor did not open. The bridge reports
        // the one it did, and that is the file read back.
        ScopedLayoutProject project(
            "config_version=5\n\n[audio]\n\nbuses/default_bus_layout=\"res://moved.tres\"\n");
        std::ofstream("opened.tres") << "[gd_resource type=\"AudioBusLayout\" format=3]\n\n"
                                        "[resource]\nbus/1/name = &\"Music\"\n";
        editor->answer = {{"status", "success"}, {"bus", 1}, {"name", "Music"},
                          {"layout_path", "res://opened.tres"},
                          {"project_layout_path", "res://moved.tres"},
                          {"persisted_by_editor", true}};
        const auto added = payloadOfCall("audio_add_bus", {{"name", "Music"}});
        ASSERT_EQ(added["layout_written"], json(true));
        ASSERT_EQ(added["layout_path"], json("res://opened.tres"));
        ASSERT_TRUE(!added.contains("layout_read_only"));
    }
    {
        ScopedLayoutProject project("config_version=5\n");
        std::ofstream("default_bus_layout.tres") << "[gd_resource type=\"AudioBusLayout\" format=3]\n\n"
                                                    "[resource]\n";
        // Every write bit: on Windows the file is read-only only when none is left.
        std::filesystem::permissions("default_bus_layout.tres",
                                     std::filesystem::perms::owner_write | std::filesystem::perms::group_write |
                                         std::filesystem::perms::others_write,
                                     std::filesystem::perm_options::remove);
        editor->answer = {{"status", "success"}, {"bus", 1}, {"name", "Music"},
                          {"layout_path", "res://default_bus_layout.tres"},
                          {"persisted_by_editor", true}};
        const auto started = std::chrono::steady_clock::now();
        const auto added = payloadOfCall("audio_add_bus", {{"name", "Music"}});
        ASSERT_EQ(added["layout_written"], json(false));
        ASSERT_EQ(added["layout_read_only"], json(true));
        ASSERT_TRUE(added["layout_note"].get<std::string>().find("read-only") != std::string::npos);
        ASSERT_TRUE(added["layout_note"].get<std::string>().find("own schedule") == std::string::npos);
#ifdef _WIN32
        // A save that has already failed is not waited on.
        ASSERT_TRUE(std::chrono::steady_clock::now() - started < std::chrono::seconds(2));
#endif
        (void)started;

        editor->answer = {{"status", "success"}, {"bus", 1}, {"persisted_by_editor", true},
                          {"layout_path", "res://default_bus_layout.tres"},
                          {"limitation", "reaches the project on disk anyway"}};
        const auto configured = payloadOfCall("audio_configure_bus", {{"bus", "Music"}, {"mute", true}});
        ASSERT_EQ(configured["layout_read_only"], json(true));
#ifdef _WIN32
        ASSERT_EQ(configured["persisted_by_editor"], json(false));
        ASSERT_TRUE(configured["limitation"].get<std::string>().find("read-only") != std::string::npos);
#endif
    }
    registry.setIpcClient(nullptr);
}

// An attached engine whose read fails is not an engine that is absent. Every
// game session failed audio.listBuses on a missing EditorInterface, and the
// tool answered with the layout file as if nothing were attached, which is how
// that failure stayed hidden.
void a_failed_live_read_says_so() {
    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();
    ScopedLayoutProject project("config_version=5\n");
    class FailingEngine final : public didi::ipc::IIpcClient {
    public:
        bool connect(const std::string&, int) override { return true; }
        void disconnect() override {}
        bool isConnected() const override { return true; }
        didi::Result<json> sendRequest(const std::string&, const json&, int) override {
            return didi::Error(500, "Can't retrieve singleton 'EditorInterface' outside of editor.");
        }
    };
    registry.setIpcClient(std::make_shared<FailingEngine>());
    const auto listed = payloadOfCall("audio_list_buses", json::object());
    registry.setIpcClient(nullptr);
    ASSERT_EQ(listed["execution_mode"], json("offline_fallback"));
    ASSERT_EQ(listed["live_error"]["code"], json(500));
    ASSERT_TRUE(listed.contains("live_error_note"));
}

// The layout in a named file, read whatever project.godot names.
void layout_read_from_a_named_file() {
    ScopedLayoutProject project(
        "config_version=5\n\n[audio]\n\nbuses/default_bus_layout=\"res://named.tres\"\n");
    std::ofstream("other.tres") << "[gd_resource type=\"AudioBusLayout\" format=3]\n\n"
                                   "[resource]\nbus/1/name = &\"Other\"\n";
    const auto named = didi::offline::readAudioBusLayout(".");
    ASSERT_TRUE(named.isOk());
    ASSERT_EQ(named.value()["layout_path"], json("res://named.tres"));
    ASSERT_EQ(named.value()["layout_present"], json(false));
    const auto other = didi::offline::readAudioBusLayoutFile(".", "res://other.tres");
    ASSERT_TRUE(other.isOk());
    ASSERT_EQ(other.value()["layout_path"], json("res://other.tres"));
    ASSERT_EQ(other.value()["buses"][1]["name"], json("Other"));
}

// The tool that answers why a game is silent could not ask the game. Both audio
// reads were editor only while audio_list_buses's documentation described a bus
// a script muted at runtime and audio_configure_bus carried a sentence for a
// game session nothing could reach. They are editor or game now, at the tool,
// at the method and at the hook, and audio_add_bus stays editor only.
void a_game_answers_for_its_own_mix() {
    using didi::runtime::LiveSessionKindPolicy;
    ASSERT_TRUE(didi::runtime::livePolicyForTool("audio_list_buses") == LiveSessionKindPolicy::editor_or_game);
    ASSERT_TRUE(didi::runtime::livePolicyForTool("audio_configure_bus") == LiveSessionKindPolicy::editor_or_game);
    ASSERT_TRUE(didi::runtime::livePolicyForTool("audio_add_bus") == LiveSessionKindPolicy::editor_only);

    auto& hook = didi::godot::EditorHook::instance();
    hook.cancelPendingCommands("test reset");
    didi::godot::EditorHookTestAccess::setSessionKind(hook, didi::runtime::SessionKind::game);
    for (const auto* method : {"audio.listBuses", "audio.configureBus"}) {
        auto answered = didi::godot::EditorHookTestAccess::executeOnMainThread(
            hook, method, json{{"bus", "Master"}, {"mute", false}});
        // No engine here, so the call fails further in; what matters is that
        // the session kind is not the reason.
        const bool kind_refused = answered.contains("error") && answered["error"].contains("data") &&
                                  answered["error"]["data"].value("code", std::string()) == "session_kind_rejected";
        ASSERT_TRUE(!kind_refused);
    }
    auto refused = didi::godot::EditorHookTestAccess::executeOnMainThread(
        hook, "audio.addBus", json{{"name", "Music"}});
    didi::godot::EditorHookTestAccess::setSessionKind(hook, std::nullopt);
    ASSERT_EQ(refused["error"]["data"]["code"], json("session_kind_rejected"));
}

struct Register {
    Register() {
        registerTest("AudioAddBus.AGameAnswersForItsOwnMix", a_game_answers_for_its_own_mix);
        registerTest("AudioAddBus.AFailedLiveReadSaysSo", a_failed_live_read_says_so);
        registerTest("AudioAddBus.LayoutWriteObstacles", layout_write_obstacles);
        registerTest("AudioAddBus.LayoutReadFromANamedFile", layout_read_from_a_named_file);
        registerTest("AudioAddBus.RequestValidation", request_validation);
        registerTest("AudioAddBus.NamesAsAPersonSeesThem", names_as_a_person_sees_them);
        registerTest("AudioAddBus.NameChangedByTheEngine", name_changed_by_the_engine);
        registerTest("AudioAddBus.Registration", registration);
        registerTest("AudioAddBus.Gated", gated);
        registerTest("AudioAddBus.DryRunReachesNoEngine", dry_run_reaches_no_engine);
    }
} registrar;

} // namespace
