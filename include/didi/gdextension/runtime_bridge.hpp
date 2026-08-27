#pragma once

#include "didi/common/types.hpp"
#include <string>

namespace didi {
namespace godot {

json executeRuntimeBridge(const std::string& method, const json& params,
                          const std::string& session_kind);

} // namespace godot
} // namespace didi
