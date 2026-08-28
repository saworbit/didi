#pragma once

#include "didi/common/types.hpp"

#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace didi::mcp {

struct MutationContext {
    std::string project_root;
    std::string execution_mode;
    std::optional<std::string> session_id;
    uint64_t route_generation{0};
};

struct MutationDecision {
    bool execute{true};
    bool is_error{false};
    json arguments{json::object()};
    json payload{json::object()};
};

class MutationSafety {
public:
    using Clock = std::function<int64_t()>;
    using TokenGenerator = std::function<std::string()>;
    static constexpr int64_t kConfirmationTtlMs = 120'000;

    explicit MutationSafety(Clock clock = {}, TokenGenerator token_generator = {});

    MutationDecision evaluate(const std::string& tool_name, const json& arguments,
                              const MutationContext& context);

    static bool isMutation(const std::string& tool_name);
    static bool canRequireConfirmation(const std::string& tool_name);
    static void decorateSchema(const std::string& tool_name, json& schema);

private:
    struct Confirmation {
        std::string tool_name;
        json arguments;
        MutationContext context;
        int64_t expires_at_ms{0};
    };

    static bool requiresConfirmation(const std::string& tool_name, const json& arguments);
    static bool sameContext(const MutationContext& left, const MutationContext& right);
    static json previewArguments(const json& arguments);
    static std::string bindingHash(const std::string& tool_name, const json& arguments,
                                   const MutationContext& context);
    MutationDecision errorDecision(int code, const std::string& message,
                                   const MutationContext& context) const;
    void prune(int64_t now);

    Clock m_clock;
    TokenGenerator m_tokenGenerator;
    std::unordered_map<std::string, Confirmation> m_confirmations;
    std::mutex m_mutex;
};

} // namespace didi::mcp
