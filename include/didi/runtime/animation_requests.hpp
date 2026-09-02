#pragma once

#include "didi/common/json.hpp"
#include "didi/common/types.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace didi {
namespace runtime {

struct AnimListRequest {
    std::string animation_player_path;
};

struct AnimPlayRequest {
    std::string animation_player_path;
    std::string animation_name;
    double custom_speed{1.0};
    bool from_end{false};
};

// 400 on anything outside the approved Phase 7B contracts: unknown keys,
// path or name length, a zero or non-finite speed, a speed outside -16..16,
// or a negative speed without from_end.
Result<AnimListRequest> parseAnimListRequest(const json& params);
Result<AnimPlayRequest> parseAnimPlayRequest(const json& params);

// Plain data the engine side collects, in engine order. Caps are applied by
// the builder, so the collector only has to stop reading one past each cap.
struct AnimationTrackInfo {
    int64_t index{0};
    int64_t type_id{0};
    std::string path;
    std::vector<double> key_times;
    bool key_times_cut{false};
};

struct AnimationInfo {
    std::string name;
    double length{0.0};
    int64_t loop_mode_id{0};
    std::vector<AnimationTrackInfo> tracks;
    bool tracks_cut{false};
};

const char* animationTrackTypeName(int64_t type_id);
const char* animationLoopModeName(int64_t loop_mode_id);

constexpr size_t kMaxAnimations = 128;
constexpr size_t kMaxTracksPerAnimation = 128;
constexpr size_t kMaxKeysPerTrack = 256;
constexpr size_t kMaxAnimationCatalogBytes = 256u * 1024u;

// Sorts animations by UTF-8 name, applies the count caps and the byte budget,
// and emits the contract payload with a truncation cursor when it stopped.
json buildAnimationCatalog(std::vector<AnimationInfo> animations);

} // namespace runtime
} // namespace didi
