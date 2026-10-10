#pragma once

// The read-only expression sandbox's rules for an expression's text, apart
// from running it. The server checks an expression with them before it sends
// one to an engine, and the bridge checks it again before Godot parses it, so
// both ends refuse the same text the same way. Nothing here calls Godot.

#include "didi/common/types.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace didi::runtime {

class ExpressionPolicy {
public:
    static Result<void> validate(std::string_view source);
};

// A `node.get("property")` read in an expression: the span of source it
// covers and the property it names. The bridge reads each one natively before
// Godot runs the rest.
struct DirectPropertyRead {
    size_t start;
    size_t end;
    std::string property;
};

Result<std::vector<DirectPropertyRead>> directPropertyReads(std::string_view source);

bool isValidUtf8(std::string_view text);

} // namespace didi::runtime
