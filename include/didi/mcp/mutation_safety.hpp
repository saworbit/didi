#pragma once

#include "didi/common/types.hpp"
#include "didi/tools/resolved_tool_binding.hpp"

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

// True when calling this tool starts a subprocess against the project, so the
// code that ends up running is chosen by the project rather than by Didi.
bool toolRunsProjectControlledCode(const ResolvedToolBinding& binding);

class MutationSafety {
public:
    using Clock = std::function<int64_t()>;
    using TokenGenerator = std::function<std::string()>;
    static constexpr int64_t kConfirmationTtlMs = 120'000;

    explicit MutationSafety(Clock clock = {}, TokenGenerator token_generator = {});

    MutationDecision preview(const ResolvedToolBinding& binding, const json& arguments,
                             const MutationContext& context);
    MutationDecision authorize(const ResolvedToolBinding& binding, const json& arguments,
                               const MutationContext& context);
    MutationDecision evaluate(const ResolvedToolBinding& binding, const json& arguments,
                              const MutationContext& context);

    static bool isMutation(const ResolvedToolBinding& binding);
    static bool canRequireConfirmation(const ResolvedToolBinding& binding);
    // Whether this exact call needs confirmation, as opposed to whether the tool
    // ever can. A pure predicate over the binding and arguments, like the two
    // above; public so the protocol layer can decide whether to ask a person
    // before it reaches the token flow.
    static bool requiresConfirmation(const ResolvedToolBinding& binding,
                                     const json& arguments);
    static void decorateSchema(const ResolvedToolBinding& binding, json& schema);

private:
    struct Confirmation {
        std::string invoked_name;
        json arguments;
        MutationContext context;
        int64_t expires_at_ms{0};
    };

    static bool sameContext(const MutationContext& left, const MutationContext& right);
    static json previewArguments(const json& arguments);
    static std::string bindingHash(const ResolvedToolBinding& binding,
                                   const json& arguments,
                                   const MutationContext& context);
    MutationDecision errorDecision(const ResolvedToolBinding& binding, int code,
                                   const std::string& message,
                                   const MutationContext& context) const;
    void prune(int64_t now);

    Clock m_clock;
    TokenGenerator m_tokenGenerator;
    std::unordered_map<std::string, Confirmation> m_confirmations;
    std::mutex m_mutex;
};

} // namespace didi::mcp
