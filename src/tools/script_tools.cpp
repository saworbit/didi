#include "didi/mcp/mcp_protocol.hpp"
#include "didi/common/ipc_channel.hpp"
#include "didi/common/logger.hpp"
#include "didi/offline/gdscript_diagnostics.hpp"
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

    // Run offline analysis
    auto diags = offline::GDScriptDiagnostics::analyze(file_path, source_text);
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

CallToolResult handleScriptReflectClass(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    (void)ipc;
    std::string class_name = args.value("class_name", "");
    if (class_name.empty()) {
        return CallToolResult::error("Parameter 'class_name' is required.");
    }

    // Run offline class reflection
    json doc = offline::GDScriptDiagnostics::reflectClass(class_name);
    return CallToolResult::successJson(doc);
}

CallToolResult handleScriptGetSymbols(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    (void)ipc;
    std::string file_path = args.value("file_path", "");
    std::string source_text = args.value("source_text", "");

    if (source_text.empty() && !file_path.empty()) {
        std::string disk_path = file_path;
        if (strings::startsWith(disk_path, "res://")) disk_path = disk_path.substr(6);
        std::ifstream file(disk_path);
        if (!file.is_open() && std::filesystem::exists("demo/" + disk_path)) {
            disk_path = "demo/" + disk_path;
            file.open(disk_path);
        }
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

    std::string disk_path = file_path;
    if (strings::startsWith(disk_path, "res://")) {
        disk_path = disk_path.substr(6);
    }

    namespace fs = std::filesystem;
    fs::path target_p(disk_path);
    fs::path current_root = fs::current_path();

    try {
        auto canon_root = fs::weakly_canonical(current_root);
        auto canon_target = fs::weakly_canonical(current_root / target_p);
        auto [root_it, target_it] = std::mismatch(
            canon_root.begin(), canon_root.end(),
            canon_target.begin(), canon_target.end()
        );
        if (root_it != canon_root.end()) {
            return CallToolResult::error("Access denied: file path is outside the project root directory.");
        }
    } catch (const std::exception& e) {
        return CallToolResult::error(std::string("Path resolution error: ") + e.what());
    }

    std::string original_content;
    std::ifstream in_file(disk_path);
    if (!in_file.is_open() && fs::exists("demo/" + disk_path)) {
        disk_path = "demo/" + disk_path;
        in_file.open(disk_path);
    }

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
    std::ofstream out_file(disk_path, std::ios::trunc);
    if (!out_file.is_open()) {
        return CallToolResult::error("Cannot write patched file to disk: " + disk_path);
    }
    out_file << patched_content;
    out_file.close();

    // Verify syntax of the patched file
    auto diags = offline::GDScriptDiagnostics::analyze(file_path, patched_content);
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

} // namespace mcp
} // namespace didi
