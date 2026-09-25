#include "didi/common/ipc_channel.hpp"
#include "didi/common/json.hpp"
#include "didi/gdextension/editor_hook.hpp"
#include "didi/mcp/mutation_safety.hpp"
#include "didi/mcp/tool_registry.hpp"
#include "didi/offline/import_options.hpp"
#include "didi/offline/resource_indexer.hpp"
#include "didi/runtime/session_kind_policy.hpp"
#include "didi/tools/resolved_tool_binding.hpp"

#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#define ASSERT_TRUE(cond) if (!(cond)) throw std::runtime_error("Assertion failed: " #cond);
#define ASSERT_EQ(a, b) ASSERT_TRUE((a) == (b))

void registerTest(const std::string& name, std::function<void()> fn);

namespace {

using didi::json;
using didi::offline::ImportedStreamFacts;
using didi::offline::applyImportChanges;
using didi::offline::planImportChanges;
using didi::offline::readImportSidecar;

// The sidecars Godot wrote for a generated OGG and WAV in
// tools/vibe/probes/import_config_engine.py. 4.5.1, 4.6.2 and 4.7.2 wrote the
// same text apart from the uid and the hash in the output name.
const std::string kOggSidecar =
    "[remap]\n\n"
    "importer=\"oggvorbisstr\"\n"
    "type=\"AudioStreamOggVorbis\"\n"
    "uid=\"uid://df3lmfqkufmwd\"\n"
    "path=\"res://.godot/imported/music.ogg-3bd46d3a4b41702b152014078d12a390.oggvorbisstr\"\n\n"
    "[deps]\n\n"
    "source_file=\"res://music.ogg\"\n"
    "dest_files=[\"res://.godot/imported/music.ogg-3bd46d3a4b41702b152014078d12a390.oggvorbisstr\"]\n\n"
    "[params]\n\n"
    "loop=false\n"
    "loop_offset=0\n"
    "bpm=0\n"
    "beat_count=0\n"
    "bar_beats=4\n";

const std::string kWavSidecar =
    "[remap]\n\n"
    "importer=\"wav\"\n"
    "type=\"AudioStreamWAV\"\n"
    "uid=\"uid://c867b6i1e3qra\"\n"
    "path=\"res://.godot/imported/sfx.wav-81dc623903ef7cb5cd507a8db8f42c5d.sample\"\n\n"
    "[deps]\n\n"
    "source_file=\"res://sfx.wav\"\n"
    "dest_files=[\"res://.godot/imported/sfx.wav-81dc623903ef7cb5cd507a8db8f42c5d.sample\"]\n\n"
    "[params]\n\n"
    "force/8_bit=false\n"
    "force/mono=false\n"
    "force/max_rate=false\n"
    "force/max_rate_hz=44100\n"
    "edit/trim=false\n"
    "edit/normalize=false\n"
    "edit/loop_mode=0\n"
    "edit/loop_begin=0\n"
    "edit/loop_end=-1\n"
    "compress/mode=2\n";

std::string replaced(std::string text, const std::string& from, const std::string& to) {
    const auto at = text.find(from);
    if (at != std::string::npos) text.replace(at, from.size(), to);
    return text;
}

didi::offline::ImportSidecar sidecarOf(const std::string& text) {
    auto sidecar = readImportSidecar(text);
    ASSERT_TRUE(sidecar.isOk());
    return sidecar.value();
}

// A refusal from planImportChanges, which must be a 400 naming the argument.
didi::Error refusedPlan(const std::string& sidecar, const json& options,
                        const std::optional<ImportedStreamFacts>& facts = std::nullopt) {
    auto planned = planImportChanges(sidecarOf(sidecar), options, facts);
    ASSERT_TRUE(planned.isErr());
    ASSERT_EQ(planned.error().code, 400);
    ASSERT_TRUE(planned.error().data.contains("parameter"));
    return planned.error();
}

std::vector<didi::offline::ImportOptionChange> plannedOk(
    const std::string& sidecar, const json& options,
    const std::optional<ImportedStreamFacts>& facts = std::nullopt) {
    auto planned = planImportChanges(sidecarOf(sidecar), options, facts);
    ASSERT_TRUE(planned.isOk());
    return planned.value();
}

std::string textOf(const didi::mcp::CallToolResult& result) {
    for (const auto& item : result.content) {
        if (item.type == "text") return item.text;
    }
    return {};
}

// A project root the tests can write into, left behind as it was found.
class ScopedProject {
public:
    ScopedProject()
        : original(std::filesystem::current_path()),
          root(std::filesystem::temp_directory_path() / "didi-asset-configure-import") {
        clear();
        std::filesystem::create_directories(root / "music");
        std::filesystem::current_path(root);
        std::ofstream("project.godot") << "config_version=5\n";
        std::ofstream("music/menu.ogg", std::ios::binary) << "OggS";
        std::ofstream("music/menu.ogg.import", std::ios::binary) << kOggSidecar;
        didi::offline::ResourceIndexer::invalidateSharedIndex();
    }
    ~ScopedProject() {
        std::error_code error;
        std::filesystem::current_path(original, error);
        clear();
        didi::offline::ResourceIndexer::invalidateSharedIndex();
    }
    std::string sidecar() const {
        std::ifstream input(root / "music" / "menu.ogg.import", std::ios::binary);
        std::ostringstream buffer;
        buffer << input.rdbuf();
        return buffer.str();
    }

private:
    void clear() {
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }
    std::filesystem::path original;
    std::filesystem::path root;
};

// An attached editor, answered from a script: what the stream loads as, and
// what a reimport does.
class ScriptedEditor final : public didi::ipc::IIpcClient {
public:
    bool connect(const std::string&, int) override { return true; }
    void disconnect() override {}
    bool isConnected() const override { return true; }
    didi::Result<json> sendRequest(const std::string& method, const json& params, int) override {
        methods.push_back(method);
        return answer(method, params);
    }
    std::function<didi::Result<json>(const std::string&, const json&)> answer;
    std::vector<std::string> methods;
};

json oggStream(bool loop, double offset = 0.0) {
    return {{"path", "res://music/menu.ogg"},
            {"class", "AudioStreamOggVorbis"},
            {"length_seconds", 2.0},
            {"properties", {{"loop", loop}, {"loop_offset", offset}}}};
}

// The argument shapes, before any file is read, and then every rule the
// amendment records against the sidecars the engine wrote.
void request_validation() {
    using didi::offline::parseImportConfigureRequest;
    ASSERT_TRUE(parseImportConfigureRequest({{"asset_path", "res://music/menu.ogg"},
                                             {"options", {{"loop", true}}}}).isOk());
    for (const auto& bad : {
             json::object(),
             json{{"options", {{"loop", true}}}},
             json{{"asset_path", ""}, {"options", {{"loop", true}}}},
             json{{"asset_path", 7}, {"options", {{"loop", true}}}},
             json{{"asset_path", std::string(1025, 'a')}, {"options", {{"loop", true}}}},
             json{{"asset_path", "res://music/menu.ogg"}},
             json{{"asset_path", "res://music/menu.ogg"}, {"options", json::object()}},
             json{{"asset_path", "res://music/menu.ogg"}, {"options", json::array({1})}},
             json{{"asset_path", "res://.godot/imported/x.oggvorbisstr"}, {"options", {{"loop", true}}}},
             json{{"asset_path", "res://music/menu.ogg"}, {"options", {{"loop", true}}}, {"force", true}}}) {
        auto parsed = parseImportConfigureRequest(bad);
        ASSERT_TRUE(parsed.isErr());
        ASSERT_EQ(parsed.error().code, 400);
        ASSERT_TRUE(parsed.error().data.contains("parameter"));
    }
    // The sidecar itself is named back to the asset it belongs to.
    auto sidecar_path = parseImportConfigureRequest(
        {{"asset_path", "res://music/menu.ogg.import"}, {"options", {{"loop", true}}}});
    ASSERT_EQ(sidecar_path.error().data["retry_with"]["asset_path"], json("res://music/menu.ogg"));

    // OGG and MP3: loop is a JSON boolean. Godot would take 1 and "yes" as true.
    auto looped = plannedOk(kOggSidecar, {{"loop", true}});
    ASSERT_EQ(looped.size(), 1u);
    ASSERT_EQ(looped[0].text, std::string("true"));
    ASSERT_EQ(looped[0].previous, json(false));
    for (const auto& value : {json(1), json("yes"), json(nullptr)}) {
        ASSERT_EQ(refusedPlan(kOggSidecar, {{"loop", value}}).data["key"], json("loop"));
    }
    // loop_offset: seconds, at least 0 and below the track's length.
    ASSERT_EQ(plannedOk(kOggSidecar, {{"loop_offset", 0.25}})[0].text, std::string("0.25"));
    ASSERT_EQ(plannedOk(kOggSidecar, {{"loop_offset", 1}})[0].text, std::string("1.0"));
    refusedPlan(kOggSidecar, {{"loop_offset", -1.0}});
    const ImportedStreamFacts two_seconds{2.0, std::nullopt};
    refusedPlan(kOggSidecar, {{"loop_offset", 2.0}}, two_seconds);
    ASSERT_TRUE(plannedOk(kOggSidecar, {{"loop_offset", 1.5}}, two_seconds).size() == 1u);
    // A WAV's key on an OGG, and a key the tool does not set, name what to send.
    auto wav_key = refusedPlan(kOggSidecar, {{"edit/loop_mode", 2}});
    ASSERT_EQ(wav_key.data["retry_with"]["options"]["loop"], json(true));
    auto other = refusedPlan(kWavSidecar, {{"compress/mode", 0}});
    ASSERT_EQ(other.data["configurable_keys"],
              json::array({"edit/loop_mode", "edit/loop_begin", "edit/loop_end"}));
    auto ogg_key = refusedPlan(kWavSidecar, {{"loop", true}});
    ASSERT_EQ(ogg_key.data["retry_with"]["options"]["edit/loop_mode"], json(2));
    // Any other importer is refused on the asset, not on an option.
    auto texture = refusedPlan(replaced(kOggSidecar, "oggvorbisstr\"", "texture\""), {{"loop", true}});
    ASSERT_EQ(texture.data["parameter"], json("asset_path"));
    ASSERT_EQ(texture.data["importer"], json("texture"));
    // A key the engine that wrote the sidecar does not have.
    refusedPlan(replaced(kOggSidecar, "loop_offset=0\n", ""), {{"loop_offset", 0.5}});

    // WAV: the loop mode, as a number from 0 to 4 or its label in any case.
    ASSERT_EQ(plannedOk(kWavSidecar, {{"edit/loop_mode", 2}})[0].text, std::string("2"));
    ASSERT_EQ(plannedOk(kWavSidecar, {{"edit/loop_mode", "forward"}})[0].value, json(2));
    ASSERT_EQ(plannedOk(kWavSidecar, {{"edit/loop_mode", "Ping-Pong"}})[0].value, json(3));
    for (const auto& value : {json(5), json(-1), json(2.5), json("sideways"), json(true)}) {
        refusedPlan(kWavSidecar, {{"edit/loop_mode", value}});
    }
    // The window: frames, under a mode that uses it, begin below end, inside
    // the stream.
    refusedPlan(kWavSidecar, {{"edit/loop_mode", 2}, {"edit/loop_begin", -5}});
    refusedPlan(kWavSidecar, {{"edit/loop_mode", 2}, {"edit/loop_end", 0}});
    ASSERT_TRUE(plannedOk(kWavSidecar, {{"edit/loop_mode", 2}, {"edit/loop_end", -1}}).size() == 2u);
    auto no_mode = refusedPlan(kWavSidecar, {{"edit/loop_begin", 1000}});
    ASSERT_EQ(no_mode.data["retry_with"]["options"]["edit/loop_mode"], json(2));
    refusedPlan(kWavSidecar, {{"edit/loop_mode", 2}, {"edit/loop_begin", 5000}, {"edit/loop_end", 1000}});
    const ImportedStreamFacts half_second{0.5, int64_t{22050}};
    refusedPlan(kWavSidecar, {{"edit/loop_mode", 2}, {"edit/loop_end", 999999}}, half_second);
    refusedPlan(kWavSidecar, {{"edit/loop_mode", 2}, {"edit/loop_begin", 11025}}, half_second);
    auto window = plannedOk(kWavSidecar,
                            {{"edit/loop_mode", 2}, {"edit/loop_begin", 1000}, {"edit/loop_end", 5000}},
                            half_second);
    ASSERT_EQ(window.size(), 3u);
    // Sorted by key, so a plan reads the same whichever order the object had.
    ASSERT_EQ(window[0].key, std::string("edit/loop_begin"));
}

// The edit changes the named lines and nothing else, in the file's own line
// endings, and refuses a file Godot would not parse or never writes.
void sidecar_edit() {
    const auto edit = [](const std::string& text, const json& options) {
        auto sidecar = sidecarOf(text);
        auto changes = planImportChanges(sidecar, options, std::nullopt);
        ASSERT_TRUE(changes.isOk());
        return applyImportChanges(sidecar, changes.value());
    };
    auto looped = edit(kOggSidecar, {{"loop", true}, {"loop_offset", 0.25}});
    ASSERT_TRUE(looped.isOk());
    ASSERT_EQ(looped.value(),
              replaced(replaced(kOggSidecar, "loop=false\n", "loop=true\n"), "loop_offset=0\n",
                       "loop_offset=0.25\n"));

    std::string crlf;
    for (const char c : kOggSidecar) {
        if (c == '\n') crlf += "\r\n";
        else crlf += c;
    }
    auto crlf_edit = edit(crlf, {{"loop", true}});
    ASSERT_TRUE(crlf_edit.isOk());
    ASSERT_EQ(crlf_edit.value(), replaced(crlf, "loop=false\r\n", "loop=true\r\n"));

    const auto no_final = kOggSidecar.substr(0, kOggSidecar.size() - 1);
    auto last_line = edit(no_final, {{"loop_offset", 0.5}});
    ASSERT_TRUE(last_line.isOk());
    ASSERT_EQ(last_line.value(), replaced(no_final, "loop_offset=0\n", "loop_offset=0.5\n"));
    ASSERT_TRUE(last_line.value().back() == '4');

    // A line Godot's parser cannot start: the engine would reimport with
    // defaults and a new uid, so the file is refused rather than edited.
    auto unparseable = readImportSidecar(replaced(kOggSidecar, "bpm=0", "bpm=)"));
    ASSERT_TRUE(unparseable.isErr());
    ASSERT_EQ(unparseable.error().code, 422);
    ASSERT_EQ(unparseable.error().data["code"], json("unparseable_import_metadata"));
    ASSERT_TRUE(unparseable.error().data.value("line", 0) > 0);

    // What only a hand-edited file holds.
    const auto hand_edited = [&](const std::string& text) {
        auto result = edit(text, {{"loop", true}});
        ASSERT_TRUE(result.isErr());
        ASSERT_EQ(result.error().code, 422);
        ASSERT_EQ(result.error().data["code"], json("hand_edited_import_metadata"));
    };
    hand_edited(replaced(kOggSidecar, "bpm=0\n", "bpm=0\nloop=false\n"));
    hand_edited(replaced(kOggSidecar, "loop=false\n", "loop=false bpm=0\n"));

    // After the reimport: the sidecar and the stream, each against the plan.
    auto changes = planImportChanges(sidecarOf(kOggSidecar), {{"loop", true}}, std::nullopt).value();
    const auto uid = std::string("uid://df3lmfqkufmwd");
    const auto after_loop = replaced(kOggSidecar, "loop=false", "loop=true");
    ASSERT_TRUE(didi::offline::sidecarDivergences(sidecarOf(after_loop), changes, uid).empty());
    ASSERT_EQ(didi::offline::sidecarDivergences(sidecarOf(kOggSidecar), changes, uid).size(), 1u);
    ASSERT_EQ(didi::offline::sidecarDivergences(
                  sidecarOf(replaced(after_loop, "df3lmfqkufmwd", "b2newuid")), changes, uid).size(), 1u);
    ASSERT_EQ(didi::offline::sidecarDivergences(
                  sidecarOf(replaced(after_loop, "[remap]\n", "[remap]\n\nvalid=false\n")), changes, uid)
                  .size(), 1u);
    ASSERT_TRUE(didi::offline::streamDivergences(oggStream(true), changes).empty());
    ASSERT_EQ(didi::offline::streamDivergences(oggStream(false), changes).size(), 1u);
    // The importer counts loop modes from Detect From WAV and the stream from
    // Disabled, so option 2, Forward, loads as 1.
    auto forward = planImportChanges(sidecarOf(kWavSidecar), {{"edit/loop_mode", 2}}, std::nullopt).value();
    ASSERT_TRUE(didi::offline::streamDivergences({{"properties", {{"loop_mode", 1}}}}, forward).empty());
    ASSERT_EQ(didi::offline::streamDivergences({{"properties", {{"loop_mode", 2}}}}, forward).size(), 1u);
}

void registration() {
    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();
    const auto* tool = registry.getTool("asset_configure_import");
    ASSERT_TRUE(tool != nullptr);
    ASSERT_TRUE(tool->capability.implemented);
    const auto published = tool->toJson();
    ASSERT_EQ(published["_meta"]["didi"]["executionModes"], json::array({"live"}));
    ASSERT_EQ(published["title"], json("Configure an asset's import"));
    ASSERT_EQ(published["annotations"]["readOnlyHint"], json(false));
    ASSERT_EQ(published["annotations"]["idempotentHint"], json(true));
    const auto& schema = tool->inputSchema;
    ASSERT_EQ(schema["required"], json::array({"asset_path", "options"}));
    ASSERT_EQ(schema["additionalProperties"], json(false));
    ASSERT_TRUE(schema["properties"].contains("dry_run"));
    ASSERT_TRUE(!schema["properties"].contains("confirmation_token"));
    for (const auto* parameter : {"asset_path", "options"}) {
        ASSERT_TRUE(schema["properties"][parameter].contains("description"));
    }
    for (const auto* key : {"loop", "loop_offset", "edit/loop_mode", "edit/loop_begin", "edit/loop_end"}) {
        ASSERT_TRUE(schema["properties"]["options"]["properties"][key].contains("description"));
    }

    didi::mcp::ResolvedToolBinding binding;
    binding.canonical_name = "asset_configure_import";
    binding.policy_source = "asset_configure_import";
    ASSERT_TRUE(didi::mcp::MutationSafety::isMutation(binding));
    ASSERT_TRUE(!didi::mcp::MutationSafety::requiresConfirmation(
        binding, json{{"asset_path", "res://music/menu.ogg"}, {"options", {{"loop", true}}}}));
    ASSERT_TRUE(didi::runtime::livePolicyForTool("asset_configure_import") ==
                didi::runtime::LiveSessionKindPolicy::editor_only);
    ASSERT_TRUE(didi::runtime::livePolicyForMethod("asset.readImportedStream") ==
                didi::runtime::LiveSessionKindPolicy::editor_only);
}

// With no editor the call says where to read the options instead, and a game
// session is refused the stream read at the hook, because a game cannot
// reimport.
void gated() {
    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();
    registry.setIpcClient(nullptr);
    const auto offline = registry.callTool(
        "asset_configure_import", {{"asset_path", "res://music/menu.ogg"}, {"options", {{"loop", true}}}});
    ASSERT_TRUE(offline.isError);
    ASSERT_TRUE(textOf(offline).find("resource_inspect") != std::string::npos);
    ASSERT_TRUE(textOf(offline).find("\"status\":\"configured\"") == std::string::npos);

    auto& hook = didi::godot::EditorHook::instance();
    hook.cancelPendingCommands("test reset");
    didi::godot::EditorHookTestAccess::setSessionKind(hook, didi::runtime::SessionKind::game);
    auto refused = didi::godot::EditorHookTestAccess::executeOnMainThread(
        hook, "asset.readImportedStream", json{{"path", "res://music/menu.ogg"}});
    didi::godot::EditorHookTestAccess::setSessionKind(hook, std::nullopt);
    ASSERT_EQ(refused["error"]["code"], json(409));
    ASSERT_EQ(refused["error"]["data"]["code"], json("session_kind_rejected"));
}

// The dry run is the safety envelope's own preview. With no editor it checks
// everything that needs no engine, says it did not read the stream, and
// leaves the sidecar's bytes as they were.
void dry_run_writes_nothing() {
    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();
    registry.setIpcClient(nullptr);
    ScopedProject project;
    const auto preview = registry.callTool(
        "asset_configure_import",
        {{"asset_path", "res://music/menu.ogg"}, {"options", {{"loop", true}}}, {"dry_run", true}});
    ASSERT_TRUE(!preview.isError);
    const auto text = textOf(preview);
    ASSERT_TRUE(text.find("mutation_preview") != std::string::npos);
    ASSERT_TRUE(text.find("\"stream_read\":false") != std::string::npos);
    ASSERT_TRUE(text.find("\"planned_options\":{\"loop\":true}") != std::string::npos);
    ASSERT_EQ(project.sidecar(), kOggSidecar);

    // A preview of a call that would be refused is refused.
    const auto wrong = registry.callTool(
        "asset_configure_import",
        {{"asset_path", "res://music/menu.ogg"}, {"options", {{"edit/loop_mode", 2}}}, {"dry_run", true}});
    ASSERT_TRUE(wrong.isError);
    ASSERT_TRUE(textOf(wrong).find("mutation_preview") == std::string::npos);
    ASSERT_EQ(project.sidecar(), kOggSidecar);
}

// The call, against an editor answered from a script: the change that holds
// is reported with what it replaced, and a change that does not hold, or a
// reimport that fails, leaves the sidecar's bytes as they were.
void verifies_and_rolls_back() {
    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();
    auto editor = std::make_shared<ScriptedEditor>();
    registry.setIpcClient(editor);
    const json call = {{"asset_path", "res://music/menu.ogg"}, {"options", {{"loop", true}}}};
    {
        ScopedProject project;
        bool reimported = false;
        editor->answer = [&](const std::string& method, const json&) -> didi::Result<json> {
            if (method == "asset.reimport") {
                reimported = true;
                return json{{"reimported", json::array({"res://music/menu.ogg"})}, {"idle", true}};
            }
            return oggStream(reimported);
        };
        const auto result = registry.callTool("asset_configure_import", call);
        ASSERT_TRUE(!result.isError);
        const auto payload = json::parse(textOf(result));
        ASSERT_EQ(payload["status"], json("configured"));
        ASSERT_EQ(payload["previous"]["loop"], json(false));
        ASSERT_EQ(payload["options"]["loop"], json(true));
        ASSERT_EQ(payload["verified"], json(true));
        ASSERT_EQ(payload["uid"], json("uid://df3lmfqkufmwd"));
        ASSERT_EQ(project.sidecar(), replaced(kOggSidecar, "loop=false", "loop=true"));
    }
    {
        // The engine loads the stream without the change: the old file goes back.
        ScopedProject project;
        editor->answer = [&](const std::string& method, const json&) -> didi::Result<json> {
            if (method == "asset.reimport") return json{{"reimported", json::array({"res://music/menu.ogg"})}};
            return oggStream(false);
        };
        const auto result = registry.callTool("asset_configure_import", call);
        ASSERT_TRUE(result.isError);
        const auto text = textOf(result);
        ASSERT_TRUE(text.find("import_change_not_held") != std::string::npos);
        ASSERT_TRUE(text.find("\"rolled_back\":true") != std::string::npos);
        ASSERT_EQ(project.sidecar(), kOggSidecar);
    }
    {
        // The reimport fails: the old file goes back too.
        ScopedProject project;
        int reimports = 0;
        editor->answer = [&](const std::string& method, const json&) -> didi::Result<json> {
            if (method == "asset.reimport") {
                ++reimports;
                if (reimports == 1) {
                    return json{{"error", {{"code", 422}, {"message", "Godot could not import it"},
                                           {"data", {{"code", "asset_import_failed"}}}}}};
                }
                return json{{"reimported", json::array({"res://music/menu.ogg"})}};
            }
            return oggStream(false);
        };
        const auto result = registry.callTool("asset_configure_import", call);
        ASSERT_TRUE(result.isError);
        ASSERT_TRUE(textOf(result).find("\"rolled_back\":true") != std::string::npos);
        ASSERT_EQ(reimports, 2);
        ASSERT_EQ(project.sidecar(), kOggSidecar);
    }
    registry.setIpcClient(nullptr);
}

// resource_inspect reads the options offline, from the sidecars each engine
// wrote, and says which of them asset_configure_import sets.
void resource_inspect_reports_import_options() {
    const auto ogg = didi::offline::describeImportSidecar(kOggSidecar);
    ASSERT_EQ(ogg["importer"], json("oggvorbisstr"));
    ASSERT_EQ(ogg["type"], json("AudioStreamOggVorbis"));
    ASSERT_EQ(ogg["uid"], json("uid://df3lmfqkufmwd"));
    ASSERT_EQ(ogg["options"]["loop"], json(false));
    ASSERT_EQ(ogg["options"]["loop_offset"], json(0));
    ASSERT_EQ(ogg["options"]["bar_beats"], json(4));
    ASSERT_EQ(ogg["configurable"], json::array({"loop", "loop_offset"}));
    ASSERT_TRUE(!ogg.contains("valid"));
    const auto wav = didi::offline::describeImportSidecar(kWavSidecar);
    ASSERT_EQ(wav["options"]["edit/loop_mode"], json(0));
    ASSERT_EQ(wav["options"]["edit/loop_end"], json(-1));
    ASSERT_EQ(wav["options"]["force/max_rate_hz"], json(44100));
    const auto broken = didi::offline::describeImportSidecar(replaced(kOggSidecar, "bpm=0", "bpm=)"));
    ASSERT_TRUE(broken.contains("parse_error"));
    ASSERT_TRUE(broken.value("line", 0) > 0);

    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();
    registry.setIpcClient(nullptr);
    ScopedProject project;
    const auto inspected = registry.callTool("resource_inspect", {{"resource_path", "res://music/menu.ogg"}});
    ASSERT_TRUE(!inspected.isError);
    const auto payload = json::parse(textOf(inspected));
    ASSERT_EQ(payload["import"]["options"]["loop"], json(false));
    ASSERT_EQ(payload["import"]["importer"], json("oggvorbisstr"));
}

struct Register {
    Register() {
        registerTest("AssetConfigureImport.RequestValidation", request_validation);
        registerTest("AssetConfigureImport.SidecarEdit", sidecar_edit);
        registerTest("AssetConfigureImport.Registration", registration);
        registerTest("AssetConfigureImport.Gated", gated);
        registerTest("AssetConfigureImport.DryRunWritesNothing", dry_run_writes_nothing);
        registerTest("AssetConfigureImport.VerifiesAndRollsBack", verifies_and_rolls_back);
        registerTest("Tools.ResourceInspectReportsImportOptions", resource_inspect_reports_import_options);
    }
} registrar;

} // namespace
