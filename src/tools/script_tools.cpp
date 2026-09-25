#include "didi/mcp/mcp_protocol.hpp"
#include "didi/common/filesystem_status.hpp"
#include "didi/common/ipc_channel.hpp"
#include "didi/common/logger.hpp"
#include "didi/common/project_path.hpp"
#include "didi/common/engine_version.hpp"
#include "didi/offline/gdscript_diagnostics.hpp"
#include "didi/offline/project_search.hpp"
#include "didi/offline/test_runner.hpp"
#include "didi/offline/project_settings_file.hpp"
#include "didi/offline/project_file_lock.hpp"
#include "didi/offline/resource_indexer.hpp"
#include "didi/runtime/session_client.hpp"
#include "didi/common/atomic_write.hpp"
#include <cctype>
#include <fstream>
#include <sstream>
#include <optional>
#include <string>
#include <vector>
#include <filesystem>

namespace didi {
namespace mcp {

// Godot refuses to load a script whose bytes are not valid UTF-8, and says so:
// "contains invalid unicode (UTF-8), so it was not loaded". A .gd saved as
// UTF-16 or ANSI is not exotic on Windows -- it is what happens when a script
// is opened and saved by an editor that is not Godot -- and both tools below
// answered about such a file as though they had read it (#613, #614).
// project_search_text has classified these since it was written, so the
// classifier is the one it uses.
std::optional<std::string> scriptEncodingRefusal(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return std::nullopt;
    std::ostringstream buffer;
    buffer << input.rdbuf();
    const auto contents = buffer.str();
    if (contents.empty() || offline::isValidUtf8Text(contents)) return std::nullopt;
    return std::string(
        "This file is not valid UTF-8, so Godot will not load it as a script. It is most "
        "likely saved as UTF-16 or in a single-byte encoding; save it as UTF-8 and try "
        "again.");
}

// The file is there and this process may not read it: chmod 000 on Unix, an ACL
// on Windows, or a file another process holds without sharing. It is what a
// file copied out of another user's home, restored from an archive, or dropped
// by a container build looks like.
//
// script_check_syntax used to report it as a syntax error at line 1 column 1 of
// a file whose bytes were never read, with isError: false, has_errors: true and
// rule "file_not_found" about a file that is found; an assistant acting on that
// goes and edits line 1. script_get_symbols reported it as 400
// invalid_arguments, about arguments that were fine. Both were the inverse of
// the absent case, which answers 404, so the state a chmod fixes was the one
// that read like a code problem (#653).
//
// `reason` carries project_search_text's word for the same state, which is the
// vocabulary two sibling readers of the same tree should not each invent, and
// the path is the res:// spelling rather than the absolute host path.
std::optional<CallToolResult> unreadableScriptRefusal(const std::filesystem::path& resolved,
                                                      const std::string& file_path) {
    std::ifstream probe(resolved, std::ios::binary);
    if (probe.is_open()) return std::nullopt;
    return CallToolResult::error(
        json{{"error",
              {{"code", 403},
               {"message", "This file is there and cannot be read: " + file_path +
                               ". Check its permissions, or whether another process is holding "
                               "it open."},
               {"data", {{"code", "forbidden"},
                         {"reason", "unreadable"},
                         {"file_path", file_path},
                         {"retryable", false}}}}}}
            .dump());
}

CallToolResult handleScriptCheckSyntax(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    std::string file_path = args.value("file_path", "");
    std::string source_text = args.value("source_text", "");

    if (file_path.empty() && source_text.empty()) {
        return CallToolResult::errorJson(
            400, "Parameter 'file_path' or 'source_text' is required.");
    }

    std::string analysis_path = file_path;
    offline::GDScriptDiagnostics::EngineCheck engine;
    std::optional<std::string> encoding_refusal;
    if (source_text.empty() && !file_path.empty()) {
        auto resolved = paths::resolveProjectFile(file_path);
        if (resolved.isErr()) {
            return CallToolResult::fromError(resolved.error(), "Invalid script file path: ");
        }
        analysis_path = paths::projectPathToUtf8(resolved.value());
        // Before the encoding question, because a file this process cannot open
        // answers neither question and used to answer both wrongly (#653).
        if (auto refused = unreadableScriptRefusal(resolved.value(), file_path)) return *refused;
        // Asked before Godot is spawned. The engine does refuse the file, but
        // its refusal has no res:// frame to hang a diagnostic on, so the
        // parser dropped it and the answer came back clean about a script the
        // engine will not load (#613).
        encoding_refusal = scriptEncodingRefusal(resolved.value());
    }

    json diag_arr = json::array();
    bool has_error = false;
    size_t diagnostics_count = 0;
    if (encoding_refusal.has_value()) {
        has_error = true;
        diagnostics_count = 1;
        diag_arr.push_back(json{{"severity", "error"},
                                {"message", *encoding_refusal},
                                {"rule", "invalid_encoding"},
                                {"line", 0},
                                {"column", 0}});
    } else {
        auto diags = offline::GDScriptDiagnostics::analyze(analysis_path, source_text, &engine);
        diagnostics_count = diags.size();
        for (const auto& d : diags) {
            if (d.severity == "error") has_error = true;
            diag_arr.push_back(d.toJson());
        }
    }

    // A compiler pass that did not happen is not a script with no errors.
    //
    // The whole question this tool answers is "does this compile?", and the
    // answer came back `has_errors: false, diagnostics: []` when the engine it
    // was told to use never launched -- a GODOT_BIN pointing at a real file
    // that is not Godot, which is what a version-manager shim or the wrong file
    // out of a bundle looks like (#677). engine_version was the only trace, and
    // nothing said the compiler had not run. shader_check_compile, the same
    // idea on the shader half, already refuses this; so does this now. A check
    // given source_text runs no engine by design and is left alone.
    const bool engine_was_asked = source_text.empty() && !file_path.empty();
    // Only when an engine was actually there to be wrong about.
    //
    // A machine with no Godot installed falls through to the bare name
    // `godot`, the exec fails with 127, and answering the lexer verdict is
    // what this tool has always done and what the output schema documents.
    // Refusing there would take the tool away from a supported configuration
    // for a misconfiguration it does not have. The value #677 is about is a
    // path that names a real file which is not the engine -- a version-manager
    // shim, the wrong file out of a bundle -- and that is the one this catches.
    const bool engine_was_wrong = engine_was_asked && engine.executable_exists && !engine.ran;
    if (engine_was_wrong && !encoding_refusal.has_value()) {
        json data = {{"code", "engine_unavailable"},
                     {"tool", "script_check_syntax"},
                     {"engine_executable", engine.executable.empty() ? json(nullptr)
                                                                     : json(engine.executable)},
                     {"engine_exit_code", engine.exit_code.has_value() ? json(*engine.exit_code)
                                                                       : json(nullptr)},
                     {"retryable", false}};
        const auto configured_engine = offline::resolveGodotExecutableDetailed();
        json error_body = {{"code", 503},
                           {"message", "Godot did not compile this script, so whether it has "
                                       "errors is unknown: " + engine.failure +
                                       ". Engine tried: " +
                                       (engine.executable.empty() ? std::string("none found")
                                                                  : engine.executable) +
                                       ". Set GODOT_BIN to a Godot executable."},
                           {"data", data}};
        versions::annotateConfiguredEngine(error_body["data"], configured_engine.configured,
                                           configured_engine.configured_rejected);
        return CallToolResult::error(json{{"error", error_body}}.dump());
    }

    json result = {
        {"file_path", file_path},
        {"diagnostics_count", diagnostics_count},
        {"has_errors", has_error},
        {"diagnostics", diag_arr}
    };
    // Whether the compiler was asked at all, which is a different fact from
    // whether it answered.
    //
    // A source_text check runs Didi's own lexical rules and nothing else, by
    // design: there is no file for `--check-only` to open. Nothing in the
    // result said so, so six scripts with real compile errors -- a typed
    // variable assigned the wrong type, a mistyped keyword, an undeclared
    // identifier, an absent method, an unknown base class, a wrong constructor
    // arity -- each came back has_errors: false, which is the same answer a
    // clean script gets (#728). Worse, the engine fields came back all-null,
    // which is byte for byte what a GODOT_BIN that cannot be launched returns,
    // so one shape stood for three states. This field separates them: false
    // means nobody asked, and engine_available answers whether an engine that
    // was asked replied.
    result["engine_checked"] = engine_was_asked;
    if (!engine_was_asked) {
        result["limitation"] =
            "The Godot compiler was not run. A source_text check has no file to compile, so "
            "has_errors covers only Didi's own lexical rules -- unbalanced brackets, bad "
            "indentation, a tab and space mix -- and not type errors, undeclared identifiers, "
            "absent methods or unknown base classes. For a compiler verdict on unsaved source, "
            "send it to project_verify_changes, which compiles it in an isolated copy of the "
            "project; for one on a file, write it with script_create and check it by file_path.";
    }
    // Whether the compiler was asked at all, which is a different fact from
    // whether it answered.
    //
    // A source_text check runs Didi's own lexical rules and nothing else, by
    // design: there is no file for `--check-only` to open. Nothing in the
    // result said so, so six scripts with real compile errors -- a typed
    // variable assigned the wrong type, a mistyped keyword, an undeclared
    // identifier, an absent method, an unknown base class, a wrong constructor
    // arity -- each came back has_errors: false, which is the same answer a
    // clean script gets (#728). Worse, the engine fields came back all-null,
    // which is byte for byte what a GODOT_BIN that cannot be launched returns,
    // so one shape stood for three states. This field separates them: false
    // means nobody asked, and engine_available answers whether an engine that
    // was asked replied.
    result["engine_checked"] = engine_was_asked;
    if (!engine_was_asked) {
        result["limitation"] =
            "The Godot compiler was not run. A source_text check has no file to compile, so "
            "has_errors covers only Didi's own lexical rules -- unbalanced brackets, bad "
            "indentation, a tab and space mix -- and not type errors, undeclared identifiers, "
            "absent methods or unknown base classes. For a compiler verdict on unsaved source, "
            "send it to project_verify_changes, which compiles it in an isolated copy of the "
            "project; for one on a file, write it with script_create and check it by file_path.";
    }
    // Published so a caller can see whether the subprocess ran at all, which
    // is what the shader half already reports and this one did not. False is
    // the honest answer on a machine with no Godot: the lexer found what it
    // found, and nothing compiled the script.
    if (engine_was_asked) {
        result["engine_available"] = engine.ran;
        result["engine_exit_code"] =
            engine.exit_code.has_value() ? json(*engine.exit_code) : json(nullptr);
        result["engine_duration_seconds"] = engine.duration_seconds;
        if (!engine.ran && !engine.failure.empty()) {
            result["engine_unavailable_reason"] = engine.failure;
        }
    }

    // Which engine answered. The whole question this tool exists for is "will
    // the engine accept this?", and resolveGodotExecutable picks newest-first
    // from a hardcoded list, so a 4.5 project could be answered about by 4.7
    // with nothing in the response saying so, and no raw_output to hide it in
    // (#617). Reading the selected session takes no route and changes no
    // selection, which is what an offline-only tool is allowed to do.
    const auto sessions = std::dynamic_pointer_cast<runtime::IRuntimeSessionClient>(ipc);
    const auto attached = sessions ? sessions->observableSession()
                                   : std::optional<runtime::SessionDescriptor>{};
    const auto configured = offline::resolveGodotExecutableDetailed();
    versions::annotateConfiguredEngine(result, configured.configured, configured.configured_rejected);
    versions::annotateCheckEngine(result, engine.version, engine.executable,
                                  attached.has_value() ? attached->engine_version
                                                       : std::string());

    return CallToolResult::successJson(result);
}

CallToolResult handleScriptCreate(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    (void)ipc;
    const std::string script_path = args.value("script_path", args.value("file_path", ""));
    if (script_path.empty()) {
        return CallToolResult::errorJson(
            400, "Parameter 'script_path' is required (e.g. res://scripts/player.gd).");
    }
    if (!args.contains("source_text") || !args["source_text"].is_string()) {
        return CallToolResult::errorJson(
            400, "Parameter 'source_text' is required and must be a string.");
    }
    if (args.contains("overwrite") && !args["overwrite"].is_boolean()) {
        return CallToolResult::errorJson(400, "Parameter 'overwrite' must be a boolean.");
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
        return CallToolResult::errorJson(
            400,
            "script_create writes GDScript, so script_path must end in .gd; received \"" +
                script_path + "\".");
    }

    namespace fs = std::filesystem;
    auto resolved = paths::resolveProjectFileForWrite(script_path);
    if (resolved.isErr()) {
        return CallToolResult::fromError(resolved.error(), "Invalid script file path: ");
    }
    const fs::path disk_path = resolved.value();
    // Reported as the readers spell it, not as the argument spelled it: the
    // resolved path, and the on-disk case when a file is already there (#546,
    // #551). Read before the write so a replaced file is named by the name it
    // had, which is the one the caller is about to lose.
    const std::string reported_path = paths::resourcePathOf(disk_path);

    std::error_code probe_error;
    const bool already_there = fs::is_regular_file(disk_path, probe_error) && !probe_error;
    if (already_there && !overwrite) {
        return CallToolResult::errorJson(
            409, "Script already exists; pass overwrite: true to replace it: " + reported_path,
            {{"code", "already_exists"}, {"retry_with", {{"overwrite", true}}}});
    }
    if (disk_path.has_parent_path()) {
        std::error_code directory_error;
        fs::create_directories(disk_path.parent_path(), directory_error);
        if (directory_error) {
            return CallToolResult::errorJson(
                files::statusForFilesystemError(directory_error),
                "Cannot create the directory for " + script_path + ": " +
                    directory_error.message());
        }
    }

    auto written = files::writeFileAtomically(disk_path, source_text);
    if (written.isErr()) {
        return CallToolResult::fromError(
            written.error(), "Cannot write the script to disk: " + script_path + ": ");
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
    auto diags = offline::GDScriptDiagnostics::analyze(reported_path);
    json diag_arr = json::array();
    bool has_error = false;
    for (const auto& d : diags) {
        if (d.severity == "error") has_error = true;
        diag_arr.push_back(d.toJson());
    }

    return CallToolResult::successJson({
        {"status", already_there ? "replaced_offline" : "created_offline"},
        {"script_path", reported_path},
        {"bytes_written", source_text.size()},
        {"diagnostics_count", diags.size()},
        {"has_errors", has_error},
        {"diagnostics", diag_arr}
    });
}

CallToolResult handleScriptReflectClass(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    std::string class_name = args.value("class_name", "");
    if (class_name.empty()) {
        return CallToolResult::errorJson(
            400, "Parameter 'class_name' is required and must not be empty.");
    }

    // Run offline class reflection
    json doc = offline::GDScriptDiagnostics::reflectClass(class_name);

    // A caller with an editor open was handed one engine's method and property
    // sets while running another, with nothing saying so (#405). Reading the
    // selected session's descriptor takes no route and changes no selection,
    // which is what an offline-only tool is allowed to do.
    const auto sessions = std::dynamic_pointer_cast<runtime::IRuntimeSessionClient>(ipc);
    const auto attached = sessions ? sessions->observableSession()
                                   : std::optional<runtime::SessionDescriptor>{};
    if (doc.contains("api_version") && doc["api_version"].is_string()) {
        const auto api_version = doc["api_version"].get<std::string>();
        if (attached.has_value()) {
            // An extension older than the field publishes no version, which
            // the shared helper reports as unknown rather than as a match.
            versions::annotateApiVersion(doc, api_version, attached->engine_version);
        } else {
            // No session selected, so no engine to ask; the project still
            // says which line saved it. A 4.5 project read against the 4.7
            // dump is the same skew either way, and before this the answer
            // depended on whether some earlier call had happened to select a
            // session (#555).
            std::error_code root_error;
            const auto root = std::filesystem::current_path(root_error);
            std::string features;
            if (!root_error) {
                auto setting = offline::readProjectSetting(root, "application/config/features");
                if (setting.isOk() && setting.value().existed) features = setting.value().literal;
            }
            versions::annotateProjectFeatures(doc, api_version, features);
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
            return CallToolResult::fromError(resolved.error(), "Invalid script file path: ");
        }
        // "this file declares nothing" and "I could not read this file" were
        // the same answer, down to truncated: false confirming nothing was
        // dropped. An agent asking where a method lives got "there is no such
        // method" and acted on it (#614).
        if (auto unreadable = unreadableScriptRefusal(resolved.value(), file_path)) {
            return *unreadable;
        }
        if (auto refused = scriptEncodingRefusal(resolved.value())) {
            return CallToolResult::error(json{{"error", {
                {"code", 415},
                {"message", *refused},
                {"data", {{"code", "binary_or_invalid_utf8"},
                          {"file_path", file_path},
                          {"retryable", false}}}}}}.dump());
        }
        std::ifstream file(resolved.value());
        if (file.is_open()) {
            std::stringstream ss;
            ss << file.rdbuf();
            source_text = ss.str();
        }
    }

    if (source_text.empty()) {
        return CallToolResult::errorJson(
            400, "No source text or valid script file found for symbol extraction.");
    }

    const auto max_symbols = static_cast<size_t>(
        args.value("max_symbols",
                   static_cast<uint64_t>(offline::GDScriptDiagnostics::kDefaultMaxSymbols)));
    json syms = offline::GDScriptDiagnostics::extractSymbols(source_text, max_symbols);
    syms["file_path"] = file_path;
    return CallToolResult::successJson(syms);
}

CallToolResult handleScriptPatchMethod(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    (void)ipc;
    std::string file_path = args.value("file_path", "");
    std::string symbol_name = args.value("method_name", args.value("symbol_name", ""));
    std::string new_definition = args.value("new_definition", "");
    std::string symbol_type = args.value("symbol_type", "function");
    const bool create_if_missing = args.value("create_if_missing", false);

    // Named one at a time, and only the published names. The old message listed
    // all three whichever was missing, and offered 'symbol_name', which is not
    // in this tool's schema and so is rejected as an unknown property.
    std::vector<std::string> missing;
    if (file_path.empty()) missing.push_back("'file_path'");
    if (symbol_name.empty()) missing.push_back("'method_name'");
    if (new_definition.empty()) missing.push_back("'new_definition'");
    if (!missing.empty()) {
        std::string names = missing.front();
        for (size_t i = 1; i < missing.size(); ++i) {
            names += (i + 1 == missing.size()) ? " and " + missing[i] : ", " + missing[i];
        }
        return CallToolResult::errorJson(
            400, "Argument " + names + " is required and must not be empty.");
    }

    namespace fs = std::filesystem;
    auto resolved = paths::resolveProjectFile(file_path);
    if (resolved.isErr()) {
        return CallToolResult::fromError(resolved.error(), "Invalid script file path: ");
    }
    const fs::path disk_path = resolved.value();
    const std::string reported_path = paths::resourcePathOf(disk_path);

    // Held from the read to the write. Two agents patching different methods
    // of one script both read the old text, and the second write replaced the
    // first while both reported success; on Windows the replace could instead
    // meet the other's open file and fail. The project writers hold the same
    // lock since #953 (#954). The lock is named by the script's res:// path,
    // which resourcePathOf spells as it is on disk, so two spellings of one
    // file on a case-insensitive filesystem share it.
    std::error_code root_error;
    const auto project_root = fs::weakly_canonical(fs::current_path(), root_error);
    if (root_error || reported_path.rfind("res://", 0) != 0) {
        return CallToolResult::errorJson(
            500, "Cannot lock " + reported_path + " for method patching: the project root "
                 "cannot be resolved");
    }
    auto lock = offline::lockProjectFile(project_root, reported_path.substr(6));
    if (lock.isErr()) return CallToolResult::fromError(lock.error());

    // Read as bytes. A text-mode read on Windows turned every CRLF into LF
    // before the patcher saw the file, and the atomic writer writes bytes, so
    // a CRLF file came back LF on every line while the result reported a
    // single-method change (#550). The patcher works in LF; the file's own
    // convention is put back on the way out, the way the settings writer
    // already does for project.godot.
    std::string original_content;
    {
        std::ifstream in_file(disk_path, std::ios::binary);
        if (!in_file.is_open()) {
            return CallToolResult::errorJson(
                500, "Cannot open file for method patching: " + reported_path);
        }
        std::stringstream ss;
        ss << in_file.rdbuf();
        original_content = ss.str();
    }
    const bool crlf = original_content.find("\r\n") != std::string::npos;
    const bool trailing_newline = !original_content.empty() && original_content.back() == '\n';
    const std::string working_content =
        crlf ? strings::replaceAll(original_content, "\r\n", "\n") : original_content;
    // The replacement arrives however the caller's client spelled its line
    // breaks; it joins the file in the file's convention either way.
    const std::string working_definition = strings::replaceAll(new_definition, "\r\n", "\n");

    auto patch_res = offline::GDScriptDiagnostics::patchSymbol(
        working_content, symbol_name, working_definition, symbol_type, create_if_missing);
    if (patch_res.isErr()) {
        // The patcher's own code, not a flat 400. A symbol the script does not
        // declare is a 404, which is what a caller branches on to decide
        // between retrying with the right name and creating the symbol (#569).
        return CallToolResult::fromError(patch_res.error());
    }

    const bool created = patch_res.value().created;
    std::string patched_content = patch_res.value().source_text;
    // A file that ended without a newline keeps ending without one. The
    // patcher terminates what it splices in, so the one it adds at the end of
    // the file is the only byte here that was not asked for.
    if (!trailing_newline && !patched_content.empty() && patched_content.back() == '\n') {
        patched_content.pop_back();
    }
    if (crlf) patched_content = strings::replaceAll(patched_content, "\n", "\r\n");
    auto written = files::writeFileAtomically(disk_path, patched_content);
    // Released before the syntax check below, which can start the engine and
    // needs nothing but the file to be there.
    lock.value().reset();
    if (written.isErr()) {
        return CallToolResult::fromError(
            written.error(), "Cannot write patched file to disk: " + reported_path + ": ");
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
    auto diags = offline::GDScriptDiagnostics::analyze(reported_path);
    json diag_arr = json::array();
    bool has_error = false;
    for (const auto& d : diags) {
        if (d.severity == "error") has_error = true;
        diag_arr.push_back(d.toJson());
    }

    json result = {
        {"status", "success"},
        {"file_path", reported_path},
        {"method_name", symbol_name},
        // Which of the two things happened. One response shape stood in for
        // both, so a caller who asked to replace a method and got a new one
        // appended had nothing in the answer that said so (#569).
        {"created", created},
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
