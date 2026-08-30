#include "didi/common/image_diff.hpp"

#include <algorithm>
#include <limits>

namespace didi::image {

Result<size_t> checkedRgbaSize(int64_t width, int64_t height) {
    if (width < 1 || height < 1 || width > kMaxCaptureDimension || height > kMaxCaptureDimension) {
        return Error::invalidArgument("Image dimensions must be from 1 to 2048 pixels");
    }
    const auto pixel_count = static_cast<uint64_t>(width) * static_cast<uint64_t>(height);
    if (pixel_count > std::numeric_limits<size_t>::max() / 4u) {
        return Error::invalidArgument("Image dimensions overflow RGBA storage");
    }
    return static_cast<size_t>(pixel_count * 4u);
}

json DiffBounds::toJson() const {
    return {{"x", x}, {"y", y}, {"width", width}, {"height", height}};
}

json ImageDiffResult::toJson() const {
    return {{"resolution", {{"width", width}, {"height", height}}},
            {"changed_pixels", changed_pixels}, {"total_pixels", total_pixels},
            {"changed_ratio", changed_ratio},
            {"mean_absolute_error", {{"r", mean_absolute_error[0]},
                                      {"g", mean_absolute_error[1]},
                                      {"b", mean_absolute_error[2]},
                                      {"a", mean_absolute_error[3]}}},
            {"max_channel_delta", max_channel_delta},
            {"threshold", threshold},
            {"bounding_box", bounds.has_value() ? json(bounds->toJson()) : json(nullptr)},
            // Nothing exceeded the threshold. Sub-threshold differences can
            // still show up in mean_absolute_error and max_channel_delta.
            {"identical", changed_pixels == 0},
            // No difference at all, at any magnitude.
            {"bit_identical", max_channel_delta == 0}};
}

Result<ImageDiffResult> diffRgba(const RgbaImage& before,
                                 const RgbaImage& after,
                                 uint8_t threshold) {
    if (before.width != after.width || before.height != after.height) {
        return Error(409, "Baseline and comparison capture dimensions must match exactly");
    }
    const auto expected = checkedRgbaSize(before.width, before.height);
    if (expected.isErr()) return expected.error();
    if (before.rgba.size() != expected.value() || after.rgba.size() != expected.value()) {
        return Error::invalidArgument("RGBA storage length does not match image dimensions");
    }

    ImageDiffResult result;
    result.width = before.width;
    result.height = before.height;
    result.total_pixels = static_cast<uint64_t>(before.width) * static_cast<uint64_t>(before.height);
    result.diff_rgba.assign(expected.value(), 0);
    std::array<uint64_t, 4> sums{};
    int min_x = before.width;
    int min_y = before.height;
    int max_x = -1;
    int max_y = -1;

    for (uint64_t pixel = 0; pixel < result.total_pixels; ++pixel) {
        const size_t offset = static_cast<size_t>(pixel * 4u);
        bool changed = false;
        std::array<uint8_t, 4> deltas{};
        for (size_t channel = 0; channel < 4; ++channel) {
            const auto left = before.rgba[offset + channel];
            const auto right = after.rgba[offset + channel];
            const auto delta = static_cast<uint8_t>(left > right ? left - right : right - left);
            deltas[channel] = delta;
            sums[channel] += delta;
            result.max_channel_delta = std::max(result.max_channel_delta, delta);
            changed = changed || delta > threshold;
        }
        if (!changed) continue;
        ++result.changed_pixels;
        const int x = static_cast<int>(pixel % static_cast<uint64_t>(before.width));
        const int y = static_cast<int>(pixel / static_cast<uint64_t>(before.width));
        min_x = std::min(min_x, x);
        min_y = std::min(min_y, y);
        max_x = std::max(max_x, x);
        max_y = std::max(max_y, y);
        result.diff_rgba[offset] = deltas[0];
        result.diff_rgba[offset + 1] = deltas[1];
        result.diff_rgba[offset + 2] = deltas[2];
        result.diff_rgba[offset + 3] = 255;
    }

    result.threshold = threshold;
    result.changed_ratio = static_cast<double>(result.changed_pixels) /
                           static_cast<double>(result.total_pixels);
    for (size_t channel = 0; channel < 4; ++channel) {
        result.mean_absolute_error[channel] = static_cast<double>(sums[channel]) /
                                              static_cast<double>(result.total_pixels);
    }
    if (result.changed_pixels > 0) {
        result.bounds = DiffBounds{min_x, min_y, max_x - min_x + 1, max_y - min_y + 1};
    }
    return result;
}

} // namespace didi::image
