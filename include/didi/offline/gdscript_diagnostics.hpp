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
    // Which engine the check spawned, and where it came from. Both empty when
    // no engine ran: a source_text-only check, a path Godot never saw, or a
    // machine with no Godot at all. `script_check_syntax` had no field for this
    // and not even a raw_output to hide it in, so an answer from 4.7 about a 4.5
    // project was indistinguishable from one the project's own engine gave
    // (#617).
    struct EngineCheck {
        std::string version;
        std::string executable;
        // Whether the compiler pass actually produced a verdict.
        //
        // A launch that fails, and a real file that is not Godot, both leave
        // no output and no banner, and the check returned no diagnostics --
        // which the tool reported as has_errors: false on a script nobody
        // compiled (#677). The engine always prints its banner, so a run with
        // no version is a run that did not happen.
        bool ran{false};
        // Why it did not run, when it did not. Empty otherwise.
        std::string failure;
        std::optional<int> exit_code;
        double duration_seconds{0.0};
    };

    static std::vector<ScriptDiagnostic> analyze(const std::string& file_path,
                                                 const std::string& source_text = "",
                                                 EngineCheck* engine = nullptr);

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

    // The checks patchSymbol makes on its arguments alone, before any file is
    // opened. Exposed so a dry run runs them: the preview described a
    // replacement as planned for a call the write then refused 400, which is
    // the same defect as the node-path one (#571).
    static std::optional<Error> validatePatchArguments(const std::string& symbol_name,
                                                       const std::string& new_definition,
                                                       const std::string& symbol_type);

    static Result<SymbolPatch> patchSymbol(const std::string& source_text,
                                           const std::string& symbol_name,
                                           const std::string& new_definition,
                                           const std::string& symbol_type = "function",
                                           bool create_if_missing = false);

    static std::vector<ScriptDiagnostic> runGodotCompilerCheck(const std::string& script_file_path,
                                                               EngineCheck* engine = nullptr);

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
    // `max_symbols` bounds how many declarations come back, counted across all
    // six kinds, because the response is one thing rather than six. The result
    // always carries symbol_count_total, returned_count and truncated, so a
    // caller can tell a complete answer from a clipped one -- which a 15 MB
    // reply with no limit field anywhere could not (#575).
    static constexpr size_t kDefaultMaxSymbols = 2000;
    static json extractSymbols(const std::string& source_text,
                               size_t max_symbols = kDefaultMaxSymbols);
    static std::optional<GDScriptDeclaration> parseDeclaration(std::string_view code_line);
};

} // namespace offline
} // namespace didi
