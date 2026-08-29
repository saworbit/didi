#pragma once

#include "didi/common/json.hpp"
#include "didi/runtime/session_kind_policy.hpp"

#include <string_view>

namespace didi::mcp {

struct ResolvedToolBinding {
    std::string_view invoked_name;
    std::string_view canonical_name;
    std::string_view schema_source;
    std::string_view capability_source;
    std::string_view policy_source;
    std::string_view handler_id;
    std::string_view ipc_method;
    runtime::SessionKindPolicy session_policy;
};

ResolvedToolBinding resolveAliasBinding(std::string_view invoked_name,
                                        const json& arguments = json::object());

} // namespace didi::mcp
