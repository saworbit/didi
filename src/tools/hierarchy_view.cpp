#include "didi/tools/hierarchy_view.hpp"

#include <algorithm>
#include <functional>
#include <map>

namespace didi::mcp {
namespace {

const json& childrenOf(const json& node) {
    static const json kEmpty = json::array();
    const auto found = node.find("children");
    return found != node.end() && found->is_array() ? *found : kEmpty;
}

std::string typeOf(const json& node) {
    const auto found = node.find("type");
    return found != node.end() && found->is_string() ? found->get<std::string>() : std::string{};
}

// Counts every node in a subtree by type, so an omitted branch can still say
// what was in it rather than only how many.
void countByType(const json& node, std::map<std::string, size_t>& counts, size_t& total) {
    ++total;
    ++counts[typeOf(node)];
    for (const auto& child : childrenOf(node)) countByType(child, counts, total);
}

json countsToJson(const std::map<std::string, size_t>& counts) {
    json result = json::object();
    for (const auto& [type, count] : counts) result[type] = count;
    return result;
}

// True when this node or anything beneath it matches the filter.
bool subtreeMatches(const json& node, const std::set<std::string>& filter) {
    if (filter.count(typeOf(node)) != 0) return true;
    for (const auto& child : childrenOf(node)) {
        if (subtreeMatches(child, filter)) return true;
    }
    return false;
}

json summarizeBranch(const json& node) {
    std::map<std::string, size_t> counts;
    size_t total = 0;
    countByType(node, counts, total);
    json summary = {
        {"name", node.value("name", "")},
        {"type", typeOf(node)},
        {"path", node.value("path", "")},
        {"node_count", total},
        {"counts_by_type", countsToJson(counts)}
    };
    return summary;
}

} // namespace

json shapeHierarchy(const json& tree, const HierarchyViewOptions& options,
                    HierarchyViewStats& stats) {
    stats = HierarchyViewStats{};
    if (!tree.is_object()) return tree;

    if (options.summary) {
        std::map<std::string, size_t> counts;
        size_t total = 0;
        countByType(tree, counts, total);
        stats.node_count = total;

        json branches = json::array();
        for (const auto& child : childrenOf(tree)) branches.push_back(summarizeBranch(child));
        return json{
            {"name", tree.value("name", "")},
            {"type", typeOf(tree)},
            {"path", tree.value("path", "")},
            {"node_count", total},
            {"counts_by_type", countsToJson(counts)},
            {"branches", std::move(branches)}
        };
    }

    const bool filtering = !options.class_filter.empty();
    const bool budgeted = options.max_nodes > 0;

    // Depth first, so a budget keeps a coherent path from the root rather than
    // an arbitrary slice. A node that spends the last of the budget still
    // reports what its children were.
    std::function<json(const json&)> shape = [&](const json& node) -> json {
        json result = node;
        if (filtering && options.class_filter.count(typeOf(node)) != 0) {
            result["matched"] = true;
            ++stats.matched_nodes;
        }
        ++stats.node_count;

        json kept = json::array();
        std::map<std::string, size_t> omitted_counts;
        size_t omitted_total = 0;

        for (const auto& child : childrenOf(node)) {
            if (filtering && !subtreeMatches(child, options.class_filter)) continue;
            if (budgeted && stats.node_count >= options.max_nodes) {
                countByType(child, omitted_counts, omitted_total);
                continue;
            }
            kept.push_back(shape(child));
        }

        result["children"] = std::move(kept);
        if (omitted_total > 0) {
            stats.truncated = true;
            result["children_omitted"] = omitted_total;
            result["children_summary"] = countsToJson(omitted_counts);
        }
        return result;
    };

    return shape(tree);
}

Result<HierarchyViewOptions> parseHierarchyViewOptions(const json& arguments) {
    HierarchyViewOptions options;
    if (!arguments.is_object()) return options;

    if (arguments.contains("max_nodes")) {
        const auto& value = arguments["max_nodes"];
        if (!value.is_number_integer() || value.get<int64_t>() < 1 ||
            value.get<int64_t>() > 100000) {
            return Error::invalidArgument("max_nodes must be an integer from 1 to 100000");
        }
        options.max_nodes = static_cast<size_t>(value.get<int64_t>());
    }

    if (arguments.contains("class_filter")) {
        const auto& value = arguments["class_filter"];
        if (!value.is_array() || value.empty() || value.size() > 64) {
            return Error::invalidArgument("class_filter must be an array of 1 to 64 type names");
        }
        for (const auto& entry : value) {
            if (!entry.is_string() || entry.get<std::string>().empty()) {
                return Error::invalidArgument("class_filter entries must be non-empty strings");
            }
            options.class_filter.insert(entry.get<std::string>());
        }
    }

    if (arguments.contains("summary")) {
        if (!arguments["summary"].is_boolean()) {
            return Error::invalidArgument("summary must be a boolean");
        }
        options.summary = arguments["summary"].get<bool>();
    }

    if (options.summary && (options.max_nodes > 0 || !options.class_filter.empty())) {
        return Error::invalidArgument(
            "summary returns counts for the whole tree, so it cannot be combined with "
            "max_nodes or class_filter");
    }
    return options;
}

} // namespace didi::mcp
