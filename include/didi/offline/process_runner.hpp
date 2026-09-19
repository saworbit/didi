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
    // Whether the child was placed in something that reaches its descendants:
    // a job object on Windows, a process group on POSIX. It is what makes the
    // timeout kill cover the whole tree rather than the one process this call
    // started, which matters because these tools run build systems -- `dotnet
    // build` spawns MSBuild worker nodes -- and Didi is a long lived server, so
    // a leak accumulates over a session rather than dying with the call.
    //
    // It normally succeeds. A host that runs this process inside a job with
    // breakaway restricted can refuse the assignment, and then the timeout
    // reaches the process this call started and nothing under it; the run still
    // happens, and this is how a caller tells the weaker path from the usual
    // one (#758).
    //
    // Not the same claim as TestSessionResult::contained, which is about the
    // job alone and is false on POSIX. This one is true on POSIX when the group
    // exists, because here the group is what the timeout signals.
    bool contained{false};
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
