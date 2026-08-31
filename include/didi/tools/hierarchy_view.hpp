#pragma once

#include "didi/common/types.hpp"

#include <cstddef>
#include <set>
#include <string>

namespace didi::mcp {

// Shaping for scene_get_hierarchy responses.
//
// The tool returned the whole recursive tree, and a production level runs to
// tens of thousands of lines of JSON. That is billed as tokens on every query
// and can be larger than the context it is being read into, so the useful
// answer is often a filtered or counted view rather than the whole thing.
//
// This shapes an already-built tree, so it applies identically to the live
// editor response and the offline .tscn parse. It is deliberately not the only
// bound: the live bridge caps its own walk so a large scene cannot exceed the
// IPC frame before this layer ever sees it.
struct HierarchyViewOptions {
    // 0 means unbounded, which stays the default so existing callers see no
    // change until they ask for a budget.
    size_t max_nodes{0};
    // Empty means every type. A node is kept when it matches, or when one of
    // its descendants does, so the path to a match is never broken.
    std::set<std::string> class_filter;
    // Counts by type and one level of branch structure, with no properties and
    // no per-node recursion.
    bool summary{false};
};

struct HierarchyViewStats {
    size_t node_count{0};
    size_t matched_nodes{0};
    bool truncated{false};
};

// Returns the shaped tree and fills stats. An unset options object returns the
// tree unchanged apart from the node count.
json shapeHierarchy(const json& tree, const HierarchyViewOptions& options,
                    HierarchyViewStats& stats);

// Reads the three options off a tool argument object, rejecting malformed ones
// rather than silently ignoring them.
Result<HierarchyViewOptions> parseHierarchyViewOptions(const json& arguments);

} // namespace didi::mcp
