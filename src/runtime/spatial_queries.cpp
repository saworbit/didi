#include "didi/runtime/spatial_queries.hpp"

#include <cmath>
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
