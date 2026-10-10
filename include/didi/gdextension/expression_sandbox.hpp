#pragma once

#include "didi/common/types.hpp"

#include <string>

namespace didi {
namespace godot {

// Runs an expression the policy in didi/runtime/expression_policy.hpp accepts,
// through Godot's Expression class, in the editor or a game.
json executeExpression(const json& params, const std::string& session_kind);

} // namespace godot
} // namespace didi
