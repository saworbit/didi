#include "didi/common/image_diff.hpp"
#include <vector>
#include <iomanip>
#include <sstream>
#include <cmath>
#include <array>

#include <algorithm>
#include <limits>

namespace didi::image {
namespace {

// Fixed width so two hashes are always comparable at a glance in a payload.
std::string perceptualHashHex(uint64_t hash) {
    std::ostringstream out;
    out << std::hex << std::setfill('0') << std::setw(16) << hash;
    return out.str();
}

} // namespace


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
            {"bit_identical", max_channel_delta == 0},
            // Neither of these is thresholded. ssim is 1.0 for identical
            // images; perceptual_distance is 0 when the two hash alike.
            {"ssim", ssim},
            {"perceptual_hash", {{"before", perceptualHashHex(perceptual_hash_before)},
                                  {"after", perceptualHashHex(perceptual_hash_after)},
                                  {"hamming_distance", perceptual_distance}}}};
}

namespace {

// Rec. 709 luma. The perceptual measures work on brightness, because that is
// where structure lives; a colour shift that preserves luma is a different
// question and the raw channel metrics already answer it.
std::vector<double> lumaPlane(const RgbaImage& image) {
    std::vector<double> luma(static_cast<size_t>(image.width) * image.height, 0.0);
    for (size_t pixel = 0; pixel < luma.size(); ++pixel) {
        const size_t offset = pixel * 4;
        luma[pixel] = 0.2126 * image.rgba[offset] +
                      0.7152 * image.rgba[offset + 1] +
                      0.0722 * image.rgba[offset + 2];
    }
    return luma;
}

// Box-average down to size x size. Averaging rather than sampling, so a single
// noisy pixel cannot move a hash bit on its own.
std::vector<double> downsampleLuma(const std::vector<double>& luma, int width, int height,
                                   int size) {
    std::vector<double> small(static_cast<size_t>(size) * size, 0.0);
    for (int y = 0; y < size; ++y) {
        const int y0 = static_cast<int>(static_cast<int64_t>(y) * height / size);
        const int y1 = std::max(y0 + 1, static_cast<int>(static_cast<int64_t>(y + 1) * height / size));
        for (int x = 0; x < size; ++x) {
            const int x0 = static_cast<int>(static_cast<int64_t>(x) * width / size);
            const int x1 = std::max(x0 + 1, static_cast<int>(static_cast<int64_t>(x + 1) * width / size));
            double total = 0.0;
            size_t count = 0;
            for (int sy = y0; sy < y1 && sy < height; ++sy) {
                for (int sx = x0; sx < x1 && sx < width; ++sx) {
                    total += luma[static_cast<size_t>(sy) * width + sx];
                    ++count;
                }
            }
            small[static_cast<size_t>(y) * size + x] = count > 0 ? total / static_cast<double>(count) : 0.0;
        }
    }
    return small;
}

} // namespace

