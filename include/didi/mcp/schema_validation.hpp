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
// Arguments that are not a JSON object are refused outright when the schema
// describes one, which every published schema does. Saying nothing about them
// reported that a call had satisfied a contract it cannot satisfy.
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

// Stamps minLength: 1 on every required string property that declares no
// minLength of its own.
//
// 41 of 90 required string parameters accepted "", and each handler behind
// one invented its own answer: "Parameter 'script_path' is required" for an
// argument that was supplied, a bare string for class_name, and a test lab
// with no target for target_resource_path (#553, #554). Every required
// string on this surface names something -- a path, a node, a class, a
// setting, a group -- so the empty string is never the value a caller meant.
// The one exception is file contents, which an empty file legitimately has;
// it is named here rather than left to the next author to remember.
//
// Stamped where additionalProperties is stamped, and for the same reason: the
// argument check then answers all of them identically, in the envelope, naming
// the property, and the next tool to be added cannot quietly reintroduce the
// gap. A minLength written inline in a schema is the author's and wins.
void requireNonEmptyRequiredStrings(std::string_view schema_source, json& schema);

// Stamps maxLength on every required string parameter that does not already
// carry one, sized by what the value is rather than by one global number.
//
// The surface split in an odd place: 62 of 64 numeric parameters carried a
// minimum, and 90 of 191 strings carried a maxLength, with 50 required ones
// unbounded. A blackboard key was capped at 512 bytes and a Godot node path was
// not capped at all. A length nobody declared is a question the schema cannot
// answer, and it is also what the server agrees to buffer before any handler
// has looked at the call (#573).
void boundRequiredStrings(std::string_view schema_source, json& schema);

// The declared ceiling for each kind of string, so the stamp and the tests read
// the same numbers.
namespace bounds {
// An identifier: a method, a property, a group, an action, a setting key. Godot
// has no identifier near this length; the figure matches project_search_text's
// query, which is the bound the surface already published for a short value.
constexpr int kIdentifier = 256;
// A path, res:// or node. The 1024 project_search_text.search_path already
// uses.
constexpr int kPath = 1024;
// A body: a whole script file, or one symbol's definition. The one place a
// large number is the right answer, and the point is that it is declared. Far
// above any hand-written .gd, and small enough that a fat-fingered paste is
// refused in the envelope rather than buffered.
constexpr int kBody = 1048576;
}  // namespace bounds

} // namespace mcp
} // namespace didi
