#pragma once

#include "didi/common/json.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace didi {
namespace runtime {

// Telling which node painted which pixel.
//
// A segmentation pass draws every geometry node in a flat colour of its own, so
// a picture of the scene becomes a map from pixel to node. That only works if
// the colour in the legend is the colour in the image, and it is not: the
// viewport post-processes after the pass shader writes, and by how much depends
// on the engine. A colour written as (1,73,151) came back as (1,92,186) on
// 4.5.1 and unchanged on 4.7.2.
//
// So the legend does not claim the colour it asked for. The frame is read back,
// each region is matched to the entry it is nearest, and the legend reports
// what was observed beside what was written. That is true on both engines
// without either of them having to behave the same way.
//
// None of this needs Godot, so none of it is in the bridge.
struct SegmentationColor {
    uint8_t r{0};
    uint8_t g{0};
    uint8_t b{0};

    bool operator==(const SegmentationColor& other) const {
        return r == other.r && g == other.g && b == other.b;
    }
};

// Three levels a channel, so a shifted level stays nearer its own written value
// than its neighbour's. Four levels puts 170 and 255 close enough together
// after the shift that the answer would depend on the engine.
inline constexpr int kSegmentationLevels = 3;

// How far a pixel may sit from an entry and still be called that entry. Half
// the distance between two levels, less a margin, so a pixel between two
// entries is unclaimed rather than assigned to whichever is a byte closer.
inline constexpr int kSegmentationMatchRadius = 48;

// The entries, in assignment order. Greys are excluded: a viewport background
// is far more likely to be neutral than coloured, and an entry the background
// could sit on is an entry that would claim pixels no node painted.
const std::vector<SegmentationColor>& segmentationPalette();

// What one entry turned out to occupy in the frame.
struct SegmentationRegion {
    uint64_t pixels{0};
    // The rectangle those pixels fall in, which is the bounding box the report
    // asked for, taken from the picture rather than projected onto it.
    int min_x{0};
    int min_y{0};
    int max_x{0};
    int max_y{0};
    // The commonest colour among the pixels claimed, which is what the legend
    // reports. A mean would be easier and would be wrong: averaging a region
    // whose pixels are 254 and 255 can name a colour that is in neither, and
    // the promise this pass makes is that the colour in the legend is a colour
    // in the picture.
    SegmentationColor observed{};
};

struct SegmentationScan {
    std::vector<SegmentationRegion> regions;  // parallel to the palette prefix in use
    uint64_t unclaimed_pixels{0};
};

// Reads a captured frame back and says where each entry ended up.
SegmentationScan readSegmentation(const uint8_t* rgba, int width, int height, size_t entries);

} // namespace runtime
} // namespace didi
