#include "didi/mcp/project_tools.hpp"
#include "didi/offline/project_search.hpp"

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
    return CallToolResult::error("Invalid project search request: " + error.message);
}

} // namespace

CallToolResult handleProjectListAutoloads(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    return forwardLiveProject(args, ipc, "project.listAutoloads", "list project autoloads");
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
CallToolResult handleProjectGetSetting(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    return forwardLiveProject(args, ipc, "project.getSetting", "read a project setting");
}
CallToolResult handleProjectSetSetting(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    return forwardLiveProject(args, ipc, "project.setSetting", "persist a project setting");
}

CallToolResult handleProjectSearchText(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    (void)ipc;
    auto options = parseSearchOptions(args);
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
