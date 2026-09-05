#include "didi/runtime/spatial_queries.hpp"

#include <cmath>
#include <string>
#include <initializer_list>
#include <limits>

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

Result<double> coordinate(const json& vector, const char* axis, const char* field) {
    if (!vector.contains(axis)) {
        return Error::invalidArgument(std::string(field) + "." + axis + " is required");
    }
    const auto& value = vector[axis];
    if (!value.is_number()) {
        return Error::invalidArgument(std::string(field) + "." + axis + " must be a number");
    }
    const double number = value.get<double>();
    if (!std::isfinite(number) || number < -kSpatialCoordinateLimit || number > kSpatialCoordinateLimit) {
        return Error::invalidArgument(std::string(field) + "." + axis +
                                      " must be finite and within -1000000..1000000");
    }
    return number;
}

Result<SpatialPoint> parsePoint(const json& params, const char* field) {
    if (!params.contains(field)) return Error::invalidArgument(std::string(field) + " is required");
    const auto& vector = params[field];
    if (!vector.is_object()) return Error::invalidArgument(std::string(field) + " must be a vector object");
    SpatialPoint point;
    if (vector.contains("z")) {
        if (!onlyKeys(vector, {"x", "y", "z"})) {
            return Error::invalidArgument(std::string(field) + " contains an unknown property");
        }
        point.dimension = 3;
    } else {
        if (!onlyKeys(vector, {"x", "y"})) {
            return Error::invalidArgument(std::string(field) + " contains an unknown property");
        }
        point.dimension = 2;
    }
    auto x = coordinate(vector, "x", field);
    if (x.isErr()) return x.error();
    auto y = coordinate(vector, "y", field);
    if (y.isErr()) return y.error();
    point.x = x.value();
    point.y = y.value();
    if (point.dimension == 3) {
        auto z = coordinate(vector, "z", field);
        if (z.isErr()) return z.error();
        point.z = z.value();
    }
    return point;
}

// A bound printed as 0.001 rather than 0.001000, so a message reads like the
// number a caller would have written.
std::string formatBound(double value) {
    std::string text = std::to_string(value);
    const size_t point = text.find('.');
    if (point == std::string::npos) return text;
    size_t last = text.find_last_not_of('0');
    if (last == point) last = point - 1;
    return text.substr(0, last + 1);
}

Result<int64_t> layerField(const json& params, const char* field) {
    if (!params.contains(field)) return int64_t{1};
    const auto& value = params[field];
    if (!value.is_number_integer() && !value.is_number_unsigned()) {
        return Error::invalidArgument(std::string(field) + " must be an integer");
    }
    if (value.is_number_unsigned() &&
        value.get<uint64_t>() > static_cast<uint64_t>(std::numeric_limits<int32_t>::max())) {
        return Error::invalidArgument(std::string(field) + " must be from 1 to 2147483647");
    }
    const auto number = value.get<int64_t>();
    if (number < 1 || number > std::numeric_limits<int32_t>::max()) {
        return Error::invalidArgument(std::string(field) + " must be from 1 to 2147483647");
    }
    return number;
}

} // namespace

json SpatialPoint::toJson() const {
    if (dimension == 2) return json{{"x", x}, {"y", y}};
    return json{{"x", x}, {"y", y}, {"z", z}};
}

Result<RaycastRequest> parseRaycastRequest(const json& params) {
    if (!params.is_object()) return Error::invalidArgument("Raycast params must be an object");
    if (!onlyKeys(params, {"from", "to", "collision_mask"})) {
        return Error::invalidArgument("Raycast request contains an unknown property");
    }
    RaycastRequest request;
    auto from = parsePoint(params, "from");
    if (from.isErr()) return from.error();
    auto to = parsePoint(params, "to");
    if (to.isErr()) return to.error();
    request.from = from.value();
    request.to = to.value();
    if (request.from.dimension != request.to.dimension) {
        return Error::invalidArgument("from and to must share one dimension");
    }
    if (request.from.x == request.to.x && request.from.y == request.to.y &&
        request.from.z == request.to.z) {
        return Error::invalidArgument("from and to must describe a non-zero segment");
    }
    auto mask = layerField(params, "collision_mask");
    if (mask.isErr()) return mask.error();
    request.collision_mask = mask.value();
    return request;
}

