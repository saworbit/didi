#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include "didi/common/types.hpp"
#include "didi/common/json.hpp"

namespace didi {
namespace offline {

struct ScriptDiagnostic {
    int line{1};
    int column{1};
    std::string severity; // "error", "warning", "info"
    std::string message;
    std::string rule;
    // Why this diagnostic is not what it appears to be. Present only when
    // something demoted it, so an unannotated diagnostic reads exactly as it
    // did before.
    std::string note;

    json toJson() const {
        json out = {
            {"line", line},
            {"column", column},
            {"severity", severity},
            {"message", message},
            {"rule", rule}
        };
        if (!note.empty()) out["note"] = note;
        return out;
    }
};

struct GDScriptDeclaration {
    std::string name;
    std::string kind;
    std::string source;
    bool exported{false};
    // Byte offset of `name` within `source`, so a caller can read the rest of
    // the declaration without matching the name a second time.
    size_t name_offset{0};
};

class GDScriptDiagnostics {
public:
    static std::vector<ScriptDiagnostic> analyze(const std::string& file_path, const std::string& source_text = "");

    static Result<std::string> patchSymbol(const std::string& source_text,
                                           const std::string& symbol_name,
                                           const std::string& new_definition,
                                           const std::string& symbol_type = "function");

    static std::vector<ScriptDiagnostic> runGodotCompilerCheck(const std::string& script_file_path);

    // The autoload singleton names project.godot registers, read from the
    // project root this process is running in.
    static std::vector<std::string> projectAutoloadNames();

    // Demotes the diagnostics the Godot compiler check raises only because it
    // runs in a process with no SceneTree, so `has_errors` is a verdict about
    // the script rather than about the checker. Exposed so it can be exercised
    // without a Godot binary.
    static void demoteAutoloadDiagnostics(std::vector<ScriptDiagnostic>& diagnostics,
                                          const std::vector<std::string>& autoload_names);

    static json reflectClass(const std::string& class_name);
    static json extractSymbols(const std::string& source_text);
    static std::optional<GDScriptDeclaration> parseDeclaration(std::string_view code_line);
};

} // namespace offline
} // namespace didi
