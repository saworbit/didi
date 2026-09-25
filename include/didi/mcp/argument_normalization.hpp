#pragma once

#include "didi/common/types.hpp"

namespace didi::mcp {
inline constexpr char kArgumentNormalizationKey[] = "didi/argumentNormalization";
inline constexpr char kArgumentNormalizationProfile[] = "safe-v1";

[[nodiscard]] bool argumentNormalizationEnabled();
[[nodiscard]] bool supportsArgumentNormalization(std::string_view tool);

// Pure JSON perimeter for explicitly selected read-only tools. Enforces a
// cumulative budget before copying, preserves input, and strictly validates
// the entire result before returning it. Never resolves or invokes objects.
[[nodiscard]] Result<json> normalizeToolArguments(std::string_view tool,
                                                 const json& schema,
                                                 const json& arguments);
} // namespace didi::mcp
