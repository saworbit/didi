#include "didi/mcp/project_tools.hpp"
#include "didi/offline/project_search.hpp"
#include "didi/offline/project_settings_file.hpp"

#include <filesystem>
#include <set>

namespace didi::mcp {
namespace {

CallToolResult forwardLiveProject(const json& args,
                                  const std::shared_ptr<ipc::IIpcClient>& ipc,
                                  const char* method,
                                  const char* operation) {
    if (!ipc || !ipc->isConnected()) {
        return CallToolResult::error(std::string("Godot Editor is offline. Launch Godot to ") + operation + ".");
    }
    auto response = ipc->sendRequest(method, args, ipc::kWaitForDefinitiveResponse);
    if (response.isErr()) {
        return CallToolResult::error(std::string("Failed to ") + operation + ": " + response.error().message);
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
        return CallToolResult::error("Invalid autoload list request: this tool takes no arguments");
    }
    std::error_code root_error;
    const auto root = std::filesystem::current_path(root_error);
    if (root_error) {
        return CallToolResult::error("The project root cannot be resolved for an offline autoload read");
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
        return CallToolResult::error("Invalid project setting request: arguments must be an object");
    }
    const std::string setting = args.value("setting", "");
    std::error_code root_error;
    const auto root = std::filesystem::current_path(root_error);
    if (root_error) {
        return CallToolResult::error("The project root cannot be resolved for an offline setting read");
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
        return CallToolResult::error("Invalid project setting request: arguments must be an object");
    }
    const std::string setting = args.value("setting", "");
    const bool remove = args.value("remove", false);
    if (remove && args.contains("value")) {
        return CallToolResult::error("Specify either value or remove: true, not both");
    }
    if (!remove && !args.contains("value")) {
        return CallToolResult::errorJson(400, "value is required unless remove is true");
    }

    std::error_code root_error;
    const auto root = std::filesystem::current_path(root_error);
    if (root_error) {
        return CallToolResult::error("The project root cannot be resolved for an offline setting write");
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
        {"value_written", report.literal},
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

} // namespace didi::mcp
