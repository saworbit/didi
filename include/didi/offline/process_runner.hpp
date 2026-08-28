#pragma once

#include "didi/common/types.hpp"
#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

namespace didi::offline {

struct ProcessRequest {
    std::string executable;
    std::vector<std::string> arguments;
    std::filesystem::path working_directory;
    std::chrono::milliseconds timeout{30000};
    size_t max_output_bytes{1024 * 1024};
};

struct ProcessResult {
    int exit_code{0};
    bool timed_out{false};
    bool output_truncated{false};
    double duration_seconds{0.0};
    std::string output;
};

Result<ProcessResult> runProcess(const ProcessRequest& request);

#if defined(_WIN32)
namespace detail {
std::wstring quoteWindowsArgument(const std::wstring& argument);
}
#endif

} // namespace didi::offline
