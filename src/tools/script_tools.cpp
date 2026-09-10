#include "didi/mcp/mcp_protocol.hpp"
#include "didi/common/ipc_channel.hpp"
#include "didi/common/logger.hpp"
#include "didi/common/project_path.hpp"
#include "didi/offline/gdscript_diagnostics.hpp"
#include "didi/offline/resource_indexer.hpp"
#include "didi/runtime/session_client.hpp"
#include "didi/common/atomic_write.hpp"
#include <cctype>
#include <fstream>
#include <sstream>
#include <filesystem>

namespace didi {
namespace mcp {

CallToolResult handleScriptCheckSyntax(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    (void)ipc;
    std::string file_path = args.value("file_path", "");
    std::string source_text = args.value("source_text", "");

    if (file_path.empty() && source_text.empty()) {
        return CallToolResult::error("Parameter 'file_path' or 'source_text' is required.");
    }

    std::string analysis_path = file_path;
    if (source_text.empty() && !file_path.empty()) {
        auto resolved = paths::resolveProjectFile(file_path);
        if (resolved.isErr()) {
            return CallToolResult::error("Invalid script file path: " + resolved.error().message);
        }
        analysis_path = paths::projectPathToUtf8(resolved.value());
    }

    auto diags = offline::GDScriptDiagnostics::analyze(analysis_path, source_text);
    json diag_arr = json::array();
    bool has_error = false;
    for (const auto& d : diags) {
        if (d.severity == "error") has_error = true;
        diag_arr.push_back(d.toJson());
    }

    json result = {
        {"file_path", file_path},
        {"diagnostics_count", diags.size()},
        {"has_errors", has_error},
        {"diagnostics", diag_arr}
    };

    return CallToolResult::successJson(result);
}

CallToolResult handleScriptCreate(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    (void)ipc;
    const std::string script_path = args.value("script_path", args.value("file_path", ""));
    if (script_path.empty()) {
        return CallToolResult::error(
            "Parameter 'script_path' is required (e.g. res://scripts/player.gd).");
    }
    if (!args.contains("source_text") || !args["source_text"].is_string()) {
        return CallToolResult::error("Parameter 'source_text' is required and must be a string.");
    }
    if (args.contains("overwrite") && !args["overwrite"].is_boolean()) {
        return CallToolResult::error("Parameter 'overwrite' must be a boolean.");
    }
    const std::string source_text = args["source_text"].get<std::string>();
    const bool overwrite = args.value("overwrite", false);

    std::string extension;
    const auto dot = script_path.find_last_of('.');
    const auto slash = script_path.find_last_of('/');
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) {
        extension = script_path.substr(dot);
        for (auto& character : extension) {
            character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
        }
    }
    if (extension != ".gd") {
        return CallToolResult::error(
            "script_create writes GDScript, so script_path must end in .gd; received \"" +
            script_path + "\".");
    }

    namespace fs = std::filesystem;
    auto resolved = paths::resolveProjectFileForWrite(script_path);
    if (resolved.isErr()) {
        return CallToolResult::error("Invalid script file path: " + resolved.error().message);
    }
    const fs::path disk_path = resolved.value();

    std::error_code probe_error;
    const bool already_there = fs::is_regular_file(disk_path, probe_error) && !probe_error;
    if (already_there && !overwrite) {
        return CallToolResult::error(
            "Script already exists; pass overwrite: true to replace it: " + script_path);
    }
    if (disk_path.has_parent_path()) {
        std::error_code directory_error;
        fs::create_directories(disk_path.parent_path(), directory_error);
        if (directory_error) {
            return CallToolResult::error("Cannot create the directory for " + script_path + ": " +
                                         directory_error.message());
        }
    }

    auto written = files::writeFileAtomically(disk_path, source_text);
    if (written.isErr()) {
        return CallToolResult::error("Cannot write the script to disk: " + script_path + ": " +
                                     written.error().message);
    }
    offline::ResourceIndexer::invalidateSharedIndex();

    // Same check script_patch_method runs after its write, and against the file
    // on disk for the same reason: a bad script should be visible here rather
    // than at attach time.
    //
    // The res:// path, not the absolute one. A narrow string of an absolute
    // path goes through the active code page on Windows and throws for anything
    // the code page cannot hold, and Godot reports its errors against res://,
    // which is what the location patterns here match.
    auto diags = offline::GDScriptDiagnostics::analyze(script_path);
    json diag_arr = json::array();
    bool has_error = false;
    for (const auto& d : diags) {
        if (d.severity == "error") has_error = true;
        diag_arr.push_back(d.toJson());
    }

