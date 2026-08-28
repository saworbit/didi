#pragma once

#include "didi/common/types.hpp"
#include <string>
#include <vector>

namespace didi::offline {

inline constexpr char kOfflineHelperEnvironment[] = "DIDI_OFFLINE_HELPER";

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
std::vector<std::string> isolatedGodotArguments(std::vector<std::string> arguments);

} // namespace didi::offline
