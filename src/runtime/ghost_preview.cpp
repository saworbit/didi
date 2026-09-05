#include "didi/runtime/ghost_preview.hpp"

#include <algorithm>
#include <cmath>
#include <initializer_list>

namespace didi {
namespace runtime {

namespace {

bool onlyKeys(const json& object, std::initializer_list<const char*> allowed) {
    for (auto it = object.begin(); it != object.end(); ++it) {
        bool found = false;
        for (const auto* key : allowed) {
            if (it.key() == key) { found = true; break; }
        }
        if (!found) return false;
    }
    return true;
}

Result<double> component(const json& vector, const char* axis, const std::string& field) {
    if (!vector.contains(axis)) {
        return Error::invalidArgument(field + "." + axis + " is required");
    }
    if (!vector[axis].is_number()) {
        return Error::invalidArgument(field + "." + axis + " must be a number");
    }
    const double number = vector[axis].get<double>();
    if (!std::isfinite(number) || number < -kSpatialCoordinateLimit ||
        number > kSpatialCoordinateLimit) {
        return Error::invalidArgument(field + "." + axis +
                                      " must be finite and within -1000000..1000000");
    }
    return number;
}

Result<SpatialPoint> parseVector(const json& owner, const char* key, const std::string& field,
                                 bool positive_only) {
    if (!owner.contains(key)) return Error::invalidArgument(field + " is required");
    const auto& vector = owner[key];
    if (!vector.is_object()) return Error::invalidArgument(field + " must be a vector object");
    SpatialPoint point;
    if (vector.contains("z")) {
        if (!onlyKeys(vector, {"x", "y", "z"})) {
            return Error::invalidArgument(field + " contains an unknown property");
        }
        point.dimension = 3;
    } else {
        if (!onlyKeys(vector, {"x", "y"})) {
            return Error::invalidArgument(field + " contains an unknown property");
        }
        point.dimension = 2;
    }
    auto x = component(vector, "x", field);
    if (x.isErr()) return x.error();
    auto y = component(vector, "y", field);
    if (y.isErr()) return y.error();
    point.x = x.value();
    point.y = y.value();
    if (point.dimension == 3) {
        auto z = component(vector, "z", field);
        if (z.isErr()) return z.error();
        point.z = z.value();
    }
    if (positive_only) {
        // A shape with no extent on an axis draws nothing on that axis, which
        // is a preview a caller would look at and read as a mistake in the
        // scene rather than in the request.
        const bool flat = point.x <= 0.0 || point.y <= 0.0 ||
                          (point.dimension == 3 && point.z <= 0.0);
        if (flat) {
            return Error::invalidArgument(field + " must be greater than 0 on every axis");
        }
    }
    return point;
}

Result<GhostColor> parseColor(const json& owner, const std::string& field) {
    const auto& value = owner["color"];
    if (!value.is_object() || !onlyKeys(value, {"r", "g", "b"})) {
        return Error::invalidArgument(field + " must be an object with r, g and b");
    }
    GhostColor color;
    double* targets[3] = {&color.red, &color.green, &color.blue};
    const char* axes[3] = {"r", "g", "b"};
    for (int index = 0; index < 3; ++index) {
        if (!value.contains(axes[index]) || !value[axes[index]].is_number()) {
            return Error::invalidArgument(field + "." + axes[index] + " must be a number");
        }
        const double channel = value[axes[index]].get<double>();
        if (!std::isfinite(channel) || channel < 0.0 || channel > 1.0) {
            return Error::invalidArgument(field + "." + axes[index] + " must be from 0 to 1");
        }
        *targets[index] = channel;
    }
    return color;
}

} // namespace

json GhostColor::toJson() const {
    return json{{"r", red}, {"g", green}, {"b", blue}};
}

GhostColor colorForGhostKind(GhostKind kind) {
    switch (kind) {
        case GhostKind::translation: return GhostColor{1.0, 1.0, 0.0};
        case GhostKind::deletion: return GhostColor{1.0, 0.0, 0.0};
        case GhostKind::addition: break;
    }
    return GhostColor{0.0, 1.0, 1.0};
}

const char* nameForGhostKind(GhostKind kind) {
    switch (kind) {
        case GhostKind::translation: return "translation";
        case GhostKind::deletion: return "deletion";
        case GhostKind::addition: break;
    }
    return "addition";
}

Result<GhostPreviewRequest> parseGhostPreviewRequest(const json& params) {
    if (!params.is_object()) return Error::invalidArgument("Ghost preview params must be an object");
    if (!onlyKeys(params, {"previews", "replace"})) {
        return Error::invalidArgument("Ghost preview request contains an unknown property");
    }
    if (!params.contains("previews") || !params["previews"].is_array()) {
        return Error::invalidArgument("previews must be an array");
    }
    const auto& entries = params["previews"];
    if (entries.empty() || entries.size() > kMaxGhostShapesPerCall) {
        return Error::invalidArgument("previews must contain 1 to " +
                                      std::to_string(kMaxGhostShapesPerCall) + " entries");
    }

    GhostPreviewRequest request;
    if (params.contains("replace")) {
        if (!params["replace"].is_boolean()) {
            return Error::invalidArgument("replace must be a boolean");
        }
        request.replace = params["replace"].get<bool>();
    }

    request.shapes.reserve(entries.size());
    for (size_t index = 0; index < entries.size(); ++index) {
        const std::string where = "previews[" + std::to_string(index) + "]";
        const auto& entry = entries[index];
        if (!entry.is_object()) return Error::invalidArgument(where + " must be an object");
        if (!onlyKeys(entry, {"position", "size", "rotation_degrees", "kind", "color", "label"})) {
            return Error::invalidArgument(where + " contains an unknown property");
        }
        GhostShape shape;
        auto position = parseVector(entry, "position", where + ".position", false);
        if (position.isErr()) return position.error();
        auto size = parseVector(entry, "size", where + ".size", true);
        if (size.isErr()) return size.error();
        shape.position = position.value();
        shape.size = size.value();
        if (shape.position.dimension != shape.size.dimension) {
            return Error::invalidArgument(where + " mixes a 2D and a 3D vector");
        }

        if (entry.contains("rotation_degrees")) {
            if (shape.position.dimension != 3) {
                return Error::invalidArgument(where +
                    ".rotation_degrees applies to a 3D box; a 2D preview is an axis-aligned rectangle");
            }
            auto rotation = parseVector(entry, "rotation_degrees", where + ".rotation_degrees", false);
            if (rotation.isErr()) return rotation.error();
            if (rotation.value().dimension != 3) {
                return Error::invalidArgument(where + ".rotation_degrees must be a 3D vector");
            }
            shape.rotation_degrees[0] = rotation.value().x;
            shape.rotation_degrees[1] = rotation.value().y;
            shape.rotation_degrees[2] = rotation.value().z;
            shape.rotated = true;
        }

        if (entry.contains("kind")) {
            if (!entry["kind"].is_string()) {
                return Error::invalidArgument(where + ".kind must be a string");
            }
            const auto kind = entry["kind"].get<std::string>();
            if (kind == "addition") {
                shape.kind = GhostKind::addition;
            } else if (kind == "translation") {
                shape.kind = GhostKind::translation;
            } else if (kind == "deletion") {
                shape.kind = GhostKind::deletion;
            } else {
                return Error::invalidArgument(where +
                    ".kind must be addition, translation, or deletion");
            }
        }
        shape.color = colorForGhostKind(shape.kind);
        if (entry.contains("color")) {
            auto color = parseColor(entry, where + ".color");
            if (color.isErr()) return color.error();
            shape.color = color.value();
            shape.color_given = true;
        }

        if (entry.contains("label")) {
            if (!entry["label"].is_string()) {
                return Error::invalidArgument(where + ".label must be a string");
            }
            shape.label = entry["label"].get<std::string>();
            if (shape.label.size() > 256) {
                return Error::invalidArgument(where + ".label must be at most 256 characters");
            }
        }

        if (!request.shapes.empty() &&
            shape.dimension() != request.shapes.front().dimension()) {
            return Error::invalidArgument(
                where + " is " + std::to_string(shape.dimension()) + "D and the call is " +
                std::to_string(request.shapes.front().dimension()) +
                "D; one call draws into one world");
        }
        request.shapes.push_back(std::move(shape));
    }
    return request;
}

Result<GhostClearRequest> parseGhostClearRequest(const json& params) {
    if (!params.is_object()) return Error::invalidArgument("Ghost clear params must be an object");
    if (!onlyKeys(params, {"preview_id"})) {
        return Error::invalidArgument("Ghost clear request contains an unknown property");
    }
    GhostClearRequest request;
    if (params.contains("preview_id")) {
        if (!params["preview_id"].is_string()) {
            return Error::invalidArgument("preview_id must be a string");
        }
        request.preview_id = params["preview_id"].get<std::string>();
        if (request.preview_id.empty() || request.preview_id.size() > 64) {
            return Error::invalidArgument("preview_id must be 1 to 64 characters");
        }
    }
    return request;
}

} // namespace runtime
} // namespace didi
