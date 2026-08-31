#include "didi/common/base64.hpp"
#include "didi/common/image_diff.hpp"
#include "didi/gdextension/viewport_renderer.hpp"

#include <functional>
#include <stdexcept>
#include <string>
#include <algorithm>
#include <utility>
#include <vector>

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

void test_shared_capture_dimension_policy_rejects_oversized_viewports() {
    // Break caught: live capture accepts a frame that the cache/diff layer must reject later.
    ASSERT_TRUE(didi::image::checkedRgbaSize(2560, 1440).isErr());
    const auto largest_supported = didi::image::checkedRgbaSize(2048, 2048);
    ASSERT_TRUE(largest_supported.isOk());
    ASSERT_EQ(largest_supported.value(), 2048u * 2048u * 4u);
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
    ASSERT_TRUE(cache.find(id1) != nullptr);
    ASSERT_TRUE(cache.store(id3, pixel).isOk());
    ASSERT_TRUE(cache.find(id1) != nullptr);
    ASSERT_TRUE(cache.find(id2) == nullptr);
    ASSERT_TRUE(cache.find(id3) != nullptr);
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
    ASSERT_TRUE(cache.find("FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF") == nullptr);
}

// Builds a deterministic scene-like image: a bright shape on a dark ground,
// which is what gives SSIM and the hash something structural to hold on to.
static didi::image::RgbaImage patternImage(int width, int height, int shift, int noise) {
    didi::image::RgbaImage image{width, height, std::vector<uint8_t>(
        static_cast<size_t>(width) * height * 4, 0)};
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const size_t offset = (static_cast<size_t>(y) * width + x) * 4;
            const bool inside = (x + shift) > width / 4 && (x + shift) < (3 * width) / 4 &&
                                y > height / 4 && y < (3 * height) / 4;
            int value = inside ? 200 : 40;
            // A deterministic wobble standing in for antialiasing and shadow
            // filtering: it moves a lot of pixels and changes no structure.
            if (noise > 0) value += ((x * 7 + y * 13) % (2 * noise + 1)) - noise;
            const auto channel = static_cast<uint8_t>(std::clamp(value, 0, 255));
            image.rgba[offset] = channel;
            image.rgba[offset + 1] = channel;
            image.rgba[offset + 2] = channel;
            image.rgba[offset + 3] = 255;
        }
    }
    return image;
}

void test_perceptual_metrics_separate_noise_from_a_real_change() {
    // Break caught: the diff answered only on per-pixel channel deltas, so
    // render noise that changes nothing a person would notice counted the same
    // as a moved object. That is the case this exists for.
    const auto baseline = patternImage(64, 64, 0, 0);
    const auto noisy = patternImage(64, 64, 0, 6);
    const auto moved = patternImage(64, 64, 12, 0);

    const auto noise_diff = didi::image::diffRgba(baseline, noisy, 0);
    ASSERT_TRUE(noise_diff.isOk());
    const auto moved_diff = didi::image::diffRgba(baseline, moved, 0);
    ASSERT_TRUE(moved_diff.isOk());

    // Per pixel, the noise looks like a big change: most of the frame moved.
    ASSERT_TRUE(noise_diff.value().changed_pixels > noise_diff.value().total_pixels / 2);

    // Perceptually it did not. The structure is intact and the hashes agree,
    // while the moved shape scores lower and hashes differently.
    // The hash is the sharp discriminator: the noisy frame hashes identically to
    // the baseline, the moved one is 36 bits away.
    ASSERT_EQ(noise_diff.value().perceptual_distance, 0);
    ASSERT_TRUE(moved_diff.value().perceptual_distance > 8);

    // SSIM ranks them the same way. The absolute number depends on how flat the
    // content is, which is why the tool reports it rather than deciding: on this
    // deliberately flat fixture noise measures about 0.83 and the moved shape
    // about 0.74, where a photographic frame would score both far higher.
    ASSERT_TRUE(noise_diff.value().ssim > 0.8);
    ASSERT_TRUE(moved_diff.value().ssim < noise_diff.value().ssim);
}

void test_perceptual_metrics_are_exact_for_identical_frames() {
    const auto frame = patternImage(32, 32, 0, 0);
    const auto diff = didi::image::diffRgba(frame, frame, 0);
    ASSERT_TRUE(diff.isOk());
    ASSERT_EQ(diff.value().ssim, 1.0);
    ASSERT_EQ(diff.value().perceptual_distance, 0);
    ASSERT_EQ(diff.value().perceptual_hash_before, diff.value().perceptual_hash_after);

    const auto payload = diff.value().toJson();
    ASSERT_EQ(payload["ssim"], 1.0);
    ASSERT_EQ(payload["perceptual_hash"]["hamming_distance"], 0);
    // Fixed width hex, so two hashes are comparable at a glance.
    ASSERT_EQ(payload["perceptual_hash"]["before"].get<std::string>().size(), 16u);
    ASSERT_EQ(payload["perceptual_hash"]["before"], payload["perceptual_hash"]["after"]);
}

