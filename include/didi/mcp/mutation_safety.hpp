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

// Reads the target a preview is about to describe, without changing it.
//
// A dry run exists to answer "will this work, and what will it change". Binding
// the arguments to a token answers neither: a preview of a mutation that cannot
// possibly succeed was shaped exactly like a preview of one that will, so an
// agent using dry_run as its safety check got a clean preview for a typo'd
// target and discovered it mid-batch (#417).
//
// Returning an Error means the real call would fail that way, and the preview
// fails the same way rather than minting a token for it. Returning nothing
// having filled `before` means the target was read and the preview can say what
// is there now. Returning nothing without filling `before` means this tool has
// no probe, and the preview says so instead of claiming to have planned
// something.
using TargetProbe = std::function<std::optional<Error>(const json& arguments, json& before)>;

struct MutationDecision {
    bool execute{true};
    bool is_error{false};
    json arguments{json::object()};
    json payload{json::object()};
};

// True when calling this tool starts a subprocess against the project, so the
// code that ends up running is chosen by the project rather than by Didi.
bool toolRunsProjectControlledCode(const ResolvedToolBinding& binding);

// True when this tool changes state the server holds rather than anything in
// the project. Not a mutation, so there is no preview and no confirmation, but
// not read-only either: the attachment every later live call routes through is
// picked and severed here.
bool toolWritesServerState(const ResolvedToolBinding& binding);

// True when this tool can only add. Used for destructiveHint, which exists to
// separate a writer that may destroy something from one that cannot. A tool
// that takes an overwrite flag is not additive-only.
bool toolIsAdditiveOnly(const ResolvedToolBinding& binding);

// True when making the same call twice lands in the same state. Used for
// idempotentHint, which is what a host reads to decide whether a call that
// timed out can safely be sent again.
bool toolIsIdempotentWriter(const ResolvedToolBinding& binding);

// True when making this exact call a second time cannot change anything the
// first attempt may already have done.
//
// A live call that fails on the wire leaves the caller unable to say whether
// the engine ran it. For a mutation that ambiguity has to be reported, because
// repeating it could apply the change twice. For a call that changes nothing it
// costs nothing to ask again, and asking is how the ambiguity is resolved.
// Arguments matter, not only the tool: the same tool can be asked to accumulate
// rather than replace, and then a second attempt is not the first one over
// again.
bool liveCallIsRepeatable(const ResolvedToolBinding& binding, const json& arguments);

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
                              const MutationContext& context,
                              const TargetProbe& probe = {});

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
    // `data` is what this call site knows beyond the floor. Everything the
    // floor fills -- code, tool, canonical_tool, retryable -- is added after,
    // and never over the top of a key given here.
    MutationDecision errorDecision(const ResolvedToolBinding& binding, int code,
                                   const std::string& message,
                                   const MutationContext& context,
                                   json data = json::object()) const;
    void prune(int64_t now);

    Clock m_clock;
    TokenGenerator m_tokenGenerator;
    std::unordered_map<std::string, Confirmation> m_confirmations;
    std::mutex m_mutex;
};

} // namespace didi::mcp
