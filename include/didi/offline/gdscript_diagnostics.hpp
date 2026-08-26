#pragma once

#include <string>
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

    json toJson() const {
        return {
            {"line", line},
            {"column", column},
            {"severity", severity},
            {"message", message},
            {"rule", rule}
        };
    }
};

class GDScriptDiagnostics {
public:
    static std::vector<ScriptDiagnostic> analyze(const std::string& file_path, const std::string& source_text = "");

    static Result<std::string> patchSymbol(const std::string& source_text,
                                           const std::string& symbol_name,
                                           const std::string& new_definition,
                                           const std::string& symbol_type = "function");

    static std::vector<ScriptDiagnostic> runGodotCompilerCheck(const std::string& script_file_path);
};

} // namespace offline
} // namespace didi
