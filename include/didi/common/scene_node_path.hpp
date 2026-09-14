#pragma once

#include "didi/common/types.hpp"

#include <optional>
#include <string>

namespace didi::paths {

// The edited scene has one root and nothing above it, so a path that walks up
// out of it names nothing this can act on. The bridge refuses one before it
// resolves anything, and that refusal is a property of the argument rather than
// of the tree: it needs no engine, no open scene and no node to apply.
//
// It lives here because two binaries have to agree about it. The rule ran in
// the bridge alone, so a dry run composed a preview -- `planned_mutation`, a
// real `before` read off the live tree -- for a call the server already knew it
// would reject, and the identical call without `dry_run` came back 400 (#571).
// One rule, checked wherever the question is asked.
//
// Segments are compared whole: a node legitimately named `..foo` or `x..y` is
// not a parent reference and is left alone.
inline std::optional<Error> refuseParentRelativeNodePath(const std::string& path) {
    // The same three the resolver answers before it looks at segments at all.
    if (path.empty() || path == "/root" || path == ".") return std::nullopt;
    size_t segment_start = 0;
    while (segment_start <= path.size()) {
        const size_t segment_end = path.find('/', segment_start);
        const std::string segment = path.substr(
            segment_start,
            segment_end == std::string::npos ? std::string::npos : segment_end - segment_start);
        if (segment == "..") {
            return Error::invalidArgument(
                "Parent-relative '..' paths are not allowed in the edited scene");
        }
        if (segment_end == std::string::npos) break;
        segment_start = segment_end + 1;
    }
    return std::nullopt;
}

} // namespace didi::paths
