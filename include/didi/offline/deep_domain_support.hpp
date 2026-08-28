#pragma once

#include "didi/common/types.hpp"
#include <string>
#include <vector>

namespace didi::offline {

struct DomainDiagnostic {
    std::string severity;
    std::string code;
    std::string message;
    std::string path;
    int line{0};
    int column{0};

    json toJson() const;
};

std::vector<DomainDiagnostic> parseMsBuildDiagnostics(const std::string& output);
std::vector<DomainDiagnostic> parseGodotDiagnostics(const std::string& output);
std::vector<json> parseExportPresets(const std::string& contents);

} // namespace didi::offline