Result<RaycastBatchRequest> parseRaycastBatchRequest(const json& params) {
    if (!params.is_object()) return Error::invalidArgument("Raycast batch params must be an object");
    if (!onlyKeys(params, {"rays"})) {
        return Error::invalidArgument("Raycast batch request contains an unknown property");
    }
    if (!params.contains("rays") || !params["rays"].is_array()) {
        return Error::invalidArgument("rays must be an array");
    }
    const auto& rays = params["rays"];
    if (rays.empty() || rays.size() > kMaxRaycastBatch) {
        return Error::invalidArgument("rays must contain 1 to " + std::to_string(kMaxRaycastBatch) +
                                      " entries");
    }
    RaycastBatchRequest request;
    request.rays.reserve(rays.size());
    for (size_t index = 0; index < rays.size(); ++index) {
        // Each entry goes through the single-ray contract unchanged. A batch
        // that accepted anything the single call rejects, or the other way
        // round, would be a second contract nobody wrote down.
        auto ray = parseRaycastRequest(rays[index]);
        if (ray.isErr()) {
            return Error(ray.error().code,
                         "rays[" + std::to_string(index) + "]: " + ray.error().message);
        }
        if (!request.rays.empty() && ray.value().dimension() != request.rays.front().dimension()) {
            return Error::invalidArgument(
                "rays[" + std::to_string(index) +
                "] is " + std::to_string(ray.value().dimension()) +
                "D and the batch is " + std::to_string(request.rays.front().dimension()) +
                "D; one batch is answered by one space state");
        }
        request.rays.push_back(std::move(ray.value()));
    }
    return request;
}

