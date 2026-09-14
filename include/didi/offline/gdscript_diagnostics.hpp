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

// What a patch did, rather than only what the file now holds. Replacing a
// symbol and creating one that was never there are different outcomes, and the
// caller has to be able to tell them apart (#569).
struct SymbolPatch {
    std::string source_text;
    bool created{false};
};

class GDScriptDiagnostics {
public:
    static std::vector<ScriptDiagnostic> analyze(const std::string& file_path, const std::string& source_text = "");

    // The kinds of symbol a patch can name, in the order the schema publishes
    // them. Anything outside this set is refused rather than passed through to
    // a looser match, because an unrecognised kind used to switch off the guard
    // that checks the replacement declares what it replaces (#570).
    static const std::vector<std::string>& symbolTypes();
    static bool isKnownSymbolType(const std::string& symbol_type);

    // Whether the script declares this symbol, by the rules patchSymbol matches
    // with. The preview asks this so a dry run of a patch answers the way the
    // real call will.
    static bool declaresSymbol(const std::string& source_text, const std::string& symbol_name,
                               const std::string& symbol_type);

    static Result<SymbolPatch> patchSymbol(const std::string& source_text,
                                           const std::string& symbol_name,
                                           const std::string& new_definition,
                                           const std::string& symbol_type = "function",
                                           bool create_if_missing = false);

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
