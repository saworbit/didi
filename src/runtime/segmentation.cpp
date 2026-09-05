#include "didi/runtime/segmentation.hpp"

#include <algorithm>
#include <limits>
#include <map>

namespace didi {
namespace runtime {

namespace {

int levelValue(int level) {
    // 0, 128, 255. Evenly spread across the byte range, and the two ends are
    // exact so an unshifted engine reproduces them byte for byte.
    static constexpr int values[kSegmentationLevels] = {0, 128, 255};
    return values[level];
}

bool isNeutral(const SegmentationColor& color) {
    return color.r == color.g && color.g == color.b;
}

std::vector<SegmentationColor> buildPalette() {
    std::vector<SegmentationColor> palette;
    palette.reserve(kSegmentationLevels * kSegmentationLevels * kSegmentationLevels);
    for (int r = 0; r < kSegmentationLevels; ++r) {
        for (int g = 0; g < kSegmentationLevels; ++g) {
            for (int b = 0; b < kSegmentationLevels; ++b) {
                SegmentationColor color{static_cast<uint8_t>(levelValue(r)),
                                        static_cast<uint8_t>(levelValue(g)),
                                        static_cast<uint8_t>(levelValue(b))};
                if (isNeutral(color)) continue;
                palette.push_back(color);
            }
        }
    }
    // Neighbouring entries differ in one channel, which puts two nodes that are
    // next to each other in the scene in two colours that are hard to tell
    // apart by eye. Ordering by how far apart the entries are keeps the first
    // few assignments, which is most scenes, visibly distinct.
    std::sort(palette.begin(), palette.end(), [](const SegmentationColor& left,
                                                 const SegmentationColor& right) {
        const auto spread = [](const SegmentationColor& color) {
            const int high = std::max({color.r, color.g, color.b});
            const int low = std::min({color.r, color.g, color.b});
            return high - low;
        };
        if (spread(left) != spread(right)) return spread(left) > spread(right);
        if (left.r != right.r) return left.r > right.r;
        if (left.g != right.g) return left.g > right.g;
        return left.b > right.b;
    });
    return palette;
}

int squaredDistance(const SegmentationColor& left, const SegmentationColor& right) {
    const int dr = static_cast<int>(left.r) - static_cast<int>(right.r);
    const int dg = static_cast<int>(left.g) - static_cast<int>(right.g);
    const int db = static_cast<int>(left.b) - static_cast<int>(right.b);
    return dr * dr + dg * dg + db * db;
}

} // namespace

const std::vector<SegmentationColor>& segmentationPalette() {
    static const std::vector<SegmentationColor> palette = buildPalette();
    return palette;
}

SegmentationScan readSegmentation(const uint8_t* rgba, int width, int height, size_t entries) {
    SegmentationScan scan;
    const auto& palette = segmentationPalette();
    entries = std::min(entries, palette.size());
    scan.regions.resize(entries);
    if (!rgba || width <= 0 || height <= 0 || entries == 0) return scan;

    // Bounded on purpose. Inside the match radius a region is near uniform, so
    // a handful of shades is the normal case; a region that somehow produces
    // more keeps the commonest of the first ones seen rather than growing a map
    // the size of the frame.
    constexpr size_t kMaxShadesPerRegion = 256;
    std::vector<std::map<uint32_t, uint64_t>> shades(entries);
    for (auto& region : scan.regions) {
        region.min_x = width;
        region.min_y = height;
        region.max_x = -1;
        region.max_y = -1;
    }

    const int radius_squared = kSegmentationMatchRadius * kSegmentationMatchRadius;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const size_t offset = (static_cast<size_t>(y) * static_cast<size_t>(width) +
                                   static_cast<size_t>(x)) * 4u;
            const SegmentationColor pixel{rgba[offset], rgba[offset + 1], rgba[offset + 2]};
            size_t nearest = 0;
            int nearest_distance = std::numeric_limits<int>::max();
            for (size_t entry = 0; entry < entries; ++entry) {
                const int distance = squaredDistance(pixel, palette[entry]);
                if (distance < nearest_distance) {
                    nearest_distance = distance;
                    nearest = entry;
                }
            }
            // Past the radius the pixel is background, or an edge where two
            // colours blended. Either way it belongs to nobody, and saying so
            // is the difference between a legend and a guess.
            if (nearest_distance > radius_squared) {
                ++scan.unclaimed_pixels;
                continue;
            }
            auto& region = scan.regions[nearest];
            ++region.pixels;
            region.min_x = std::min(region.min_x, x);
            region.min_y = std::min(region.min_y, y);
            region.max_x = std::max(region.max_x, x);
            region.max_y = std::max(region.max_y, y);
            const uint32_t key = (static_cast<uint32_t>(pixel.r) << 16) |
                                 (static_cast<uint32_t>(pixel.g) << 8) |
                                 static_cast<uint32_t>(pixel.b);
            auto& counts = shades[nearest];
            auto found = counts.find(key);
            if (found != counts.end()) {
                ++found->second;
            } else if (counts.size() < kMaxShadesPerRegion) {
                counts.emplace(key, 1);
            }
        }
    }

    for (size_t entry = 0; entry < entries; ++entry) {
        auto& region = scan.regions[entry];
        if (region.pixels == 0) {
            region.min_x = 0;
            region.min_y = 0;
            region.max_x = 0;
            region.max_y = 0;
            continue;
        }
        uint32_t best_key = 0;
        uint64_t best_count = 0;
        for (const auto& shade : shades[entry]) {
            if (shade.second > best_count) {
                best_count = shade.second;
                best_key = shade.first;
            }
        }
        region.observed = SegmentationColor{static_cast<uint8_t>((best_key >> 16) & 0xFFu),
                                            static_cast<uint8_t>((best_key >> 8) & 0xFFu),
                                            static_cast<uint8_t>(best_key & 0xFFu)};
    }
    return scan;
}

} // namespace runtime
} // namespace didi