    return CallToolResult::successJson({
        {"status", already_there ? "replaced_offline" : "created_offline"},
        {"script_path", script_path},
        {"bytes_written", source_text.size()},
        {"diagnostics_count", diags.size()},
        {"has_errors", has_error},
        {"diagnostics", diag_arr}
    });
}

namespace {

// "Godot Engine v4.7.stable.official" and "Godot v4.5.1.stable.official" are
// the two spellings in play, one from the shipped dump's header and one from
// the engine itself. Only the major and minor decide whether the API a caller
// is reading matches the engine in front of them, so only those are compared;
// a patch difference is not a mismatch worth shouting about.
std::string majorMinorOf(const std::string& version) {
    const auto v = version.find_first_of("0123456789");
    if (v == std::string::npos) return {};
    const auto dot = version.find('.', v);
    if (dot == std::string::npos) return {};
    const auto end = version.find_first_not_of("0123456789", dot + 1);
    return version.substr(v, (end == std::string::npos ? version.size() : end) - v);
}

} // namespace

CallToolResult handleScriptReflectClass(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    std::string class_name = args.value("class_name", "");
    if (class_name.empty()) {
        return CallToolResult::error("Parameter 'class_name' is required.");
    }

    // Run offline class reflection
    json doc = offline::GDScriptDiagnostics::reflectClass(class_name);

    // A caller with an editor open was handed one engine's method and property
    // sets while running another, with nothing saying so (#405). Reading the
    // selected session's descriptor takes no route and changes no selection,
    // which is what an offline-only tool is allowed to do.
    const auto sessions = std::dynamic_pointer_cast<runtime::IRuntimeSessionClient>(ipc);
    const auto attached = sessions ? sessions->activeSession()
                                   : std::optional<runtime::SessionDescriptor>{};
    if (attached.has_value() && doc.contains("api_version") && doc["api_version"].is_string()) {
        const auto& engine = attached->engine_version;
        const auto pinned_line = majorMinorOf(doc["api_version"].get<std::string>());
        const auto engine_line = majorMinorOf(engine);
        doc["attached_engine_version"] = engine.empty() ? json(nullptr) : json(engine);
        if (engine_line.empty() || pinned_line.empty()) {
            // An extension older than the field publishes no version. Unknown
            // is not a match, and saying nothing would read as one.
            doc["api_version_matches_attached_engine"] = nullptr;
        } else {
            doc["api_version_matches_attached_engine"] = pinned_line == engine_line;
        }
    }
    return CallToolResult::successJson(doc);
}

CallToolResult handleScriptGetSymbols(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    (void)ipc;
    std::string file_path = args.value("file_path", "");
    std::string source_text = args.value("source_text", "");

    if (source_text.empty() && !file_path.empty()) {
        auto resolved = paths::resolveProjectFile(file_path);
        if (resolved.isErr()) {
            return CallToolResult::error("Invalid script file path: " + resolved.error().message);
        }
        std::ifstream file(resolved.value());
        if (file.is_open()) {
            std::stringstream ss;
            ss << file.rdbuf();
            source_text = ss.str();
        }
    }

    if (source_text.empty()) {
        return CallToolResult::error("No source text or valid script file found for symbol extraction.");
    }

    json syms = offline::GDScriptDiagnostics::extractSymbols(source_text);
    syms["file_path"] = file_path;
    return CallToolResult::successJson(syms);
}

CallToolResult handleScriptPatchMethod(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    (void)ipc;
    std::string file_path = args.value("file_path", "");
    std::string symbol_name = args.value("method_name", args.value("symbol_name", ""));
    std::string new_definition = args.value("new_definition", "");
    std::string symbol_type = args.value("symbol_type", "function");

    if (file_path.empty() || symbol_name.empty() || new_definition.empty()) {
        return CallToolResult::error("Parameters 'file_path', 'method_name'/'symbol_name', and 'new_definition' are required.");
    }

    namespace fs = std::filesystem;
    auto resolved = paths::resolveProjectFile(file_path);
    if (resolved.isErr()) {
        return CallToolResult::error("Invalid script file path: " + resolved.error().message);
    }
    const fs::path disk_path = resolved.value();

    std::string original_content;
    std::ifstream in_file(disk_path);
    if (in_file.is_open()) {
        std::stringstream ss;
        ss << in_file.rdbuf();
        original_content = ss.str();
        in_file.close();
    } else {
        return CallToolResult::error("Cannot open file for method patching: " + file_path);
    }

    auto patch_res = offline::GDScriptDiagnostics::patchSymbol(original_content, symbol_name, new_definition, symbol_type);
    if (patch_res.isErr()) {
        return CallToolResult::error("Patching error: " + patch_res.error().message);
    }

    std::string patched_content = patch_res.value();
    auto written = files::writeFileAtomically(disk_path, patched_content);
    if (written.isErr()) {
        return CallToolResult::error("Cannot write patched file to disk: " + file_path +
                                     ": " + written.error().message);
    }
    offline::ResourceIndexer::invalidateSharedIndex();

    // Verify syntax of the patched file.
    //
    // Analyze against the file just written rather than against the content in
    // hand. `analyze` only runs the Godot compiler check when no source_text is
    // supplied, so passing `patched_content` here silently reduced this to the
    // lexical rules: a patch that left the file unparseable still came back
    // with `has_errors: false` and an empty diagnostics array, and only a
    // separate `script_check_syntax` call revealed it. The write above already
    // put this exact content on disk, so the file is the same evidence.
    //
    // The res:// path for the same reason script_create passes it: a narrow
    // absolute path throws on Windows for characters outside the code page, and
    // Godot's diagnostics name res:// paths.
    auto diags = offline::GDScriptDiagnostics::analyze(file_path);
    json diag_arr = json::array();
    bool has_error = false;
    for (const auto& d : diags) {
        if (d.severity == "error") has_error = true;
        diag_arr.push_back(d.toJson());
    }

    json result = {
        {"status", "success"},
        {"file_path", file_path},
        {"method_name", symbol_name},
        {"has_errors", has_error},
        {"diagnostics", diag_arr}
    };

    return CallToolResult::successJson(result);
}

static CallToolResult forwardLiveScriptWiring(const json& args,
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

CallToolResult handleScriptAttachToNode(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    return forwardLiveScriptWiring(args, ipc, "script.attachToNode", "attach a script to a node");
}

CallToolResult handleScriptDetachFromNode(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    return forwardLiveScriptWiring(args, ipc, "script.detachFromNode", "detach a script from a node");
}

} // namespace mcp
} // namespace didi