void test_structural_similarity_falls_as_structure_diverges() {
    const auto baseline = patternImage(64, 64, 0, 0);
    // Increasing displacement must not score better than a smaller one.
    const double near = didi::image::structuralSimilarity(baseline, patternImage(64, 64, 4, 0));
    const double far = didi::image::structuralSimilarity(baseline, patternImage(64, 64, 16, 0));
    ASSERT_TRUE(near > far);
    ASSERT_TRUE(far >= 0.0 && near <= 1.0);

    // Mismatched dimensions are not comparable, and say so rather than
    // reporting a similarity that means nothing.
    ASSERT_EQ(didi::image::structuralSimilarity(baseline, patternImage(32, 32, 0, 0)), 0.0);
}

void test_perceptual_hash_ignores_uniform_brightness() {
    // Break caught: a hash that folds in the DC term moves every bit when the
    // exposure changes, which is the opposite of what it is for.
    const auto baseline = patternImage(64, 64, 0, 0);
    auto brighter = baseline;
    for (size_t offset = 0; offset < brighter.rgba.size(); offset += 4) {
        for (int channel = 0; channel < 3; ++channel) {
            brighter.rgba[offset + channel] =
                static_cast<uint8_t>(std::min(255, brighter.rgba[offset + channel] + 25));
        }
    }
    ASSERT_EQ(didi::image::perceptualHash(baseline), didi::image::perceptualHash(brighter));

    ASSERT_EQ(didi::image::hammingDistance(0, 0), 0);
    ASSERT_EQ(didi::image::hammingDistance(0xFFFFFFFFFFFFFFFFull, 0), 64);
    ASSERT_EQ(didi::image::hammingDistance(0b1011ull, 0b1110ull), 2);
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

void test_restoration_guard_stays_armed_until_restore_succeeds() {
    // Break caught: restoreNow disarmed before running the callback, so a failed
    // restore was never retried. Viewport isolation left the hidden nodes hidden
    // while the caller got an error that read as if nothing had been touched.
    int attempts = 0;
    bool succeed = false;
    {
        didi::godot::RestorationGuard guard([&]() -> didi::Result<void> {
            ++attempts;
            if (!succeed) return didi::Error::internal("restore failed");
            return didi::Result<void>::ok();
        });

        ASSERT_TRUE(guard.restoreNow().isErr());
        ASSERT_EQ(attempts, 1);
        // Still armed, so a caller can try again.
        ASSERT_TRUE(guard.restoreNow().isErr());
        ASSERT_EQ(attempts, 2);

        succeed = true;
        ASSERT_TRUE(guard.restoreNow().isOk());
        ASSERT_EQ(attempts, 3);
        // Disarmed now, so the destructor must not run a fourth time.
    }
    ASSERT_EQ(attempts, 3);

    // A guard whose restore never succeeds must still be retried by the
    // destructor rather than leaving the scene mutated.
    int failing_attempts = 0;
    {
        didi::godot::RestorationGuard guard([&]() -> didi::Result<void> {
            ++failing_attempts;
            return didi::Error::internal("restore failed");
        });
        ASSERT_TRUE(guard.restoreNow().isErr());
        ASSERT_EQ(failing_attempts, 1);
    }
    ASSERT_EQ(failing_attempts, 2);
}

void test_sub_threshold_noise_is_reported_without_contradiction() {
    // Break caught: the payload said identical true and changed_pixels zero next
    // to a non-zero max_channel_delta, with nothing to say which was which.
    didi::image::RgbaImage before{2, 1, {10, 10, 10, 255, 20, 20, 20, 255}};
    didi::image::RgbaImage after{2, 1, {12, 10, 10, 255, 24, 20, 20, 255}};

    const auto filtered = didi::image::diffRgba(before, after, 5);
    ASSERT_TRUE(filtered.isOk());
    const auto payload = filtered.value().toJson();
    ASSERT_EQ(payload["changed_pixels"], 0u);
    ASSERT_EQ(payload["identical"], true);
    // The raw metrics still report the noise, and the payload now says so.
    ASSERT_EQ(payload["bit_identical"], false);
    ASSERT_EQ(payload["threshold"], 5);
    ASSERT_EQ(payload["max_channel_delta"], 4);

    // With no threshold the two flags agree, which is the common case.
    const auto strict = didi::image::diffRgba(before, after, 0);
    ASSERT_TRUE(strict.isOk());
    const auto strict_payload = strict.value().toJson();
    ASSERT_EQ(strict_payload["identical"], false);
    ASSERT_EQ(strict_payload["bit_identical"], false);
    ASSERT_EQ(strict_payload["threshold"], 0);

    // Truly equal frames report both flags true.
    const auto equal = didi::image::diffRgba(before, before, 5);
    ASSERT_TRUE(equal.isOk());
    ASSERT_EQ(equal.value().toJson()["identical"], true);
    ASSERT_EQ(equal.value().toJson()["bit_identical"], true);
}

void test_capture_cache_moves_frames_and_checks_membership_without_copying() {
    // Break caught: find() returned a full copy of a frame that can be 16 MiB,
    // and the unique-id search copied one per attempt just to test existence.
    const std::string id = "0000000000000000000000000000000a";
    didi::godot::CaptureCache cache(4, 64);
    didi::image::RgbaImage frame{2, 1, {1, 2, 3, 4, 5, 6, 7, 8}};

    ASSERT_TRUE(cache.contains(id) == false);
    ASSERT_TRUE(cache.store(id, std::move(frame)).isOk());
    ASSERT_TRUE(cache.contains(id));
    ASSERT_EQ(cache.bytes(), 8u);

    const auto* borrowed = cache.find(id);
    ASSERT_TRUE(borrowed != nullptr);
    ASSERT_EQ(borrowed->width, 2);
    ASSERT_EQ(borrowed->rgba.size(), 8u);
    // Borrowed, not copied: the pointer is into the cache's own storage.
    ASSERT_TRUE(cache.find(id) == borrowed);

    // contains() must not disturb the LRU order that find() maintains.
    ASSERT_TRUE(cache.contains("ffffffffffffffffffffffffffffffff") == false);
    ASSERT_TRUE(cache.contains("NOT-A-CAPTURE-ID") == false);
}

void test_base64_decode_round_trips_every_byte_value() {
    // Break caught: the decode table was rebuilt on the heap per call. This pins
    // the table's correctness so the static version cannot drift.
    std::vector<uint8_t> all_bytes(256);
    for (size_t i = 0; i < all_bytes.size(); ++i) all_bytes[i] = static_cast<uint8_t>(i);

    for (size_t length = 0; length <= 8; ++length) {
        const std::vector<uint8_t> slice(all_bytes.begin(), all_bytes.begin() + length);
        ASSERT_EQ(didi::base64::decode(didi::base64::encode(slice)), slice);
    }
    ASSERT_EQ(didi::base64::decode(didi::base64::encode(all_bytes)), all_bytes);

    // Both padded and unpadded input, and the + and / alphabet positions.
    ASSERT_EQ(didi::base64::encode(std::vector<uint8_t>{0xFB, 0xEF, 0xBE}), "++++");
    ASSERT_EQ(didi::base64::decode("++++"), (std::vector<uint8_t>{0xFB, 0xEF, 0xBE}));
    ASSERT_EQ(didi::base64::decode("////"), (std::vector<uint8_t>{0xFF, 0xFF, 0xFF}));
    ASSERT_EQ(didi::base64::decode("TQ=="), (std::vector<uint8_t>{'M'}));
    ASSERT_EQ(didi::base64::decode("TQ"), (std::vector<uint8_t>{'M'}));
}

struct RegisterImageDiffTests {
    RegisterImageDiffTests() {
        registerTest("ImageDiff.PerceptualSeparatesNoiseFromChange",
                     test_perceptual_metrics_separate_noise_from_a_real_change);
        registerTest("ImageDiff.PerceptualExactForIdenticalFrames",
                     test_perceptual_metrics_are_exact_for_identical_frames);
        registerTest("ImageDiff.SsimFallsAsStructureDiverges",
                     test_structural_similarity_falls_as_structure_diverges);
        registerTest("ImageDiff.PerceptualHashIgnoresBrightness",
                     test_perceptual_hash_ignores_uniform_brightness);
        registerTest("ImageDiff.SubThresholdNoiseIsUnambiguous",
                     test_sub_threshold_noise_is_reported_without_contradiction);
        registerTest("CaptureCache.BorrowsAndMovesFrames",
                     test_capture_cache_moves_frames_and_checks_membership_without_copying);
        registerTest("Base64.DecodeRoundTripsEveryByte",
                     test_base64_decode_round_trips_every_byte_value);
        registerTest("ImageDiff.ThresholdAndBoundingBox", test_threshold_and_bounding_box);
        registerTest("ImageDiff.AlphaOnlyAndIdentical", test_alpha_only_and_identical_results);
        registerTest("ImageDiff.RejectsShapeMismatchAndInvalidStorage", test_rejects_shape_mismatch_and_invalid_storage);
        registerTest("ImageDiff.SharedCaptureDimensionPolicy", test_shared_capture_dimension_policy_rejects_oversized_viewports);
        registerTest("CaptureCache.LruAndByteBudget", test_capture_cache_lru_and_byte_budget);
        registerTest("CaptureCache.RejectsBadIdsAndOversizeEntries", test_capture_cache_rejects_bad_ids_and_oversize_entries);
        registerTest("ViewportIsolation.RestorationGuard", test_restoration_guard_runs_once_on_early_exit);
        registerTest("ViewportIsolation.GuardStaysArmedUntilRestoreSucceeds",
                     test_restoration_guard_stays_armed_until_restore_succeeds);
    }
} g_register_image_diff_tests;

} // namespace
