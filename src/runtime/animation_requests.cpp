#include "didi/runtime/animation_requests.hpp"

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

Result<std::string> boundedString(const json& params, const char* key, size_t maximum) {
    if (!params.contains(key)) return Error::invalidArgument(std::string(key) + " is required");
    const auto& value = params[key];
    if (!value.is_string()) return Error::invalidArgument(std::string(key) + " must be a string");
    const auto& text = value.get_ref<const std::string&>();
    if (text.empty() || text.size() > maximum) {
        return Error::invalidArgument(std::string(key) + " must be 1 to " + std::to_string(maximum) + " bytes");
    }
    if (text.find('\0') != std::string::npos) {
        return Error::invalidArgument(std::string(key) + " may not contain NUL");
    }
    return text;
}

} // namespace

Result<AnimListRequest> parseAnimListRequest(const json& params) {
    if (!params.is_object()) return Error::invalidArgument("Animation params must be an object");
    if (!onlyKeys(params, {"animation_player_path"})) {
        return Error::invalidArgument("Animation request contains an unknown property");
    }
    auto path = boundedString(params, "animation_player_path", 1024);
    if (path.isErr()) return path.error();
    return AnimListRequest{path.value()};
}

Result<AnimPlayRequest> parseAnimPlayRequest(const json& params) {
    if (!params.is_object()) return Error::invalidArgument("Animation params must be an object");
    if (!onlyKeys(params, {"animation_player_path", "animation_name", "custom_speed", "from_end"})) {
        return Error::invalidArgument("Animation request contains an unknown property");
    }
    AnimPlayRequest request;
    auto path = boundedString(params, "animation_player_path", 1024);
    if (path.isErr()) return path.error();
    request.animation_player_path = path.value();
    auto name = boundedString(params, "animation_name", 256);
    if (name.isErr()) return name.error();
    request.animation_name = name.value();
    if (params.contains("custom_speed")) {
        const auto& speed = params["custom_speed"];
        if (!speed.is_number()) return Error::invalidArgument("custom_speed must be a number");
        request.custom_speed = speed.get<double>();
        if (!std::isfinite(request.custom_speed) || request.custom_speed == 0.0 ||
            request.custom_speed < -16.0 || request.custom_speed > 16.0) {
            return Error::invalidArgument("custom_speed must be finite, non-zero and within -16..16");
        }
    }
    if (params.contains("from_end")) {
        if (!params["from_end"].is_boolean()) return Error::invalidArgument("from_end must be a boolean");
        request.from_end = params["from_end"].get<bool>();
    }
    if (request.custom_speed < 0.0 && !request.from_end) {
        return Error::invalidArgument("A negative custom_speed requires from_end: true");
    }
    return request;
}

const char* animationTrackTypeName(int64_t type_id) {
    switch (type_id) {
        case 0: return "value";
        case 1: return "position_3d";
        case 2: return "rotation_3d";
        case 3: return "scale_3d";
        case 4: return "blend_shape";
        case 5: return "method";
        case 6: return "bezier";
        case 7: return "audio";
        case 8: return "animation";
        default: return "unknown";
    }
}

const char* animationLoopModeName(int64_t loop_mode_id) {
    switch (loop_mode_id) {
        case 0: return "none";
        case 1: return "linear";
        case 2: return "pingpong";
        default: return "unknown";
    }
}

json buildAnimationCatalog(std::vector<AnimationInfo> animations) {
    std::stable_sort(animations.begin(), animations.end(),
                     [](const AnimationInfo& a, const AnimationInfo& b) { return a.name < b.name; });

    json output = json::array();
    json cursor = nullptr;
    bool truncated = false;
    size_t bytes = 2;  // the enclosing brackets
    auto stop = [&](size_t animation_index, size_t track_index, size_t key_index, const char* reason) {
        truncated = true;
        cursor = {{"animation_index", animation_index}, {"track_index", track_index},
                  {"key_index", key_index}, {"reason", reason}};
    };

    for (size_t animation_index = 0; animation_index < animations.size(); ++animation_index) {
        if (animation_index >= kMaxAnimations) { stop(animation_index, 0, 0, "count"); break; }
        const auto& animation = animations[animation_index];
        json tracks = json::array();
        bool animation_truncated = animation.tracks_cut;
        for (size_t track_index = 0; track_index < animation.tracks.size(); ++track_index) {
            if (track_index >= kMaxTracksPerAnimation) { animation_truncated = true; break; }
            const auto& track = animation.tracks[track_index];
            json key_times = json::array();
            bool track_truncated = track.key_times_cut;
            for (size_t key_index = 0; key_index < track.key_times.size(); ++key_index) {
                if (key_index >= kMaxKeysPerTrack) { track_truncated = true; break; }
                key_times.push_back(track.key_times[key_index]);
            }
            tracks.push_back({{"index", track.index}, {"type_id", track.type_id},
                              {"type_name", animationTrackTypeName(track.type_id)},
                              {"path", track.path}, {"key_times", std::move(key_times)},
                              {"truncated", track_truncated}});
        }
        json record = {{"name", animation.name}, {"length", animation.length},
                       {"loop_mode_id", animation.loop_mode_id},
                       {"loop_mode_name", animationLoopModeName(animation.loop_mode_id)},
                       {"tracks", std::move(tracks)}, {"truncated", animation_truncated}};
        const size_t record_bytes = record.dump().size() + 1;
        if (bytes + record_bytes > kMaxAnimationCatalogBytes) {
            // Stop before the record rather than emitting a partial one.
            stop(animation_index, 0, 0, "bytes");
            break;
        }
        bytes += record_bytes;
        output.push_back(std::move(record));
    }
    return {{"animations", std::move(output)}, {"truncated", truncated}, {"truncated_at", cursor}};
}

} // namespace runtime
} // namespace didi
