#include "didi/common/image_diff.hpp"
#include "didi/gdextension/viewport_renderer.hpp"

#include <functional>
#include <stdexcept>
#include <string>

#define ASSERT_TRUE(cond) if (!(cond)) throw std::runtime_error("Assertion failed: " #cond)
#define ASSERT_EQ(a, b) ASSERT_TRUE((a) == (b))

void registerTest(const std::string& name, std::function<void()> fn);

namespace {

void test_threshold_and_bounding_box() {
    // Break caught: threshold equality is treated as changed or bounds point at the wrong pixel.
    didi::image::RgbaImage before{2, 1, {0, 0, 0, 255, 10, 10, 10, 255}};
    didi::image::RgbaImage after{2, 1, {0, 0, 0, 255, 12, 9, 10, 255}};
    const auto diff = didi::image::diffRgba(before, after, 1);
    ASSERT_TRUE(diff.isOk());
    ASSERT_EQ(diff.value().changed_pixels, 1u);
    ASSERT_EQ(diff.value().total_pixels, 2u);
    ASSERT_EQ(diff.value().max_channel_delta, 2u);
    ASSERT_TRUE(diff.value().bounds.has_value());
    ASSERT_EQ(diff.value().bounds->x, 1);
    ASSERT_EQ(diff.value().bounds->y, 0);
    ASSERT_EQ(diff.value().bounds->width, 1);
    ASSERT_EQ(diff.value().bounds->height, 1);
    ASSERT_EQ(diff.value().diff_rgba, (std::vector<uint8_t>{0, 0, 0, 0, 2, 1, 0, 255}));
}

void test_alpha_only_and_identical_results() {
    // Break caught: alpha-only changes disappear or identical images report a phantom box.
    didi::image::RgbaImage before{1, 1, {20, 30, 40, 10}};
    didi::image::RgbaImage after{1, 1, {20, 30, 40, 12}};
    const auto changed = didi::image::diffRgba(before, after, 1);
    ASSERT_TRUE(changed.isOk());
    ASSERT_EQ(changed.value().changed_pixels, 1u);
    ASSERT_EQ(changed.value().max_channel_delta, 2u);
    ASSERT_EQ(changed.value().diff_rgba, (std::vector<uint8_t>{0, 0, 0, 255}));

    const auto identical = didi::image::diffRgba(before, before, 0);
    ASSERT_TRUE(identical.isOk());
    ASSERT_EQ(identical.value().changed_pixels, 0u);
    ASSERT_TRUE(!identical.value().bounds.has_value());
    ASSERT_EQ(identical.value().diff_rgba, (std::vector<uint8_t>{0, 0, 0, 0}));
}

void test_rejects_shape_mismatch_and_invalid_storage() {
    // Break caught: unchecked dimensions cause out-of-bounds reads or implicit resampling.
    didi::image::RgbaImage one{1, 1, {0, 0, 0, 255}};
    didi::image::RgbaImage two{2, 1, std::vector<uint8_t>(8, 0)};
    ASSERT_TRUE(didi::image::diffRgba(one, two, 0).isErr());
    didi::image::RgbaImage short_storage{2, 1, std::vector<uint8_t>(7, 0)};
    ASSERT_TRUE(didi::image::diffRgba(short_storage, two, 0).isErr());
    didi::image::RgbaImage invalid{0, 1, {}};
    ASSERT_TRUE(didi::image::diffRgba(invalid, invalid, 0).isErr());
}

void test_capture_cache_lru_and_byte_budget() {
    // Break caught: capture IDs remain forever or eviction ignores recent baseline use.
    const std::string id1 = "00000000000000000000000000000001";
    const std::string id2 = "00000000000000000000000000000002";
    const std::string id3 = "00000000000000000000000000000003";
    didi::image::RgbaImage pixel{1, 1, {1, 2, 3, 4}};
    didi::godot::CaptureCache cache(2, 8);
    ASSERT_TRUE(cache.store(id1, pixel).isOk());
    ASSERT_TRUE(cache.store(id2, pixel).isOk());
    ASSERT_TRUE(cache.find(id1).has_value());
    ASSERT_TRUE(cache.store(id3, pixel).isOk());
    ASSERT_TRUE(cache.find(id1).has_value());
    ASSERT_TRUE(!cache.find(id2).has_value());
    ASSERT_TRUE(cache.find(id3).has_value());
    ASSERT_EQ(cache.size(), 2u);
    ASSERT_EQ(cache.bytes(), 8u);
}

void test_capture_cache_rejects_bad_ids_and_oversize_entries() {
    // Break caught: malformed IDs enter the cache or a single entry exceeds the memory cap.
    didi::godot::CaptureCache cache(8, 4);
    didi::image::RgbaImage pixel{1, 1, {1, 2, 3, 4}};
    ASSERT_TRUE(cache.store("NOT-A-CAPTURE-ID", pixel).isErr());
    didi::image::RgbaImage two_pixels{2, 1, std::vector<uint8_t>(8, 0)};
    ASSERT_TRUE(cache.store("00000000000000000000000000000001", two_pixels).isErr());
    ASSERT_TRUE(!cache.find("FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF").has_value());
}

void test_restoration_guard_runs_once_on_early_exit() {
    // Break caught: an encoding/capture error bypasses temporary editor-state restoration.
    int restorations = 0;
    {
        didi::godot::RestorationGuard guard([&]() {
            ++restorations;
            return didi::Result<void>::ok();
        });
    }
    ASSERT_EQ(restorations, 1);

    {
        didi::godot::RestorationGuard guard([&]() {
            ++restorations;
            return didi::Result<void>::ok();
        });
        ASSERT_TRUE(guard.restoreNow().isOk());
        ASSERT_TRUE(guard.restoreNow().isOk());
    }
    ASSERT_EQ(restorations, 2);
}

struct RegisterImageDiffTests {
    RegisterImageDiffTests() {
        registerTest("ImageDiff.ThresholdAndBoundingBox", test_threshold_and_bounding_box);
        registerTest("ImageDiff.AlphaOnlyAndIdentical", test_alpha_only_and_identical_results);
        registerTest("ImageDiff.RejectsShapeMismatchAndInvalidStorage", test_rejects_shape_mismatch_and_invalid_storage);
        registerTest("CaptureCache.LruAndByteBudget", test_capture_cache_lru_and_byte_budget);
        registerTest("CaptureCache.RejectsBadIdsAndOversizeEntries", test_capture_cache_rejects_bad_ids_and_oversize_entries);
        registerTest("ViewportIsolation.RestorationGuard", test_restoration_guard_runs_once_on_early_exit);
    }
} g_register_image_diff_tests;

} // namespace
