#pragma once

#include <string>
#include "didi/common/types.hpp"

namespace didi {
namespace mcp {

// Fills in a `description` for every parameter of a published input schema that
// does not already carry one.
//
// 217 of 381 parameters had none, and 52 tools documented none of theirs. Every
// tool had a top-level description; the parameters inside it mostly did not,
// and the names that cost a caller the most are the ones that are not the
// obvious guess (#462). The prose lives in one table rather than beside each
// schema, because a name that means the same thing in fifteen tools should read
// the same in all fifteen, and because a table is a thing a contract test can
// hold to account.
//
// `schema_source` is the canonical name, so an alias documents its parameters
// identically to the tool it resolves to. A description written inline in the
// schema always wins.
void applyParameterDescriptions(const std::string& schema_source, json& input_schema);

} // namespace mcp
} // namespace didi