Result<FrustumRequest> parseFrustumRequest(const json& params) {
    if (!params.is_object()) return Error::invalidArgument("Frustum params must be an object");
    if (!onlyKeys(params, {"camera_node", "camera", "collision_mask", "sightline", "max_results"})) {
        return Error::invalidArgument("Frustum request contains an unknown property");
    }
    const bool has_node = params.contains("camera_node");
    const bool has_camera = params.contains("camera");
    if (has_node == has_camera) {
        return Error::invalidArgument(
            "Exactly one of camera_node and camera is required; one query is answered by one frustum");
    }

    FrustumRequest request;
    if (has_node) {
        if (!params["camera_node"].is_string()) {
            return Error::invalidArgument("camera_node must be a string");
        }
        request.source = FrustumSource::camera_node;
        request.camera_node = params["camera_node"].get<std::string>();
        if (request.camera_node.empty() || request.camera_node.size() > 1024) {
            return Error::invalidArgument("camera_node must be 1 to 1024 characters");
        }
    } else {
        if (!params["camera"].is_object()) {
            return Error::invalidArgument("camera must be an object");
        }
        const auto& camera = params["camera"];
        if (!onlyKeys(camera, {"position", "look_at", "up", "fov_degrees", "near", "far", "aspect"})) {
            return Error::invalidArgument("camera contains an unknown property");
        }
        request.source = FrustumSource::parameters;

        const auto solid_point = [&](const char* field) -> Result<SpatialPoint> {
            const std::string label = "camera." + std::string(field);
            // parsePoint names the field it read in its own errors, so the
            // prefix goes on the message rather than on the key it looks up.
            auto point = parsePoint(camera, field);
            if (point.isErr()) return Error(point.error().code, "camera." + point.error().message);
            // parsePoint reads a 2D point when z is absent. A frustum has no 2D
            // form, so the omission is a mistake worth naming rather than a
            // shape to accept quietly.
            if (point.value().dimension != 3) {
                return Error::invalidArgument(label + " must be a 3D point; a frustum has no 2D form");
            }
            return point.value();
        };
        auto position = solid_point("position");
        if (position.isErr()) return position.error();
        auto look_at = solid_point("look_at");
        if (look_at.isErr()) return look_at.error();
        request.position = position.value();
        request.look_at = look_at.value();
        if (camera.contains("up")) {
            auto up = solid_point("up");
            if (up.isErr()) return up.error();
            request.up = up.value();
            request.up_given = true;
        }

        const auto number = [&](const char* field, double low, double high) -> Result<double> {
            const std::string label = "camera." + std::string(field);
            if (!camera.contains(field)) {
                return Error::invalidArgument(label + " is required");
            }
            if (!camera[field].is_number()) {
                return Error::invalidArgument(label + " must be a number");
            }
            const double value = camera[field].get<double>();
            if (!std::isfinite(value) || value < low || value > high) {
                return Error::invalidArgument(label + " must be from " + formatBound(low) + " to " +
                                              formatBound(high));
            }
            return value;
        };
        auto fov = number("fov_degrees", 1.0, 179.0);
        if (fov.isErr()) return fov.error();
        auto near_plane = number("near", 0.001, kSpatialCoordinateLimit);
        if (near_plane.isErr()) return near_plane.error();
        auto far_plane = number("far", 0.001, kSpatialCoordinateLimit);
        if (far_plane.isErr()) return far_plane.error();
        auto aspect = number("aspect", 0.01, 100.0);
        if (aspect.isErr()) return aspect.error();
        request.fov_degrees = fov.value();
        request.near_plane = near_plane.value();
        request.far_plane = far_plane.value();
        request.aspect = aspect.value();
        if (request.near_plane >= request.far_plane) {
            return Error::invalidArgument("camera.near must be less than camera.far");
        }

        const double forward_x = request.look_at.x - request.position.x;
        const double forward_y = request.look_at.y - request.position.y;
        const double forward_z = request.look_at.z - request.position.z;
        const double forward_length =
            std::sqrt(forward_x * forward_x + forward_y * forward_y + forward_z * forward_z);
        if (forward_length < 1e-6) {
            return Error::invalidArgument("camera.look_at must differ from camera.position");
        }
        const double up_length = std::sqrt(request.up.x * request.up.x + request.up.y * request.up.y +
                                           request.up.z * request.up.z);
        if (up_length < 1e-6) {
            return Error::invalidArgument("camera.up must have a length");
        }
        // Parallel up and forward leave the roll undefined, and the cross
        // product would hand back a zero vector that every later test would
        // read as a degenerate frustum rather than as the bad input it is.
        const double cross_x = forward_y * request.up.z - forward_z * request.up.y;
        const double cross_y = forward_z * request.up.x - forward_x * request.up.z;
        const double cross_z = forward_x * request.up.y - forward_y * request.up.x;
        const double cross_length =
            std::sqrt(cross_x * cross_x + cross_y * cross_y + cross_z * cross_z);
        if (cross_length < 1e-6 * forward_length * up_length) {
            return Error::invalidArgument("camera.up is parallel to the view direction, which leaves "
                                          "the roll of the frustum undefined");
        }
    }

    auto mask = layerField(params, "collision_mask");
    if (mask.isErr()) return mask.error();
    request.collision_mask = mask.value();

    if (params.contains("sightline")) {
        if (!params["sightline"].is_boolean()) {
            return Error::invalidArgument("sightline must be a boolean");
        }
        request.sightline = params["sightline"].get<bool>();
    }
    if (params.contains("max_results")) {
        const auto& value = params["max_results"];
        if (!value.is_number_integer() && !value.is_number_unsigned()) {
            return Error::invalidArgument("max_results must be an integer");
        }
        const auto count = value.get<int64_t>();
        if (count < 1 || count > kMaxFrustumResults) {
            return Error::invalidArgument("max_results must be from 1 to " +
                                          std::to_string(kMaxFrustumResults));
        }
        request.max_results = count;
    }
    return request;
}

