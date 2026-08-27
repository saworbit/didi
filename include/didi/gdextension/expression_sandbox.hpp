#pragma once

#include "didi/common/types.hpp"

#include <string>
#include <string_view>

namespace didi {
namespace godot {

class ExpressionPolicy {
public:
    static Result<void> validate(std::string_view source);
};

json executeExpression(const json& params, const std::string& session_kind);

} // namespace godot
} // namespace didi
