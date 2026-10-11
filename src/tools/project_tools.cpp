#include "didi/mcp/project_tools.hpp"
#include "didi/common/project_path.hpp"
#include "didi/offline/import_options.hpp"
#include "didi/offline/project_search.hpp"
#include "didi/offline/project_settings_file.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <set>
#include "didi/mcp/tool_registration.hpp"

namespace didi::mcp {
namespace {

CallToolResult forwardLiveProject(const json& args,
                                  const std::shared_ptr<ipc::IIpcClient>& ipc,
                                  const char* method,
                                  const char* operation) {
    if (!ipc || !ipc->isConnected()) {
        return CallToolResult::notConnected(std::string("Godot Editor is offline. Launch Godot to ") + operation + ".");
    }
    auto response = ipc->sendRequest(method, args, ipc::kWaitForDefinitiveResponse);
    if (response.isErr()) {
        return CallToolResult::fromError(response.error(), std::string("Failed to ") + operation + ": ");
    }
    return CallToolResult::successJson(response.value());
}

Result<offline::SearchOptions> parseSearchOptions(const json& args) {
    if (!args.is_object()) return Error::invalidArgument("Project search arguments must be an object");
    if (!args.contains("query") || !args["query"].is_string()) {
        return Error::invalidArgument("query must be a string");
    }
    offline::SearchOptions options;
    options.query = args["query"].get<std::string>();
    if (args.contains("search_path")) {
        if (!args["search_path"].is_string()) return Error::invalidArgument("search_path must be a string");
        options.search_path = args["search_path"].get<std::string>();
    }
    if (args.contains("extensions")) {
        if (!args["extensions"].is_array() || args["extensions"].empty()) {
            return Error::invalidArgument("extensions must be a non-empty array of strings");
        }
        options.extensions.clear();
        for (const auto& value : args["extensions"]) {
            if (!value.is_string()) return Error::invalidArgument("extensions must contain only strings");
            options.extensions.push_back(value.get<std::string>());
        }
    }
    if (args.contains("case_sensitive")) {
        if (!args["case_sensitive"].is_boolean()) return Error::invalidArgument("case_sensitive must be a boolean");
        options.case_sensitive = args["case_sensitive"].get<bool>();
    }
    if (args.contains("whole_word")) {
        if (!args["whole_word"].is_boolean()) return Error::invalidArgument("whole_word must be a boolean");
        options.whole_word = args["whole_word"].get<bool>();
    }
    if (args.contains("max_results")) {
        const auto& value = args["max_results"];
        const bool valid = value.is_number_unsigned()
            ? value.get<uint64_t>() >= 1u && value.get<uint64_t>() <= 500u
            : value.is_number_integer() && value.get<int64_t>() >= 1 &&
              value.get<int64_t>() <= 500;
        if (!valid) {
            return Error::invalidArgument("max_results must be an integer from 1 to 500");
        }
        options.max_results = static_cast<size_t>(value.get<uint64_t>());
    }
    return options;
}

CallToolResult searchError(const Error& error) {
    return CallToolResult::fromError(error, "Invalid project search request: ");
}

} // namespace

// Lists the autoloads, using the editor when one is attached and project.godot
// directly when none is.
//
// Offline is the ordinary state of a machine, and five readers of project files
// already answer there. This one refused, while project_analyze_impact resolved
// autoloads out of the same file with no editor and reported the line each one
// is on, so the [autoload] section was parsed offline by one tool and
// unreadable to the tool named after it (#780).
CallToolResult handleProjectListAutoloads(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    if (ipc && ipc->isConnected()) {
        return forwardLiveProject(args, ipc, "project.listAutoloads", "list project autoloads");
    }
    if (!args.is_object() || !args.empty()) {
        return CallToolResult::errorJson(400, "Invalid autoload list request: this tool takes no arguments");
    }
    std::error_code root_error;
    const auto root = std::filesystem::current_path(root_error);
    if (root_error) {
        return CallToolResult::errorJson(500, "The project root cannot be resolved for an offline autoload read");
    }
    auto read = offline::readProjectAutoloads(root);
    if (read.isErr()) {
        return CallToolResult::fromError(read.error(), "Failed to read the project autoloads: ");
    }
    json entries = json::array();
    for (const auto& autoload : read.value()) {
        entries.push_back({{"name", autoload.name},
                           {"path", autoload.path},
                           {"singleton", autoload.singleton}});
    }
    return CallToolResult::successJson(
        {{"status", "success"},
         {"autoloads", std::move(entries)},
         {"execution_mode", "offline_fallback"},
         {"is_live_engine", false},
         {"read_from", "res://project.godot"},
         // An autoload is a project setting and the engine adds no defaults to
         // this section, so the file is the whole answer. What the file cannot
         // show is an editor holding an unsaved change to it.
         {"limitation",
          "project.godot was read directly because no editor session is attached. An editor "
          "with unsaved changes to the autoload list would answer differently, and nothing "
          "here has loaded any of these scripts, so a path that no longer exists is reported "
          "exactly as a working one is. Attach an editor to have the engine answer."}});
}
CallToolResult handleProjectSetAutoload(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    return forwardLiveProject(args, ipc, "project.setAutoload", "persist a project autoload");
}
CallToolResult handleProjectRemoveAutoload(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    return forwardLiveProject(args, ipc, "project.removeAutoload", "remove a project autoload");
}
CallToolResult handleProjectListInputActions(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    return forwardLiveProject(args, ipc, "project.listInputActions", "list project input actions");
}
CallToolResult handleProjectSetInputAction(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    return forwardLiveProject(args, ipc, "project.setInputAction", "persist a project input action");
}
CallToolResult handleProjectRemoveInputAction(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    return forwardLiveProject(args, ipc, "project.removeInputAction", "remove a project input action");
}
// Reads a project setting, using the editor when one is attached and
// project.godot directly when none is.
//
// The offline route exists because its own writer has one. project_set_setting
// writes the file with no editor and explains itself, and then the tool whose
// job is to read the value back answered 503, so a caller could not verify the
// write, read before overwriting, or diff either side of it. The workaround was
// to parse project.godot in the client, which is the thing the tool exists to
// avoid (#780).
CallToolResult handleProjectGetSetting(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    if (ipc && ipc->isConnected()) {
        return forwardLiveProject(args, ipc, "project.getSetting", "read a project setting");
    }
    if (!args.is_object()) {
        return CallToolResult::errorJson(400, "Invalid project setting request: arguments must be an object");
    }
    const std::string setting = args.value("setting", "");
    std::error_code root_error;
    const auto root = std::filesystem::current_path(root_error);
    if (root_error) {
        return CallToolResult::errorJson(500, "The project root cannot be resolved for an offline setting read");
    }
    auto read = offline::readProjectSetting(root, setting);
    if (read.isErr()) {
        return CallToolResult::fromError(read.error(), "Failed to read a project setting: ");
    }
    if (!read.value().existed) {
        // Weaker than the live 404 and it says so. The engine answers this
        // question from its own defaults as well as from the file, and Godot
        // writes a default into project.godot only once something changes it,
        // so a built-in this file does not mention is a setting the engine
        // still has a value for.
        return CallToolResult::errorJson(
            404,
            "project.godot does not set " + setting +
                ". That is not the same answer an engine gives: Godot holds a default for "
                "every built-in setting and only writes one into this file once it is "
                "changed, so an attached editor may still have a value for this name. "
                "Attach an editor to ask it.",
            json{{"code", "not_found"},
                 {"setting", setting},
                 {"execution_mode", "offline_fallback"},
                 {"is_live_engine", false},
                 {"read_from", "res://project.godot"}});
    }
    return CallToolResult::successJson(
        {{"status", "success"},
         {"setting", read.value().setting},
         // The literal, not a parsed value. Turning `PackedStringArray("4.5")`
         // into JSON offline means writing a Variant parser, and the writer
         // beside this one already publishes what it put in the file for the
         // same reason: the literal is the evidence, where a value that looks
         // right is a guess. A caller that wants the value parsed attaches an
         // editor and gets `value` instead.
         {"value_literal", read.value().literal},
         {"execution_mode", "offline_fallback"},
         {"is_live_engine", false},
         {"read_from", "res://project.godot"},
         {"limitation",
          "project.godot was read directly because no editor session is attached, so this is "
          "the literal text the file holds rather than the value an engine would load it as, "
          "and it is published as value_literal rather than value for that reason. A running "
          "editor holding an unsaved change would answer differently."}});
}
namespace {

constexpr const char* kTranslationsSetting = "internationalization/locale/translations";

std::string lowerExtension(const std::filesystem::path& path) {
    auto extension = path.extension().string();
    if (!extension.empty() && extension.front() == '.') extension.erase(0, 1);
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return extension;
}

std::string elementName(size_t index) {
    return "value[" + std::to_string(index) + "]";
}

// Why a file cannot be registered as a translation, or nothing when it can.
// Asked of the engine on 4.5.1, 4.6.2 and 4.7.2 with each kind registered alone
// and the game told to use its locale (tools/vibe/probes/localisation_engine.py,
// the register part): a .translation, a .po, a .mo and a .res holding a
// Translation load. A .tres never does, because the engine loads registered
// translations before it can read a text resource and prints "No loader found".
// The CSV the translations were imported from loads nothing and prints one
// ERROR. A .json loads nothing and prints nothing at all.
std::optional<Error> translationFileProblem(const json& items, size_t index,
                                            const std::string& text,
                                            const std::filesystem::path& resolved) {
    static const std::set<std::string> registrable = {"translation", "po", "mo", "res"};
    const auto extension = lowerExtension(resolved);
    if (registrable.count(extension)) return std::nullopt;
    const std::string where = elementName(index) + ", " + text + ",";
    json data{{"code", "not_a_translation"},
              {"setting", kTranslationsSetting},
              {"index", index},
              {"resource_path", text},
              {"retryable", false}};

    if (extension == "csv") {
        auto sidecar_text = offline::readImportSidecarFile(resolved.string() + ".import");
        std::optional<offline::ImportSidecar> sidecar;
        if (sidecar_text.isOk()) {
            auto parsed = offline::readImportSidecar(sidecar_text.value());
            if (parsed.isOk()) sidecar = std::move(parsed.value());
        }
        if (!sidecar) {
            data["code"] = "translation_source_not_imported";
            return Error(409,
                         where + " is a CSV that has not been imported, so there is no "
                         ".translation file to register yet. Godot registers what the import "
                         "writes, not the CSV: registered, it loads no translation and prints "
                         "an ERROR at startup. Import it in an attached editor, then register "
                         "the files its .import sidecar lists under dest_files.",
                         std::move(data));
        }
        const auto destinations = offline::importDestinations(*sidecar);
        if (sidecar->importer == "csv_translation" && !destinations.empty()) {
            // The same list with this entry replaced by what its import wrote,
            // which is the value that succeeds.
            json replaced = json::array();
            std::set<std::string> seen;
            for (size_t i = 0; i < items.size(); ++i) {
                if (i == index) {
                    for (const auto& destination : destinations) {
                        if (seen.insert(destination).second) replaced.push_back(destination);
                    }
                } else if (!items[i].is_string() || seen.insert(items[i].get<std::string>()).second) {
                    replaced.push_back(items[i]);
                }
            }
            std::string listed;
            for (size_t i = 0; i < destinations.size(); ++i) {
                listed += (i == 0 ? "" : i + 1 == destinations.size() ? " and " : ", ") + destinations[i];
            }
            data["code"] = "translation_source_registered";
            data["translation_files"] = destinations;
            data["retry_with"] = json{{"value", std::move(replaced)}};
            return Error(409,
                         where + " is the CSV the translations were imported from. Godot "
                         "registers what the import wrote, not its source: with the CSV "
                         "registered the game loads no translation and prints an ERROR at "
                         "startup. Register " + listed + " instead.",
                         std::move(data));
        }
        const std::string importer =
            sidecar->importer.empty() ? "no importer" : "the " + sidecar->importer + " importer";
        return Error(409,
                     where + " is a CSV its .import sidecar gives to " + importer +
                         ", so it holds no translation. A CSV of translations is imported by "
                         "csv_translation, and the .translation files it writes are what a "
                         "project registers.",
                     std::move(data));
    }
    if (extension == "tres") {
        return Error(409,
                     where + " is a text resource, and Godot loads registered translations "
                     "before it can read one: the game prints \"No loader found\" at startup "
                     "and loads nothing from it, whatever the file holds. Save the Translation "
                     "as .res, or register a .translation, .po or .mo file.",
                     std::move(data));
    }
    return Error(409,
                 where + " is not a file Godot registers a translation from. It takes a "
                 ".translation, .po, .mo or .res file; anything else loads no translation, and "
                 "a file with a loader of its own, such as a .json, fails without a word.",
                 std::move(data));
}

// A res:// value with nothing at it, answered the way the live route answers a
// single string (#490), with the element that named it.
Error missingSettingResource(const std::string& setting, const std::string& text,
                             std::optional<size_t> index) {
    json data{{"setting", setting},
              {"resource_path", text},
              {"resource_exists", false},
              {"retryable", false}};
    std::string where = text;
    if (index) {
        data["index"] = *index;
        where += ", at " + elementName(*index);
    }
    return Error(404,
                 "Project setting resource not found: " + where + ". Nothing is at that path, "
                 "so " + setting + " would name a file the project cannot load.",
                 std::move(data));
}

} // namespace

std::optional<Error> checkSettingValuePaths(const json& args) {
    if (!args.is_object() || args.value("remove", false)) return std::nullopt;
    const auto value = args.find("value");
    if (value == args.end() || !value->is_array()) return std::nullopt;
    const auto setting_value = args.find("setting");
    if (setting_value == args.end() || !setting_value->is_string()) return std::nullopt;
    const auto& setting = setting_value->get_ref<const std::string&>();
    const bool translations = setting == kTranslationsSetting;

    const auto& items = *value;
    for (size_t index = 0; index < items.size(); ++index) {
        if (!items[index].is_string()) continue;
        const auto& text = items[index].get_ref<const std::string&>();
        if (!strings::startsWith(text, "res://")) continue;
        auto resolved = paths::resolveProjectFileForWrite(text);
        if (resolved.isErr()) {
            return Error(400,
                         elementName(index) + ", " + text + ", is not a path in the project: " +
                             resolved.error().message + ".",
                         json{{"setting", setting}, {"index", index}, {"retryable", false}});
        }
        std::error_code error;
        if (!std::filesystem::exists(resolved.value(), error) || error) {
            return missingSettingResource(setting, text, index);
        }
        if (translations) {
            if (auto problem = translationFileProblem(items, index, text, resolved.value())) {
                return problem;
            }
        }
    }
    return std::nullopt;
}

// Persists a project setting, using the editor when one is attached and
// project.godot directly when none is.
//
// The offline route exists because of a deadlock, not for convenience. Every
// project writer used to be live-only, a live session needs the Didi addon
// enabled, and enabling the addon is a write to editor_plugins/enabled. An
// agent handed a bare project could not perform the one call that would let it
// perform any other (#382).
CallToolResult handleProjectSetSetting(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    if (ipc && ipc->isConnected()) {
        return forwardLiveProject(args, ipc, "project.setSetting", "persist a project setting");
    }
    if (!args.is_object()) {
        return CallToolResult::errorJson(400, "Invalid project setting request: arguments must be an object");
    }
    const std::string setting = args.value("setting", "");
    const bool remove = args.value("remove", false);
    if (remove && args.contains("value")) {
        return CallToolResult::errorJson(400, "Specify either value or remove: true, not both");
    }
    if (!remove && !args.contains("value")) {
        return CallToolResult::errorJson(400, "value is required unless remove is true");
    }
    // The live route refuses a res:// string with nothing at it (#490), and
    // this route wrote one, so application/run/main_scene could name a scene
    // that does not exist with no editor attached (#989). An array's paths are
    // checked before either route, by checkSettingValuePaths.
    if (!remove && args["value"].is_string()) {
        const auto& text = args["value"].get_ref<const std::string&>();
        if (strings::startsWith(text, "res://")) {
            auto resolved = paths::resolveProjectFileForWrite(text);
            if (resolved.isErr()) {
                return CallToolResult::errorJson(
                    400, "value, " + text + ", is not a path in the project: " +
                             resolved.error().message + ".",
                    json{{"setting", setting}});
            }
            std::error_code missing_error;
            if (!std::filesystem::exists(resolved.value(), missing_error) || missing_error) {
                return CallToolResult::fromError(missingSettingResource(setting, text, std::nullopt));
            }
        }
    }

    std::error_code root_error;
    const auto root = std::filesystem::current_path(root_error);
    if (root_error) {
        return CallToolResult::errorJson(500, "The project root cannot be resolved for an offline setting write");
    }
    auto written = offline::writeProjectSetting(
        root, setting, remove ? json() : args["value"], remove);
    if (written.isErr()) {
        // The writer already says which kind of failure this is -- 404 for a
        // removal of a setting that is not there, 400 for a value it cannot
        // render -- and answering with only its message threw that away (#548).
        return CallToolResult::fromError(written.error(),
                                         "Failed to persist a project setting: ");
    }

    const auto& report = written.value();
    // What the file holds now, read back after the write rather than the
    // literal this call rendered (#1019). Null after a removal.
    auto stored = offline::readProjectSetting(root, setting);
    if (stored.isErr()) {
        return CallToolResult::fromError(
            Error(500, "project.godot was written and could not be read back to confirm it: " +
                           stored.error().message));
    }
    json payload{
        {"status", "success"},
        {"setting", report.setting},
        {"persisted", true},
        {"removed", report.removed},
        {"execution_mode", "offline_fallback"},
        {"is_live_engine", false},
        {"written_to", "res://project.godot"},
        {"section", report.section},
        {"key", report.key},
        {"section_created", report.section_created},
        {"replaced_existing", report.existed},
        // The live path answers this with ProjectSettings.has_setting and
        // refuses an unknown name unless create says otherwise. There is no
        // engine here, and the shipped class reference publishes no
        // ProjectSettings property list, so a misspelled built-in name is
        // genuinely unanswerable offline. Unknown is reported as unknown
        // rather than as a pass (#464).
        {"defined_by_engine", json(nullptr)},
        // Say what went into the file. An offline write has no engine to
        // confirm it against, so the literal is the evidence that the value
        // arrived as the caller meant it, not as a string that looks like it.
        {"value_written", stored.value().existed ? json(stored.value().literal) : json(nullptr)},
        // Nothing is running to read this. A setting that gates engine
        // start-up, editor_plugins/enabled above all, takes effect when Godot
        // is next launched and not before.
        {"limitation",
         "project.godot was written directly because no editor session is attached. "
         "Nothing has loaded the new value yet; it takes effect the next time Godot "
         "starts. If a Godot editor is running on this project without the Didi addon, "
         "close it before writing, because saving its own settings would overwrite this. "
         "The setting name was not checked against the engine, because there is no engine "
         "attached to ask, so defined_by_engine is null: a misspelled built-in name is "
         "written here exactly as a deliberate custom setting would be. Attach an editor "
         "to have the name checked."}
    };
    if (report.existed) payload["previous_value"] = report.previous_literal;
    // With an editor attached a string for an int, bool or array setting is
    // refused as a type mismatch. Offline there is no engine to ask what the
    // setting holds, and a String setting can hold "1152", so the text is
    // written as sent. What it reads as is said, with the typed form to send
    // instead: a stringified editor_plugins/enabled wrote a plugin list the
    // editor never loads, and nothing in the answer said why (#1016).
    if (!remove && args["value"].is_string()) {
        const auto typed = json::parse(args["value"].get_ref<const std::string&>(), nullptr, false);
        const char* reads_as = typed.is_boolean() ? "a boolean"
                               : typed.is_number() ? "a number"
                               : typed.is_array()  ? "an array"
                               : typed.is_object() ? "an object"
                                                   : nullptr;
        if (reads_as != nullptr) {
            payload["value_text_reads_as"] = typed;
            payload["retry_with"] = {{"value", typed}};
            payload["limitation"] =
                payload["limitation"].get<std::string>() + " value arrived as text that reads as " +
                reads_as + ", and it was written as a String, quotes included. If the setting "
                "holds " + reads_as + ", send the value as that JSON type, not as text: "
                "retry_with carries it.";
        }
    }
    return CallToolResult::successJson(std::move(payload));
}

CallToolResult handleProjectSearchText(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    (void)ipc;
    auto options = parseSearchOptions(args);
    // A reference lives in more formats than a declaration does, so text search
    // reads every project text format unless the caller narrows it.
    if (options.isOk() && !args.contains("extensions")) {
        options.value().extensions = offline::defaultTextSearchExtensions();
    }
    if (options.isErr()) return searchError(options.error());
    offline::ProjectSearch search(std::filesystem::current_path());
    auto result = search.searchText(options.value());
    if (result.isErr()) return searchError(result.error());
    return CallToolResult::successJson(result.value().toJson());
}

CallToolResult handleProjectSearchSymbols(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    (void)ipc;
    auto common = parseSearchOptions(args);
    if (common.isErr()) return searchError(common.error());
    offline::SymbolSearchOptions options;
    static_cast<offline::SearchOptions&>(options) = common.value();
    if (args.contains("match")) {
        if (!args["match"].is_string()) return searchError(Error::invalidArgument("match must be a string"));
        const auto value = args["match"].get<std::string>();
        if (value == "exact") options.match = offline::SymbolMatch::Exact;
        else if (value == "prefix") options.match = offline::SymbolMatch::Prefix;
        else if (value == "contains") options.match = offline::SymbolMatch::Contains;
        else return searchError(Error::invalidArgument("match must be exact, prefix, or contains"));
    }
    if (args.contains("kinds")) {
        if (!args["kinds"].is_array() || args["kinds"].empty()) {
            return searchError(Error::invalidArgument("kinds must be a non-empty array"));
        }
        static const std::set<std::string> allowed = {
            "class", "function", "signal", "variable", "constant", "enum"
        };
        options.kinds.clear();
        for (const auto& value : args["kinds"]) {
            if (!value.is_string() || !allowed.count(value.get<std::string>())) {
                return searchError(Error::invalidArgument("kinds contains an unsupported symbol kind"));
            }
            options.kinds.push_back(value.get<std::string>());
        }
    }
    offline::ProjectSearch search(std::filesystem::current_path());
    auto result = search.searchSymbols(options);
    if (result.isErr()) return searchError(result.error());
    return CallToolResult::successJson(result.value().toJson());
}

namespace {

using namespace output_schema;

json projectSearchOutputSchema(const std::string& name) {
    // The two positions carry prose because the guess is load-bearing:
    // column was a byte offset and nothing said so, which is wrong for
    // the one use a column has on any line with a non-ASCII character
    // before the match (#556).
    json match_properties = {
        {"path", string_type},
        {"line", {{"type", "integer"}, {"description", "1-based line of the match."}}},
        {"column", {{"type", "integer"},
                    {"description", "1-based column of the match in Unicode code points, "
                                    "the way an editor's goto line:col counts. Not a byte "
                                    "offset."}}},
        {"preview", string_type}};
    if (name == "project_search_symbols") {
        match_properties["name"] = string_type;
        match_properties["kind"] = string_type;
        match_properties["language"] = string_type;
    }
    return object_schema(
        {{"execution_mode", string_type},
         {"matches", array_of(object_schema(std::move(match_properties), {"path"}))},
         {"truncated", boolean_type},
         {"lexical", boolean_type},
         {"search_kind", string_type},
         {"project_root", string_type},
         {"scanned_files", integer_type},
         {"scanned_bytes", integer_type},
         {"skipped_files", integer_type},
         // Returned on every call and declared on none, which is the same
         // defect as #510 one tool over: a caller cannot see from the
         // contract that the search told it what it could not read.
         {"unsearchable_files", integer_type},
         {"unsearchable_extensions", array_of(string_type)},
         {"diagnostics", {{"type", "array"}}}},
        {"execution_mode", "matches"});
}

}  // namespace

// The tools whose handlers this file holds. registerAllDefaultTools calls
// each domain's in turn (#1256).
void ToolRegistry::registerProjectTools() {
    {
        ToolDefinition t;
        t.name = "project_search_text";
        t.description = "Searches literal text in the bounded project-owned text files a Godot project keeps references in: .gd, .cs, .tscn, .tres, .gdshader, .gdshaderinc, .godot, .cfg, .json and .import, without opening a Godot session. The result reports how many files were not candidates and which extensions they had, so an empty result can be told apart from a string the project does not contain.";
        t.inputSchema = {{"type", "object"}, {"properties", {
            {"query", {{"type", "string"}, {"minLength", 1}, {"maxLength", 256}}},
            {"search_path", {{"type", "string"}, {"default", "res://"}, {"minLength", 6}, {"maxLength", 1024}}},
            {"extensions", {{"type", "array"}, {"minItems", 1}, {"maxItems", 10}, {"uniqueItems", true},
                            {"items", {{"type", "string"},
                                       {"enum", {".gd", ".cs", ".tscn", ".tres", ".gdshader",
                                                 ".gdshaderinc", ".godot", ".cfg", ".json",
                                                 ".import"}}}}}},
            {"case_sensitive", {{"type", "boolean"}, {"default", true}}},
            {"whole_word", {{"type", "boolean"}, {"default", false}}},
            {"max_results", {{"type", "integer"}, {"default", 100}, {"minimum", 1}, {"maximum", 500}}}
        }}, {"required", {"query"}}};
        t.handler = [this](const json& args) { return handleProjectSearchText(args, m_ipcClient); };
        t.outputSchema = projectSearchOutputSchema("project_search_text");
        registerTool(std::move(t));
    }
    {
        ToolDefinition t;
        t.name = "project_search_symbols";
        t.description = "Lexically searches bounded GDScript and C# declarations without opening a Godot session.";
        t.inputSchema = {{"type", "object"}, {"properties", {
            {"query", {{"type", "string"}, {"minLength", 1}, {"maxLength", 256}}},
            {"search_path", {{"type", "string"}, {"default", "res://"}, {"minLength", 6}, {"maxLength", 1024}}},
            {"extensions", {{"type", "array"}, {"minItems", 1}, {"maxItems", 4}, {"uniqueItems", true},
                            {"items", {{"type", "string"}, {"enum", {".gd", ".cs", ".tscn", ".tres"}}}}}},
            {"case_sensitive", {{"type", "boolean"}, {"default", true}}},
            {"max_results", {{"type", "integer"}, {"default", 100}, {"minimum", 1}, {"maximum", 500}}},
            {"match", {{"type", "string"}, {"default", "prefix"}, {"enum", {"exact", "prefix", "contains"}}}},
            {"kinds", {{"type", "array"}, {"minItems", 1}, {"maxItems", 6}, {"uniqueItems", true},
                       {"items", {{"type", "string"}, {"enum", {"class", "function", "signal", "variable", "constant", "enum"}}}}}}
        }}, {"required", {"query"}}};
        t.handler = [this](const json& args) { return handleProjectSearchSymbols(args, m_ipcClient); };
        t.outputSchema = projectSearchOutputSchema("project_search_symbols");
        registerTool(std::move(t));
    }

    registerPhaseTwo(
        "project_list_autoloads", "Lists persisted project autoload entries.",
        {{"type", "object"}, {"properties", json::object()}},
        [this](const json& args) { return handleProjectListAutoloads(args, m_ipcClient); });
    registerPhaseTwo(
        "project_set_autoload", "Creates or explicitly replaces a persisted project autoload.",
        {{"type", "object"}, {"properties", {
            {"name", {{"type", "string"}}}, {"path", {{"type", "string"}}},
            {"singleton", {{"type", "boolean"}, {"default", true}}},
            {"replace", {{"type", "boolean"}, {"default", false}}}
        }}, {"required", {"name", "path"}}},
        [this](const json& args) { return handleProjectSetAutoload(args, m_ipcClient); });
    registerPhaseTwo(
        "project_remove_autoload", "Removes an existing persisted project autoload.",
        {{"type", "object"}, {"properties", {{"name", {{"type", "string"}}}}}, {"required", {"name"}}},
        [this](const json& args) { return handleProjectRemoveAutoload(args, m_ipcClient); });

    registerPhaseTwo(
        "project_list_input_actions",
        "Lists the project's InputMap actions and their events, each marked defined_by_project. "
        "include_engine_defaults: true adds the engine's ui_* map, and action reads one by name.",
        {{"type", "object"}, {"properties", {
            {"include_engine_defaults", {{"type", "boolean"}}},
            {"action", {{"type", "string"}, {"minLength", 1}}}}}},
        [this](const json& args) { return handleProjectListInputActions(args, m_ipcClient); });
    registerPhaseTwo(
        "project_set_input_action", "Creates or explicitly replaces a persisted InputMap action.",
        {{"type", "object"}, {"properties", {
            {"action", {{"type", "string"}}},
            {"deadzone", {{"type", "number"}, {"minimum", 0.0}, {"maximum", 1.0}, {"default", 0.2}}},
            // The handler is closed: it refuses an unknown property, an
            // unsupported type, a missing type and a non-integer keycode, every
            // time. None of that was published -- items was {"type": "object"},
            // which says an event is any object at all -- so a host validating
            // against the schema sent whatever the model invented and learned
            // the vocabulary one round trip at a time (#736). runtime_inject_input
            // publishes a oneOf over its own event shapes; this is the same
            // thing for the four this tool takes. They are different lists on
            // purpose: an InputMap binding has no pressed state and no mouse
            // motion, and an injected event has no persistence.
            {"events", {{"type", "array"}, {"minItems", 0}, {"maxItems", 64},
                {"description",
                 "The input events bound to the action, each a closed object in Godot's "
                 "InputEvent shape."},
                {"items", {{"oneOf", json::array({
                    json{{"type", "object"}, {"additionalProperties", false},
                         {"properties", {
                             {"type", {{"const", "key"}}},
                             {"keycode", {{"type", "integer"}, {"minimum", 1},
                                          {"maximum", 4294967295},
                                          {"description",
                                           "A Godot Key enum value, not an ASCII code or a "
                                           "character. Space is 32, Escape is 4194305, A is 65, "
                                           "F1 is 4194332; script_reflect_class on Key lists "
                                           "them all."}}},
                             {"physical_keycode", {{"type", "integer"}, {"minimum", 1},
                                                   {"maximum", 4294967295},
                                                   {"description",
                                                    "The same Key enum, read by physical position "
                                                    "rather than by the active layout."}}},
                             {"unicode", {{"type", "integer"}, {"minimum", 1},
                                          {"maximum", 1114111}}},
                             {"shift_pressed", {{"type", "boolean"}}},
                             {"alt_pressed", {{"type", "boolean"}}},
                             {"ctrl_pressed", {{"type", "boolean"}}},
                             {"meta_pressed", {{"type", "boolean"}}},
                             {"shift", {{"type", "boolean"},
                                        {"description", "Alias for shift_pressed. Send one."}}},
                             {"alt", {{"type", "boolean"},
                                      {"description", "Alias for alt_pressed. Send one."}}},
                             {"ctrl", {{"type", "boolean"},
                                       {"description", "Alias for ctrl_pressed. Send one."}}},
                             {"meta", {{"type", "boolean"},
                                       {"description", "Alias for meta_pressed. Send one."}}},
                             {"device", {{"type", "integer"}, {"minimum", -1}}}
                         }},
                         {"required", json::array({"type"})},
                         {"anyOf", json::array({
                             json{{"required", json::array({"keycode"})}},
                             json{{"required", json::array({"physical_keycode"})}},
                             json{{"required", json::array({"unicode"})}}
                         })}},
                    json{{"type", "object"}, {"additionalProperties", false},
                         {"properties", {
                             {"type", {{"const", "mouse_button"}}},
                             {"button_index", {{"type", "integer"}, {"minimum", 1},
                                               {"maximum", 9}}},
                             {"device", {{"type", "integer"}, {"minimum", -1}}}
                         }},
                         {"required", json::array({"type", "button_index"})}},
                    json{{"type", "object"}, {"additionalProperties", false},
                         {"properties", {
                             {"type", {{"const", "joypad_button"}}},
                             {"button_index", {{"type", "integer"}, {"minimum", 0},
                                               {"maximum", 127}}},
                             {"device", {{"type", "integer"}, {"minimum", -1}}}
                         }},
                         {"required", json::array({"type", "button_index"})}},
                    json{{"type", "object"}, {"additionalProperties", false},
                         {"properties", {
                             {"type", {{"const", "joypad_motion"}}},
                             {"axis", {{"type", "integer"}, {"minimum", 0}, {"maximum", 9}}},
                             {"axis_value", {{"type", "number"}, {"minimum", -1},
                                             {"maximum", 1}}},
                             {"device", {{"type", "integer"}, {"minimum", -1}}}
                         }},
                         {"required", json::array({"type", "axis", "axis_value"})}}
                })}}}}},
            {"replace", {{"type", "boolean"}, {"default", false}}}
        }}, {"required", {"action"}}},
        [this](const json& args) { return handleProjectSetInputAction(args, m_ipcClient); });
    registerPhaseTwo(
        "project_remove_input_action", "Removes an existing persisted InputMap action.",
        {{"type", "object"}, {"properties", {{"action", {{"type", "string"}}}}}, {"required", {"action"}}},
        [this](const json& args) { return handleProjectRemoveInputAction(args, m_ipcClient); });

    registerPhaseTwo(
        "project_get_setting", "Reads an existing ProjectSettings value as bounded JSON.",
        {{"type", "object"}, {"properties", {{"setting", {{"type", "string"}}}}}, {"required", {"setting"}}},
        [this](const json& args) { return handleProjectGetSetting(args, m_ipcClient); });
    registerPhaseTwo(
        "project_set_setting", "Persists or explicitly removes a ProjectSettings value. With an editor attached, a name the engine does not define is refused unless create says otherwise, because a typo and a deliberate custom setting were written identically. Offline there is no engine to ask, so the name is written unchecked and the result says so in limitation. A res:// path in the value, and each one in an array, must name a file in the project; internationalization/locale/translations takes the .translation files a CSV import writes, or a .po, .mo or .res, and never the CSV itself.",
        {{"type", "object"}, {"properties", {
            {"setting", {{"type", "string"},
                         {"description", "Slash-delimited ProjectSettings name, such as display/window/size/viewport_width. Use the typed autoload and InputMap tools for those namespaces."}}},
            {"value", {{"type", anyJsonType()}}},
            {"remove", {{"type", "boolean"}, {"default", false},
                        {"description", "Remove the setting instead of writing a value. Pass this or value, not both."}}},
            {"create", {{"type", "boolean"}, {"default", false},
                        {"description", "Write a setting name the engine does not already define. Off by default, because a misspelled built-in name is indistinguishable from a deliberate custom one and costs a key nothing reads. Only an attached editor can check the name, and the result then reports defined_by_engine. Offline the check cannot run: the name is written whether create is set or not, defined_by_engine is null, and limitation says so. Attach an editor to have the name checked."}}}
        }}, {"required", {"setting"}}},
        [this](const json& args) { return handleProjectSetSetting(args, m_ipcClient); },
        // The files an array names need no engine, so both routes and a dry run
        // refuse the same values (#989).
        checkSettingValuePaths);
}

} // namespace didi::mcp
