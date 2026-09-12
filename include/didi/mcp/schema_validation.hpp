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

// Whether the check above will refuse an argument this schema does not declare.
//
// The rule and its publication are the same fact, and they were written twice:
// the server closed arguments by default in #418 and 73 of 126 published
// schemas carried no additionalProperties, so by JSON Schema they accepted
// anything. A client validating locally before sending passed a call the server
// then refused, which is the direction that turns a clean local check into a
// failed round trip (#508). registerTool stamps what this returns, so the
// schema cannot say one thing while the validator does another.
[[nodiscard]] bool topLevelArgumentsAreClosed(const json& schema);

} // namespace mcp
} // namespace didi