double structuralSimilarity(const RgbaImage& before, const RgbaImage& after) {
    if (before.width != after.width || before.height != after.height) return 0.0;
    if (before.width <= 0 || before.height <= 0) return 1.0;

    const auto left = lumaPlane(before);
    const auto right = lumaPlane(after);

    // Identical frames are exactly 1.0, said once rather than arrived at.
    // (2*ml*mr + C1) and (ml^2 + mr^2 + C1) are equal in arithmetic when the
    // means are equal, but not bit for bit in floating point, so the general
    // path lands an ulp either side of 1.0 depending on the compiler. A caller
    // comparing against 1.0 would then see identical frames score below it.
    if (left == right) return 1.0;

    // The standard stabilisers for 8 bit data, so a flat block does not divide
    // by a variance of zero.
    constexpr double kC1 = (0.01 * 255.0) * (0.01 * 255.0);
    constexpr double kC2 = (0.03 * 255.0) * (0.03 * 255.0);
    constexpr int kBlock = 8;

    double total = 0.0;
    size_t blocks = 0;
    for (int by = 0; by < before.height; by += kBlock) {
        for (int bx = 0; bx < before.width; bx += kBlock) {
            double sum_l = 0.0, sum_r = 0.0;
            double sum_ll = 0.0, sum_rr = 0.0, sum_lr = 0.0;
            size_t count = 0;
            for (int y = by; y < std::min(by + kBlock, before.height); ++y) {
                for (int x = bx; x < std::min(bx + kBlock, before.width); ++x) {
                    const size_t index = static_cast<size_t>(y) * before.width + x;
                    const double l = left[index];
                    const double r = right[index];
                    sum_l += l;
                    sum_r += r;
                    sum_ll += l * l;
                    sum_rr += r * r;
                    sum_lr += l * r;
                    ++count;
                }
            }
            if (count == 0) continue;
            const double n = static_cast<double>(count);
            const double mean_l = sum_l / n;
            const double mean_r = sum_r / n;
            const double var_l = sum_ll / n - mean_l * mean_l;
            const double var_r = sum_rr / n - mean_r * mean_r;
            const double covariance = sum_lr / n - mean_l * mean_r;
            const double numerator = (2.0 * mean_l * mean_r + kC1) * (2.0 * covariance + kC2);
            const double denominator =
                (mean_l * mean_l + mean_r * mean_r + kC1) * (var_l + var_r + kC2);
            total += denominator > 0.0 ? numerator / denominator : 1.0;
            ++blocks;
        }
    }
    if (blocks == 0) return 1.0;
    return std::clamp(total / static_cast<double>(blocks), 0.0, 1.0);
}

uint64_t perceptualHash(const RgbaImage& image) {
    if (image.width <= 0 || image.height <= 0) return 0;
    constexpr int kSize = 32;
    constexpr int kLow = 8;

    const auto small = downsampleLuma(lumaPlane(image), image.width, image.height, kSize);

    // DCT-II over the 32x32 plane, but only the low frequency 8x8 corner is
    // ever read, so the rest is never computed.
    std::array<double, kLow * kLow> coefficients{};
    for (int u = 0; u < kLow; ++u) {
        for (int v = 0; v < kLow; ++v) {
            double sum = 0.0;
            for (int y = 0; y < kSize; ++y) {
                const double cy = std::cos((2.0 * y + 1.0) * u * 3.14159265358979323846 / (2.0 * kSize));
                for (int x = 0; x < kSize; ++x) {
                    const double cx =
                        std::cos((2.0 * x + 1.0) * v * 3.14159265358979323846 / (2.0 * kSize));
                    sum += small[static_cast<size_t>(y) * kSize + x] * cy * cx;
                }
            }
            const double au = u == 0 ? std::sqrt(0.5) : 1.0;
            const double av = v == 0 ? std::sqrt(0.5) : 1.0;
            coefficients[static_cast<size_t>(u) * kLow + v] = au * av * sum;
        }
    }

    // The DC term carries overall brightness, not structure, so it is excluded
    // from the median and from the hash. A uniform exposure change should not
    // move every bit.
    std::array<double, kLow * kLow - 1> ac{};
    for (size_t index = 1; index < coefficients.size(); ++index) ac[index - 1] = coefficients[index];
    auto sorted = ac;
    std::sort(sorted.begin(), sorted.end());
    const double median = (sorted[sorted.size() / 2 - 1] + sorted[sorted.size() / 2]) / 2.0;

    uint64_t hash = 0;
    for (size_t index = 0; index < ac.size(); ++index) {
        if (ac[index] > median) hash |= (uint64_t{1} << index);
    }
    return hash;
}

int hammingDistance(uint64_t left, uint64_t right) {
    uint64_t difference = left ^ right;
    int count = 0;
    while (difference != 0) {
        difference &= difference - 1;
        ++count;
    }
    return count;
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
    result.ssim = structuralSimilarity(before, after);
    result.perceptual_hash_before = perceptualHash(before);
    result.perceptual_hash_after = perceptualHash(after);
    result.perceptual_distance =
        hammingDistance(result.perceptual_hash_before, result.perceptual_hash_after);
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
