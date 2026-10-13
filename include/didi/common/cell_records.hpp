#pragma once

#include "didi/common/types.hpp"
#include "didi/common/json_bounds.hpp"
#include <array>
#include <map>

namespace didi::cells {

constexpr size_t kMaxRecords = 256;
constexpr size_t kMaxExpandedCells = 4096;
constexpr int64_t kCoordinateLimit = 1048576;

struct PreparedRecords {
    json records;
    json expanded = json::array();
    std::vector<std::pair<size_t, size_t>> ranges;
};

// Both the public handler and raw bridge use this bounded preparation. The
// handler forwards compact canonical records; the bridge applies expanded
// cells through its existing snapshot/UndoRedo/readback transaction.
inline Result<PreparedRecords> prepare(const json& records, size_t dimensions) {
    if (!records.is_array() || records.empty() || records.size() > kMaxRecords ||
        (dimensions != 2 && dimensions != 3)) {
        return Error::invalidArgument("Argument 'cells' must contain 1 to 256 records.");
    }
    PreparedRecords prepared;
    prepared.records = json::array();
    const char* coordinate_key = dimensions == 2 ? "coords" : "position";
    static const char* axes[] = {"x", "y", "z"};
    std::map<std::array<int64_t, 3>, size_t> seen;
    const auto tuple = [&](const json& input, int64_t minimum, int64_t maximum)
        -> std::optional<std::array<int64_t, 3>> {
        std::array<int64_t, 3> out{};
        if ((!input.is_array() && !input.is_object()) || input.size() != dimensions) return std::nullopt;
        for (size_t axis = 0; axis < dimensions; ++axis) {
            if (input.is_object() && !input.contains(axes[axis])) return std::nullopt;
            auto value = boundedJsonInteger(input.is_array() ? input[axis] : input[axes[axis]],
                                             minimum, maximum);
            if (!value) return std::nullopt;
            out[axis] = *value;
        }
        return out;
    };
    for (size_t record_index = 0; record_index < records.size(); ++record_index) {
        const auto& record = records[record_index];
        const auto location = "Argument 'cells' entry " + std::to_string(record_index);
        if (!record.is_object()) return Error::invalidArgument(location + " must be an object.");
        // Raw authenticated IPC also reaches this function. Validate the
        // small fixed record payload before copying it once per covered cell:
        // a malformed string/object must never be amplified by expansion.
        const bool erase = dimensions == 2 && record.contains("erase");
        for (auto it = record.begin(); it != record.end(); ++it) {
            const auto& key = it.key();
            const bool locator = key == "coords" || key == "position" || key == "rect";
            const bool payload = dimensions == 2
                ? (erase ? key == "erase" : key == "source_id" || key == "atlas_coords" || key == "alternative_tile")
                : key == "item" || key == "orientation";
            if (!locator && !payload) {
                return Error::invalidArgument(location + " has an unknown or incompatible field '" + key + "'.");
            }
        }
        std::optional<std::array<int64_t, 3>> atlas;
        if (dimensions == 2) {
            if (erase) {
                if (!record["erase"].is_boolean() || !record["erase"].get<bool>()) {
                    return Error::invalidArgument(location + " erase must be true.");
                }
            } else {
                if (!record.contains("source_id") || !boundedJsonInteger(record["source_id"], 0, 2147483647)) {
                    return Error::invalidArgument(location + " source_id must be an integer from 0 to 2147483647.");
                }
                if (record.contains("atlas_coords")) atlas = tuple(record["atlas_coords"], 0, kCoordinateLimit);
                if (!atlas) return Error::invalidArgument(location + " atlas_coords must be a bounded integer pair.");
                if (record.contains("alternative_tile") && !boundedJsonInteger(record["alternative_tile"], 0, 65535)) {
                    return Error::invalidArgument(location + " alternative_tile must be an integer from 0 to 65535.");
                }
            }
        } else {
            if (!record.contains("item") || !boundedJsonInteger(record["item"], -1, 2147483647)) {
                return Error::invalidArgument(location + " item must be an integer from -1 to 2147483647.");
            }
            const auto orientation = record.contains("orientation")
                ? boundedJsonInteger(record["orientation"], 0, 23) : std::optional<int64_t>(0);
            if (!orientation) return Error::invalidArgument(location + " orientation must be an integer from 0 to 23.");
            if (record["item"] == -1 && *orientation != 0) {
                return Error::invalidArgument(location + " erases (item -1) and sets orientation " +
                    std::to_string(*orientation) + "; an erased cell has no orientation. Drop orientation, or set an item to place.");
            }
        }
        std::array<int64_t, 3> position{}, size{1, 1, 1};
        const bool rectangle = record.contains("rect");
        if (rectangle) {
            if (record.contains("coords") || record.contains("position")) {
                return Error::invalidArgument(location + " must use rect or a cell coordinate, not both.");
            }
            const auto& rect = record["rect"];
            if (!rect.is_object() || rect.size() != 2 || !rect.contains("position") || !rect.contains("size")) {
                return Error::invalidArgument(location + " rect requires only position and size.");
            }
            auto origin = tuple(rect["position"], -kCoordinateLimit, kCoordinateLimit);
            auto extent = tuple(rect["size"], 1, static_cast<int64_t>(kMaxExpandedCells));
            if (!origin || !extent) {
                return Error::invalidArgument(location + " rect.position needs bounded integer coordinates and rect.size needs positive integers.");
            }
            position = *origin;
            size = *extent;
            if (dimensions == 2) size[2] = 1;
        } else {
            const char* alias = dimensions == 2 ? "position" : "coords";
            if (record.contains(coordinate_key) == record.contains(alias)) {
                return Error::invalidArgument(location + " requires exactly one cell coordinate or rect.");
            }
            const char* supplied = record.contains(coordinate_key) ? coordinate_key : alias;
            auto origin = tuple(record[supplied], -kCoordinateLimit, kCoordinateLimit);
            if (!origin) return Error::invalidArgument(location + " has invalid integer coordinates.");
            position = *origin;
        }
        size_t count = 1;
        for (size_t axis = 0; axis < dimensions; ++axis) {
            // Values are already bounded; division checks the product before
            // multiplication, and the endpoint check precedes expansion.
            if (position[axis] > kCoordinateLimit - (size[axis] - 1)) {
                return Error::invalidArgument(location + " rect ends outside coordinate bounds.");
            }
            if (static_cast<size_t>(size[axis]) > kMaxExpandedCells / count) {
                return Error::invalidArgument("Argument 'cells' touches more than 4096 cells; split the rectangles into separate calls.");
            }
            count *= static_cast<size_t>(size[axis]);
        }
        if (count > kMaxExpandedCells - prepared.expanded.size()) {
            return Error::invalidArgument("Argument 'cells' touches more than 4096 cells; split the rectangles into separate calls.");
        }
        json canonical_position = json::array(), canonical_size = json::array();
        for (size_t axis = 0; axis < dimensions; ++axis) {
            canonical_position.push_back(position[axis]);
            canonical_size.push_back(size[axis]);
        }
        json canonical = record;
        canonical.erase(dimensions == 2 ? "position" : "coords");
        if (rectangle) canonical["rect"] = {{"position", canonical_position}, {"size", canonical_size}};
        else canonical[coordinate_key] = canonical_position;
        if (atlas) canonical["atlas_coords"] = json::array({(*atlas)[0], (*atlas)[1]});
        prepared.records.push_back(canonical);
        const size_t begin = prepared.expanded.size();
        for (int64_t z = 0; z < size[2]; ++z) {
            for (int64_t y = 0; y < size[1]; ++y) {
                for (int64_t x = 0; x < size[0]; ++x) {
                    const std::array<int64_t, 3> point{position[0] + x, position[1] + y, position[2] + z};
                    auto [found, inserted] = seen.emplace(point, record_index);
                    if (!inserted) {
                        std::string coordinate = "[";
                        for (size_t axis = 0; axis < dimensions; ++axis) {
                            if (axis) coordinate += ", ";
                            coordinate += std::to_string(point[axis]);
                        }
                        coordinate += "]";
                        return Error(409, "Argument 'cells' entries " + std::to_string(found->second) +
                            " and " + std::to_string(record_index) + " both write " + coordinate_key +
                            " " + coordinate + "; each one may appear once.");
                    }
                    json cell = canonical;
                    cell.erase("rect");
                    cell.erase(dimensions == 2 ? "position" : "coords");
                    json coordinates = json::array();
                    for (size_t axis = 0; axis < dimensions; ++axis) coordinates.push_back(point[axis]);
                    cell[coordinate_key] = std::move(coordinates);
                    prepared.expanded.push_back(std::move(cell));
                }
            }
        }
        prepared.ranges.emplace_back(begin, count);
    }
    return prepared;
}

// State fields come from the bridge's observed cells, after it verified every
// expanded cell. The rectangle only describes which cells that state covers.
inline json compactObserved(const PreparedRecords& prepared, const json& observed,
                             size_t dimensions) {
    static const char* axes[] = {"x", "y", "z"};
    const char* coordinate_key = dimensions == 2 ? "coords" : "position";
    json result = json::array();
    for (size_t index = 0; index < prepared.records.size(); ++index) {
        const auto [begin, count] = prepared.ranges[index];
        json record = observed.at(begin);
        if (prepared.records[index].contains("rect")) {
            json position = json::object(), size = json::object();
            const auto& rect = prepared.records[index]["rect"];
            for (size_t axis = 0; axis < dimensions; ++axis) {
                position[axes[axis]] = rect["position"][axis];
                size[axes[axis]] = rect["size"][axis];
            }
            record.erase(coordinate_key);
            record["rect"] = {{"position", std::move(position)}, {"size", std::move(size)}};
            record["cell_count"] = count;
        }
        result.push_back(std::move(record));
    }
    return result;
}

} // namespace didi::cells