Result<ClearanceRequest> parseClearanceRequest(const json& params) {
    if (!params.is_object()) return Error::invalidArgument("Clearance params must be an object");
    if (!onlyKeys(params, {"shape", "from", "to", "collision_mask"})) {
        return Error::invalidArgument("Clearance request contains an unknown property");
    }
    ClearanceRequest request;
    auto from = parsePoint(params, "from");
    if (from.isErr()) return from.error();
    auto to = parsePoint(params, "to");
    if (to.isErr()) return to.error();
    request.from = from.value();
    request.to = to.value();
    if (request.from.dimension != request.to.dimension) {
        return Error::invalidArgument("from and to must share one dimension");
    }
    // A zero-length sweep is allowed here, unlike a ray: asking whether a shape
    // fits where it stands is a real question, and it is the one a spawn point
    // asks.
    auto mask = layerField(params, "collision_mask");
    if (mask.isErr()) return mask.error();
    request.collision_mask = mask.value();

    if (!params.contains("shape") || !params["shape"].is_object()) {
        return Error::invalidArgument("shape must be an object");
    }
    const auto& shape = params["shape"];
    if (!shape.contains("kind") || !shape["kind"].is_string()) {
        return Error::invalidArgument("shape.kind must be box, sphere, or capsule");
    }
    const auto kind = shape["kind"].get<std::string>();
    const auto extent = [&](const char* field) -> Result<double> {
        if (!shape.contains(field)) {
            return Error::invalidArgument(std::string("shape.") + field + " is required for a " +
                                          kind + " shape");
        }
        const auto& value = shape[field];
        if (!value.is_number()) {
            return Error::invalidArgument(std::string("shape.") + field + " must be a number");
        }
        const double number = value.get<double>();
        if (!std::isfinite(number) || number <= 0.0 || number > kClearanceExtentLimit) {
            return Error::invalidArgument(std::string("shape.") + field +
                                          " must be finite and greater than 0 and at most 100000");
        }
        return number;
    };

    if (kind == "box") {
        if (!onlyKeys(shape, {"kind", "size"})) {
            return Error::invalidArgument("a box shape takes kind and size only");
        }
        auto size = parsePoint(shape, "size");
        if (size.isErr()) return size.error();
        if (size.value().dimension != request.from.dimension) {
            return Error::invalidArgument("shape.size must have the same dimension as from and to");
        }
        // A zero or negative extent is a box that is not there, and Godot would
        // answer about it rather than refuse.
        const bool positive = size.value().x > 0.0 && size.value().y > 0.0 &&
                              (size.value().dimension == 2 || size.value().z > 0.0);
        if (!positive) {
            return Error::invalidArgument("every component of shape.size must be greater than 0");
        }
        request.shape = ClearanceShapeKind::box;
        request.size = size.value();
    } else if (kind == "sphere") {
        if (!onlyKeys(shape, {"kind", "radius"})) {
            return Error::invalidArgument("a sphere shape takes kind and radius only");
        }
        auto radius = extent("radius");
        if (radius.isErr()) return radius.error();
        request.shape = ClearanceShapeKind::sphere;
        request.radius = radius.value();
    } else if (kind == "capsule") {
        if (!onlyKeys(shape, {"kind", "radius", "height"})) {
            return Error::invalidArgument("a capsule shape takes kind, radius and height only");
        }
        auto radius = extent("radius");
        if (radius.isErr()) return radius.error();
        auto height = extent("height");
        if (height.isErr()) return height.error();
        request.shape = ClearanceShapeKind::capsule;
        request.radius = radius.value();
        request.height = height.value();
    } else {
        return Error::invalidArgument("shape.kind must be box, sphere, or capsule");
    }
    return request;
}

Result<NavPathRequest> parseNavPathRequest(const json& params) {
    if (!params.is_object()) return Error::invalidArgument("Navigation params must be an object");
    if (!onlyKeys(params, {"start_point", "end_point", "navigation_layers", "optimize"})) {
        return Error::invalidArgument("Navigation request contains an unknown property");
    }
    NavPathRequest request;
    auto start = parsePoint(params, "start_point");
    if (start.isErr()) return start.error();
    auto end = parsePoint(params, "end_point");
    if (end.isErr()) return end.error();
    request.start_point = start.value();
    request.end_point = end.value();
    if (request.start_point.dimension != request.end_point.dimension) {
        return Error::invalidArgument("start_point and end_point must share one dimension");
    }
    auto layers = layerField(params, "navigation_layers");
    if (layers.isErr()) return layers.error();
    request.navigation_layers = layers.value();
    if (params.contains("optimize")) {
        if (!params["optimize"].is_boolean()) return Error::invalidArgument("optimize must be a boolean");
        request.optimize = params["optimize"].get<bool>();
    }
    return request;
}

} // namespace runtime
} // namespace didi
