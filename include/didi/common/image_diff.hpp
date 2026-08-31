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

// Two different questions get answered here, and mixing them up is what made
// "identical: true" sit next to "max_channel_delta: 4" in the same payload.
//
//   changed_pixels, changed_ratio, bounds, diff_rgba and the identical flag are
//   THRESHOLDED. A pixel counts only when some channel differs by more than the
//   threshold the caller passed.
//
//   mean_absolute_error and max_channel_delta are RAW. They accumulate every
//   delta, including the sub-threshold ones that were deliberately filtered out.
//
// The payload now carries the threshold that was applied and a separate
// bit_identical flag, so a reader can tell the two apart without guessing.
struct ImageDiffResult {
    int width{0};
    int height{0};
    uint64_t changed_pixels{0};
    uint64_t total_pixels{0};
    double changed_ratio{0.0};
    std::array<double, 4> mean_absolute_error{};
    uint8_t max_channel_delta{0};
    uint8_t threshold{0};
    std::optional<DiffBounds> bounds;
    std::vector<uint8_t> diff_rgba;

    // Perceptual measures, which answer a different question again from either
    // group above: not "which pixels changed" but "does this still look like
    // the same picture". Shadow filtering, antialiasing jitter and particle
    // timing move thousands of pixels without changing what is on screen, and a
    // per-pixel count cannot tell that apart from a real regression.
    //
    // ssim is 1.0 for identical images and falls towards 0 as structure
    // diverges. perceptual_distance is the Hamming distance between the two
    // 64 bit DCT hashes, so 0 means the images hash alike and 64 means nothing
    // in common. Neither is thresholded; callers pick their own tolerance.
    double ssim{1.0};
    uint64_t perceptual_hash_before{0};
    uint64_t perceptual_hash_after{0};
    int perceptual_distance{0};

    json toJson() const;
};

// Mean structural similarity over 8x8 luma blocks, in 0.0 to 1.0. Blocks rather
// than a Gaussian window: it is deterministic, cheap enough to run on every
// diff, and the difference does not matter for deciding whether a frame
// regressed.
double structuralSimilarity(const RgbaImage& before, const RgbaImage& after);

// 64 bit DCT perceptual hash. Two images that look alike hash alike, whatever
// their pixel-level noise.
uint64_t perceptualHash(const RgbaImage& image);

int hammingDistance(uint64_t left, uint64_t right);

Result<ImageDiffResult> diffRgba(const RgbaImage& before,
                                 const RgbaImage& after,
                                 uint8_t threshold);

Result<size_t> checkedRgbaSize(int64_t width, int64_t height);

} // namespace didi::image
