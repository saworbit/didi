#pragma once

#include <string>
#include <vector>
#include "didi/common/types.hpp"
#include "didi/common/json.hpp"

namespace didi {
namespace offline {

std::string resolveGodotExecutable();

struct TestSessionLog {
    std::string level; // "INFO", "WARN", "ERROR", "SCRIPT_ERROR"
    std::string message;
    std::string timestamp;

    json toJson() const {
        return {
            {"level", level},
            {"message", message},
            {"timestamp", timestamp}
        };
    }
};

struct TestSessionResult {
    bool success{true};
    int exit_code{0};
    double duration_seconds{0.0};
    std::vector<TestSessionLog> logs;
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    std::string summary;

    json toJson() const {
        json log_arr = json::array();
        for (const auto& l : logs) log_arr.push_back(l.toJson());

        return {
            {"success", success},
            {"exit_code", exit_code},
            {"duration_seconds", duration_seconds},
            {"logs", log_arr},
            {"errors", errors},
            {"warnings", warnings},
            {"summary", summary}
        };
    }
};

class TestRunner {
public:
    static TestSessionResult runSession(const std::string& scene_path,
                                        int timeout_seconds = 10,
                                        bool headless = true,
                                        bool break_on_error = true,
                                        const std::vector<std::string>& extra_args = {});
};

} // namespace offline
} // namespace didi
