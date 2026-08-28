#pragma once

#include "didi/common/json.hpp"
#include "didi/common/types.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace didi::image {

inline constexpr int kMaxCaptureDimension = 2048;

struct RgbaImage {
    int width{0};
    int height{0};
    std::vector<uint8_t> rgba;
};

struct DiffBounds {
    int x{0};
    int y{0};
    int width{0};
    int height{0};

    json toJson() const;
};

struct ImageDiffResult {
    int width{0};
    int height{0};
    uint64_t changed_pixels{0};
    uint64_t total_pixels{0};
    double changed_ratio{0.0};
    std::array<double, 4> mean_absolute_error{};
    uint8_t max_channel_delta{0};
    std::optional<DiffBounds> bounds;
    std::vector<uint8_t> diff_rgba;

    json toJson() const;
};

Result<ImageDiffResult> diffRgba(const RgbaImage& before,
                                 const RgbaImage& after,
                                 uint8_t threshold);

Result<size_t> checkedRgbaSize(int64_t width, int64_t height);

} // namespace didi::image
