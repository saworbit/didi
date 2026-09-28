#pragma once

#include "didi/common/types.hpp"
#include "didi/mcp/mcp_protocol.hpp"

#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace didi::mcp {

// The same call failing the same way again (Q6 in docs/BUILD_QUEUE.md).
//
// Trial 06 showed an agent reads the answer in front of it, and a refusal that
// names its fix is only half of that: an agent that sends the same call again
// has not read it. The second identical failure in a row says so, in
// `error.data.repeated`, and points at the fix the refusal already carries.
//
// "The same call" is the tool and the arguments as sent, less the confirmation
// token, which is new on every attempt and would make every retry look new.
// "The same way" is the refusal's `data.code`. A success of the same call, or
// a different failure, starts the count again. Nothing is ever refused for
// being repeated: polling is legitimate, and a state that changes between two
// calls is exactly what a caller polls for.
//
// One tracker per conversation. A 2024-11-05 client is the whole process; a
// modern request is counted within the runtime session it named, so two tasks
// on one process never see each other's repeats.
class RepeatedFailures {
public:
    // Records one answer to one call, and marks it when it repeats the last
    // failure of that call.
    void observe(const std::string& conversation, const std::string& tool, const json& arguments,
                 CallToolResult& result);

    // Bounds what one conversation can make this remember.
    static constexpr std::size_t kMaxRemembered = 256;

private:
    struct Failure {
        std::string code;
        int count{0};
    };
    std::mutex m_mutex;
    std::unordered_map<std::string, Failure> m_failures;
    std::deque<std::string> m_order;
};

// The refusal's error envelope in a result, if it has one: an `error` object
// with a numeric `code` in its first text item, the same test the error floor
// uses. A live refusal can arrive without isError set, so the flag is not it.
[[nodiscard]] std::optional<json> errorEnvelopeOf(const CallToolResult& result);

}  // namespace didi::mcp
