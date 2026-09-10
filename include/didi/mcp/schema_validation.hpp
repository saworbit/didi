#pragma once

#include "didi/common/types.hpp"

#include <optional>
#include <string>

namespace didi {
namespace mcp {

// Checks call arguments against the JSON Schema a tool publishes.
//
// The server tells clients what a tool accepts and then has to mean it. Every
// handler used to re-derive its own checks by hand, so coverage was uneven: one
// tool answered a missing argument with a clear sentence, another with a
// transport error, and scene_add_to_group quietly mutated the edited scene root
// (#396, #397). This is the one place that reads the published schema.
//
// It covers the keywords the published schemas actually use: type, required,
// properties, additionalProperties, enum, minLength, maxLength, minimum,
// maximum, minItems, maxItems and items. Anything else is passed over rather
// than guessed at, $ref included, and handler-side checks stay as the second
// line of defence. This is a contract check, not a JSON Schema engine: a value
// it passes over is one a handler still has to validate.
//
// Returns a sentence naming the offending property, or nothing when the
// arguments satisfy the schema.
[[nodiscard]] std::optional<std::string> validateAgainstSchema(const json& schema,
                                                               const json& arguments);

} // namespace mcp
} // namespace didi
