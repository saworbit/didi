#include "didi/mcp/mcp_protocol.hpp"
#include "didi/tools/phase7_live_forward.hpp"
#include "didi/common/ipc_channel.hpp"
#include "didi/common/json_bounds.hpp"
#include "didi/common/logger.hpp"
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <map>

namespace didi {
namespace mcp {

namespace {

CallToolResult requestError(const ResolvedToolBinding& binding,
                            std::string_view message, int code = 400) {
    return CallToolResult::error(json{{"error", {
        {"code", code}, {"message", message},
        {"data", {{"tool", binding.invoked_name},
                  {"canonical_tool", binding.canonical_name},
                  {"retryable", false}}}}}}.dump());
}

bool hasOnlyKeys(const json& value, std::initializer_list<std::string_view> allowed) {
    if (!value.is_object()) return false;
    for (auto it = value.begin(); it != value.end(); ++it) {
        bool found = false;
        for (const auto key : allowed) if (it.key() == key) { found = true; break; }
        if (!found) return false;
    }
    return true;
}

bool boundedString(const json& value) {
    return value.is_string() && !value.get_ref<const std::string&>().empty() &&
           value.get_ref<const std::string&>().size() <= 1024;
}

bool boundedInteger(const json& value, int64_t minimum, int64_t maximum) {
    return boundedJsonInteger(value, minimum, maximum).has_value();
}

bool integerTuple(const json& value, size_t dimensions, int64_t minimum,
                  int64_t maximum) {
    if (!value.is_array() || value.size() != dimensions) return false;
    for (const auto& component : value) {
        if (!boundedInteger(component, minimum, maximum)) return false;
    }
    return true;
}

// A coordinate written as an object, turned into the array this file and the
// bridge already speak.
//
// Vectors are objects everywhere else on the surface -- scene_set_property,
// scene_instantiate_node, resource_create, physics_raycast_query, nav_query_path
// -- and LLM_INSTRUCTIONS states the object rule flatly with no exception. The
// two cell writers took arrays, so an agent following its own instructions got
// two refusals in a row on the tool whose whole job is painting a level (#738).
// Both forms are taken now; the array stays because it is published and callers
// use it.
std::optional<json> arrayFromVectorObject(const json& value, size_t dimensions) {
    if (!value.is_object()) return std::nullopt;
    static const char* const kAxes[] = {"x", "y", "z"};
    if (dimensions > 3 || value.size() != dimensions) return std::nullopt;
    json array = json::array();
    for (size_t index = 0; index < dimensions; ++index) {
        const auto found = value.find(kAxes[index]);
        if (found == value.end() || !found->is_number_integer()) return std::nullopt;
        array.push_back(*found);
    }
    return array;
}

// Every coordinate in a cell list, rewritten to the array form once, before
// anything validates or forwards it. One place, so the local check, the wire
// and the bridge cannot disagree about what a caller sent.
json withNormalizedCoordinates(const json& args) {
    if (!args.is_object() || !args.contains("cells") || !args["cells"].is_array()) return args;
    json normalized = args;
    for (auto& cell : normalized["cells"]) {
        if (!cell.is_object()) continue;
        // gridmap_set_cells calls it position and tilemap_set_cells calls it
        // coords, for the same thing. Each takes the other's name.
        if (cell.contains("coords") && !cell.contains("position") &&
            normalized.contains("gridmap_path")) {
            cell["position"] = cell["coords"];
            cell.erase("coords");
        } else if (cell.contains("position") && !cell.contains("coords") &&
                   normalized.contains("tilemap_path")) {
            cell["coords"] = cell["position"];
            cell.erase("position");
        }
        for (const auto& field : {std::make_pair("coords", size_t{2}),
                                  std::make_pair("atlas_coords", size_t{2}),
                                  std::make_pair("position", size_t{3})}) {
            const auto found = cell.find(field.first);
            if (found == cell.end()) continue;
            if (auto array = arrayFromVectorObject(*found, field.second)) {
                *found = std::move(*array);
            }
        }
    }
    return normalized;
}

bool validateTileCells(const json& cells) {
    if (!cells.is_array() || cells.empty() || cells.size() > 256) return false;
    std::set<std::pair<int64_t, int64_t>> coordinates;
    for (const auto& cell : cells) {
        if (!cell.is_object() || !cell.contains("coords") ||
            !integerTuple(cell["coords"], 2, -1048576, 1048576)) return false;
        const auto coordinate = std::make_pair(cell["coords"][0].get<int64_t>(),
                                               cell["coords"][1].get<int64_t>());
        if (!coordinates.insert(coordinate).second) return false;
        if (cell.contains("erase")) {
            if (!hasOnlyKeys(cell, {"coords", "erase"}) || !cell["erase"].is_boolean() ||
                !cell["erase"].get<bool>()) return false;
            continue;
        }
        if (!hasOnlyKeys(cell, {"coords", "source_id", "atlas_coords", "alternative_tile"}) ||
            !cell.contains("source_id") || !boundedInteger(cell["source_id"], 0, 2147483647) ||
            !cell.contains("atlas_coords") ||
            !integerTuple(cell["atlas_coords"], 2, 0, 1048576) ||
            (cell.contains("alternative_tile") &&
             !boundedInteger(cell["alternative_tile"], 0, 65535))) return false;
    }
    return true;
}

bool validateGridCells(const json& cells) {
    if (!cells.is_array() || cells.empty() || cells.size() > 256) return false;
    std::set<std::tuple<int64_t, int64_t, int64_t>> positions;
    for (const auto& cell : cells) {
        if (!hasOnlyKeys(cell, {"position", "item", "orientation"}) ||
            !cell.contains("position") ||
            !integerTuple(cell["position"], 3, -1048576, 1048576) ||
            !cell.contains("item") || !boundedInteger(cell["item"], -1, 2147483647) ||
            (cell.contains("orientation") &&
             !boundedInteger(cell["orientation"], 0, 23))) return false;
        const auto position = std::make_tuple(cell["position"][0].get<int64_t>(),
                                              cell["position"][1].get<int64_t>(),
                                              cell["position"][2].get<int64_t>());
        if (!positions.insert(position).second) return false;
        if (cell["item"].get<int64_t>() == -1 && cell.value("orientation", 0) != 0) return false;
    }
    return true;
}

std::string tupleText(const std::vector<int64_t>& tuple) {
    std::string text = "[";
    for (size_t index = 0; index < tuple.size(); ++index) {
        if (index) text += ", ";
        text += std::to_string(tuple[index]);
    }
    return text + "]";
}

// The two rules on these tools a JSON Schema cannot state, and so the two a
// caller is most likely to trip: uniqueness across array entries, and a
// constraint between two fields of one entry. The schema check answers
// everything else with a sentence naming the entry and the value, and these
// answered with a raw identifier that named neither the offending cell, nor the
// rule, nor what to change (#619).
std::optional<std::string> describeDuplicateTuple(const json& cells, const char* key,
                                                  size_t dimensions) {
    if (!cells.is_array()) return std::nullopt;
    std::map<std::vector<int64_t>, size_t> seen;
    for (size_t index = 0; index < cells.size(); ++index) {
        const auto& cell = cells[index];
        if (!cell.is_object() || !cell.contains(key) || !cell[key].is_array() ||
            cell[key].size() != dimensions) return std::nullopt;
        std::vector<int64_t> tuple;
        for (const auto& component : cell[key]) {
            const auto number = jsonInt64(component);
            if (!number.has_value()) return std::nullopt;
            tuple.push_back(*number);
        }
        const auto found = seen.find(tuple);
        if (found != seen.end()) {
            return "Argument 'cells' entries " + std::to_string(found->second) + " and " +
                   std::to_string(index) + " both write " + std::string(key) + " " +
                   tupleText(tuple) + "; each one may appear once.";
        }
        seen.emplace(std::move(tuple), index);
    }
    return std::nullopt;
}

std::optional<std::string> describeErasedCellWithOrientation(const json& cells) {
    if (!cells.is_array()) return std::nullopt;
    for (size_t index = 0; index < cells.size(); ++index) {
        const auto& cell = cells[index];
        if (!cell.is_object() || !cell.contains("item")) continue;
        const auto item = jsonInt64(cell["item"]);
        if (!item.has_value() || *item != -1) continue;
        if (!cell.contains("orientation")) continue;
        const auto orientation = jsonInt64(cell["orientation"]);
        if (!orientation.has_value() || *orientation == 0) continue;
        return "Argument 'cells' entry " + std::to_string(index) + " erases (item -1) and sets "
               "orientation " + std::to_string(*orientation) +
               "; an erased cell has no orientation. Drop orientation, or set an item to place.";
    }
    return std::nullopt;
}

} // namespace

CallToolResult handleTilemapSetCells(const ResolvedToolBinding& binding, const json& args,
                         std::shared_ptr<ipc::IIpcClient> ipc) {
    const json request = withNormalizedCoordinates(args);
    if (request.is_object() && request.contains("cells")) {
        if (auto duplicate = describeDuplicateTuple(request["cells"], "coords", 2)) {
            return requestError(binding, *duplicate, 409);
        }
    }
    if (!hasOnlyKeys(request, {"tilemap_path", "cells"}) ||
        !request.contains("tilemap_path") || !boundedString(request["tilemap_path"]) ||
        !request.contains("cells") || !validateTileCells(request["cells"])) {
        return requestError(binding, "invalid_tilemap_set_cells_request");
    }
    return sendPhase7LiveRequest(binding, request, ipc);
}

CallToolResult handleTilemapGetUsedRect(const ResolvedToolBinding& binding, const json& args,
                         std::shared_ptr<ipc::IIpcClient> ipc) {
    if (!hasOnlyKeys(args, {"tilemap_path"}) || !args.contains("tilemap_path") ||
        !boundedString(args["tilemap_path"])) {
        return requestError(binding, "invalid_tilemap_get_used_rect_request");
    }
    return sendPhase7LiveRequest(binding, args, ipc);
}

CallToolResult handleGridmapSetCells(const ResolvedToolBinding& binding, const json& args,
                         std::shared_ptr<ipc::IIpcClient> ipc) {
    const json request = withNormalizedCoordinates(args);
    if (request.is_object() && request.contains("cells")) {
        if (auto duplicate = describeDuplicateTuple(request["cells"], "position", 3)) {
            return requestError(binding, *duplicate, 409);
        }
        // The other rule the schema cannot state. Asked before the catch-all
        // below, which named none of the several rules it stands for.
        if (auto erased = describeErasedCellWithOrientation(request["cells"])) {
            return requestError(binding, *erased);
        }
    }
    if (!hasOnlyKeys(request, {"gridmap_path", "cells"}) ||
        !request.contains("gridmap_path") || !boundedString(request["gridmap_path"]) ||
        !request.contains("cells") || !validateGridCells(request["cells"])) {
        return requestError(binding, "invalid_gridmap_set_cells_request");
    }
    return sendPhase7LiveRequest(binding, request, ipc);
}

} // namespace mcp
} // namespace didi
