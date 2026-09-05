#include "didi/runtime/segmentation.hpp"

#include <functional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#define ASSERT_TRUE(cond) \
    if (!(cond)) throw std::runtime_error("Assertion failed: " #cond);
#define ASSERT_EQ(a, b) \
    if (!((a) == (b))) throw std::runtime_error("Assertion failed: " #a " == " #b);

void registerTest(const std::string& name, std::function<void()> fn);

namespace {

using didi::runtime::kSegmentationMatchRadius;
using didi::runtime::readSegmentation;
using didi::runtime::SegmentationColor;
using didi::runtime::segmentationPalette;

std::vector<uint8_t> frameOf(int width, int height, SegmentationColor background) {
    std::vector<uint8_t> rgba(static_cast<size_t>(width) * height * 4u, 255);
    for (size_t index = 0; index < rgba.size(); index += 4) {
        rgba[index] = background.r;
        rgba[index + 1] = background.g;
        rgba[index + 2] = background.b;
        rgba[index + 3] = 255;
    }
    return rgba;
}

void paint(std::vector<uint8_t>& rgba, int width, int x0, int y0, int x1, int y1,
           SegmentationColor color) {
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            const size_t offset = (static_cast<size_t>(y) * width + x) * 4u;
            rgba[offset] = color.r;
            rgba[offset + 1] = color.g;
            rgba[offset + 2] = color.b;
        }
    }
}

void test_palette_entries_stay_apart_and_avoid_neutral_colours() {
    const auto& palette = segmentationPalette();
    ASSERT_TRUE(palette.size() >= 20);

    std::set<std::string> seen;
    for (const auto& entry : palette) {
        // A neutral entry is one a grey background could sit on, and a
        // background claiming to be a node is the failure this pass exists to
        // avoid.
        ASSERT_TRUE(!(entry.r == entry.g && entry.g == entry.b));
        seen.insert(std::to_string(entry.r) + "," + std::to_string(entry.g) + "," +
                    std::to_string(entry.b));
    }
    ASSERT_EQ(seen.size(), palette.size());

    // Every pair has to be further apart than twice the match radius, or a
    // pixel could be inside the radius of two entries at once and the answer
    // would depend on which was checked first.
    for (size_t left = 0; left < palette.size(); ++left) {
        for (size_t right = left + 1; right < palette.size(); ++right) {
            const int dr = int(palette[left].r) - int(palette[right].r);
            const int dg = int(palette[left].g) - int(palette[right].g);
            const int db = int(palette[left].b) - int(palette[right].b);
            const int distance = dr * dr + dg * dg + db * db;
            ASSERT_TRUE(distance > kSegmentationMatchRadius * kSegmentationMatchRadius);
        }
    }
}

void test_scan_finds_each_region_its_bounds_and_the_colour_it_actually_has() {
    const auto& palette = segmentationPalette();
    const int width = 40;
    const int height = 20;
    // Mid grey, which no palette entry is, so nothing claims it.
    auto rgba = frameOf(width, height, SegmentationColor{128, 128, 128});
    paint(rgba, width, 2, 3, 9, 8, palette[0]);
    paint(rgba, width, 20, 10, 29, 15, palette[1]);

    const auto scan = readSegmentation(rgba.data(), width, height, 3);
    ASSERT_EQ(scan.regions.size(), 3u);

    ASSERT_EQ(scan.regions[0].pixels, 8u * 6u);
    ASSERT_EQ(scan.regions[0].min_x, 2);
    ASSERT_EQ(scan.regions[0].min_y, 3);
    ASSERT_EQ(scan.regions[0].max_x, 9);
    ASSERT_EQ(scan.regions[0].max_y, 8);
    ASSERT_TRUE(scan.regions[0].observed == palette[0]);

    ASSERT_EQ(scan.regions[1].pixels, 10u * 6u);
    ASSERT_EQ(scan.regions[1].min_x, 20);
    ASSERT_EQ(scan.regions[1].max_y, 15);

    // An entry nothing painted is reported with no pixels rather than left out,
    // because a node that is occluded or off screen is an answer.
    ASSERT_EQ(scan.regions[2].pixels, 0u);

    const uint64_t painted = scan.regions[0].pixels + scan.regions[1].pixels;
    ASSERT_EQ(scan.unclaimed_pixels, static_cast<uint64_t>(width) * height - painted);
}

void test_scan_matches_a_colour_the_viewport_shifted_and_reports_what_it_saw() {
    // The 4.5.1 measurement that stopped this pass shipping: a colour written
    // as (1,73,151) came back as (1,92,186). The legend has to survive that,
    // and it has to say what it saw rather than what it asked for.
    const auto& palette = segmentationPalette();
    const int width = 16;
    const int height = 16;
    auto rgba = frameOf(width, height, SegmentationColor{60, 60, 60});

    const auto shifted = [](uint8_t value) -> uint8_t {
        // Monotone and biggest in the middle, the shape the measurement showed.
        if (value == 0) return 0;
        if (value == 255) return 255;
        return static_cast<uint8_t>(value + 29);
    };
    // An entry with a mid level in it. The first entries are made of 0 and 255
    // only, which a monotone shift leaves alone, so they would prove nothing.
    size_t mid_entry = 0;
    while (mid_entry < palette.size() && palette[mid_entry].r != 128 &&
           palette[mid_entry].g != 128 && palette[mid_entry].b != 128) {
        ++mid_entry;
    }
    ASSERT_TRUE(mid_entry < palette.size());
    const SegmentationColor written = palette[mid_entry];
    const SegmentationColor seen{shifted(written.r), shifted(written.g), shifted(written.b)};
    paint(rgba, width, 4, 4, 11, 11, seen);

    const auto scan = readSegmentation(rgba.data(), width, height, mid_entry + 1);
    ASSERT_EQ(scan.regions[mid_entry].pixels, 64u);
    // Matched to the right entry, and reporting the colour in the picture.
    ASSERT_TRUE(scan.regions[mid_entry].observed == seen);
    ASSERT_TRUE(!(scan.regions[mid_entry].observed == written));
    // And nothing else claimed it.
    for (size_t entry = 0; entry < mid_entry; ++entry) {
        ASSERT_EQ(scan.regions[entry].pixels, 0u);
    }
}

void test_observed_colour_is_one_the_picture_actually_has() {
    // Break caught: the legend reported the mean of the region, and averaging
    // pixels that are 255 and 253 names 254, which is in neither. The promise
    // is that the colour in the legend is a colour in the picture.
    const auto& palette = segmentationPalette();
    const int width = 10;
    const int height = 1;
    auto rgba = frameOf(width, height, SegmentationColor{128, 128, 128});
    const SegmentationColor common{static_cast<uint8_t>(palette[0].r == 0 ? 2 : palette[0].r - 2),
                                   palette[0].g, palette[0].b};
    const SegmentationColor rare{static_cast<uint8_t>(palette[0].r == 0 ? 6 : palette[0].r - 6),
                                 palette[0].g, palette[0].b};
    paint(rgba, width, 0, 0, 6, 0, common);
    paint(rgba, width, 7, 0, 9, 0, rare);

    const auto scan = readSegmentation(rgba.data(), width, height, 1);
    ASSERT_EQ(scan.regions[0].pixels, 10u);
    ASSERT_TRUE(scan.regions[0].observed == common);
}

void test_scan_leaves_a_pixel_between_two_entries_to_nobody() {
    const auto& palette = segmentationPalette();
    const int width = 8;
    const int height = 8;
    // Halfway between the first two entries: nearer to neither than the radius.
    const SegmentationColor between{
        static_cast<uint8_t>((int(palette[0].r) + int(palette[1].r)) / 2),
        static_cast<uint8_t>((int(palette[0].g) + int(palette[1].g)) / 2),
        static_cast<uint8_t>((int(palette[0].b) + int(palette[1].b)) / 2)};
    auto rgba = frameOf(width, height, between);

    const auto scan = readSegmentation(rgba.data(), width, height, 2);
    ASSERT_EQ(scan.regions[0].pixels, 0u);
    ASSERT_EQ(scan.regions[1].pixels, 0u);
    ASSERT_EQ(scan.unclaimed_pixels, 64u);
}

struct RegisterSegmentation {
    RegisterSegmentation() {
        registerTest("Segmentation.PaletteStaysApart",
                     test_palette_entries_stay_apart_and_avoid_neutral_colours);
        registerTest("Segmentation.ScanFindsRegions",
                     test_scan_finds_each_region_its_bounds_and_the_colour_it_actually_has);
        registerTest("Segmentation.ScanSurvivesAShiftedViewport",
                     test_scan_matches_a_colour_the_viewport_shifted_and_reports_what_it_saw);
        registerTest("Segmentation.ObservedColourIsInThePicture",
                     test_observed_colour_is_one_the_picture_actually_has);
        registerTest("Segmentation.AmbiguousPixelIsUnclaimed",
                     test_scan_leaves_a_pixel_between_two_entries_to_nobody);
    }
} g_registerSegmentation;

} // namespace
