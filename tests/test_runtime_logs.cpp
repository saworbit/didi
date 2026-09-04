#include "didi/gdextension/runtime_log.hpp"
#include "didi/common/logger.hpp"

#include <functional>
#include <limits>
#include <stdexcept>
#include <string>

#define ASSERT_TRUE(cond) if (!(cond)) throw std::runtime_error("Assertion failed: " #cond);
#define ASSERT_EQ(a, b) ASSERT_TRUE((a) == (b))

void registerTest(const std::string& name, std::function<void()> fn);

static void test_runtime_log_ring_reports_gap_and_advances_past_filtered_records() {
    // Break caught: returning only matching records without advancing the cursor loops forever.
    didi::godot::RuntimeLogRing ring(3);
    ASSERT_TRUE(ring.append("debug", "test", "one").isOk());
    ASSERT_TRUE(ring.append("info", "test", "two").isOk());
    ASSERT_TRUE(ring.append("warning", "test", "three").isOk());
    ASSERT_TRUE(ring.append("error", "test", "four").isOk());

    const auto page = ring.read(0, 2, "warning").value();
    ASSERT_TRUE(page["dropped_before_cursor"]);
    ASSERT_EQ(page["oldest_cursor"], 2u);
    ASSERT_EQ(page["records"].size(), 2u);
    ASSERT_EQ(page["records"][0]["sequence"], 3u);
    ASSERT_EQ(page["records"][1]["sequence"], 4u);
    ASSERT_EQ(page["next_cursor"], 5u);

    const auto next_page = ring.read(page["next_cursor"].get<uint64_t>(), 2, "warning").value();
    ASSERT_EQ(next_page["records"].size(), 0u);
    ASSERT_EQ(next_page["next_cursor"], 5u);

    const auto filtered = ring.read(2, 2, "error").value();
    ASSERT_EQ(filtered["records"].size(), 1u);
    ASSERT_EQ(filtered["records"][0]["sequence"], 4u);
    ASSERT_EQ(filtered["next_cursor"], 5u);
}

static void test_runtime_log_ring_reports_no_gap_on_the_first_read_from_zero() {
    // Break caught: cursor 0 was compared against the oldest retained sequence,
    // which is 1 on a ring that has never wrapped, so the documented first read
    // of every session came back with dropped_before_cursor true and every
    // record still in the payload. A client that trusts the flag concludes it
    // missed engine output it never missed.
    didi::godot::RuntimeLogRing ring(8);
    ASSERT_TRUE(ring.append("info", "test", "one").isOk());
    ASSERT_TRUE(ring.append("info", "test", "two").isOk());

    const auto page = ring.read(0, 100, "debug").value();
    ASSERT_TRUE(!page["dropped_before_cursor"]);
    ASSERT_EQ(page["oldest_cursor"], 1u);
    ASSERT_EQ(page["records"].size(), 2u);
    ASSERT_EQ(page["records"][0]["sequence"], 1u);
    ASSERT_EQ(page["next_cursor"], 3u);

    // An empty ring has nothing to have dropped either.
    didi::godot::RuntimeLogRing empty(8);
    const auto empty_page = empty.read(0, 100, "debug").value();
    ASSERT_TRUE(!empty_page["dropped_before_cursor"]);
    ASSERT_EQ(empty_page["records"].size(), 0u);

    // A ring that starts at a later sequence keeps the same answer: cursor 0
    // means the beginning of what this ring ever held, not a lost record.
    didi::godot::RuntimeLogRing resumed(8, 500);
    ASSERT_TRUE(resumed.append("info", "test", "later").isOk());
    const auto resumed_page = resumed.read(0, 100, "debug").value();
    ASSERT_TRUE(!resumed_page["dropped_before_cursor"]);
    ASSERT_EQ(resumed_page["oldest_cursor"], 500u);

    // Eviction is still a real gap, whether the caller asks from 0 or from a
    // cursor that named a discarded record.
    didi::godot::RuntimeLogRing wrapped(2);
    ASSERT_TRUE(wrapped.append("info", "test", "one").isOk());
    ASSERT_TRUE(wrapped.append("info", "test", "two").isOk());
    ASSERT_TRUE(wrapped.append("info", "test", "three").isOk());
    ASSERT_TRUE(wrapped.read(0, 100, "debug").value()["dropped_before_cursor"]);
    ASSERT_TRUE(wrapped.read(1, 100, "debug").value()["dropped_before_cursor"]);
    ASSERT_TRUE(!wrapped.read(2, 100, "debug").value()["dropped_before_cursor"]);
}

static void test_runtime_log_ring_bounds_message_and_details() {
    // Break caught: an unbounded message or detail payload can exhaust the extension log buffer.
    didi::godot::RuntimeLogRing ring(1);
    ASSERT_TRUE(ring.append("info", "test", std::string(17000, 'm'), {{"blob", std::string(70000, 'd')}}).isOk());

    const auto record = ring.read(0, 1, "debug").value()["records"][0];
    ASSERT_TRUE(record["message"].get<std::string>().size() <= 16384u);
    ASSERT_TRUE(record.contains("details"));
    ASSERT_TRUE(record["details"].dump().size() <= 65536u);
    ASSERT_TRUE(record["details"].value("truncated", false));
}

static void test_runtime_log_ring_keeps_utf8_payloads_serializable_after_truncation() {
    // Break caught: byte truncation splits a UTF-8 code point and makes JSON serialization throw.
    didi::godot::RuntimeLogRing ring(1);
    const std::string euro = "\xE2\x82\xAC";
    ASSERT_TRUE(ring.append("info", "test", std::string(16383, 'm') + euro,
                            {{"blob", std::string(25000, 'd') + euro + std::string(25000, 'd') + euro}}).isOk());

    const auto record = ring.read(0, 1, "debug").value()["records"][0];
    ASSERT_TRUE(record["message"].get<std::string>().size() <= 16384u);
    ASSERT_TRUE(record["details"].dump().size() <= 65536u);
    ASSERT_TRUE(!record.dump().empty());
}

static void test_runtime_log_ring_replaces_malformed_utf8_scalars() {
    // Break caught: malformed UTF-8 bypasses continuation-shape checks and makes records unserializable.
    didi::godot::RuntimeLogRing ring(8);
    const std::vector<std::string> malformed = {
        "\xC0\xAF",       // overlong slash
        "\xED\xA0\x80", // surrogate
        "\xF4\x90\x80\x80", // above U+10FFFF
        "\xE2\x28\xA1", // invalid continuation
        "\xE2\x82"      // truncated sequence
    };
    for (const auto& value : malformed) {
        ASSERT_TRUE(ring.append("info", value, value, {{"bad", value}}).isOk());
    }
    const auto page = ring.read(0, 8, "debug");
    ASSERT_TRUE(page.isOk());
    for (const auto& record : page.value()["records"]) {
        ASSERT_TRUE(record["details"].is_object());
        ASSERT_TRUE(!record.dump().empty());
    }
}

static void test_runtime_log_ring_rejects_invalid_levels_and_queries() {
    // Break caught: the standalone ring accepts data outside the runtime log public contract.
    didi::godot::RuntimeLogRing ring(2);
    ASSERT_TRUE(ring.append("fatal", "test", "bad").isErr());
    ASSERT_TRUE(ring.read(0, 0, "debug").isErr());
    ASSERT_TRUE(ring.read(0, 501, "debug").isErr());
    ASSERT_TRUE(ring.read(0, 1, "fatal").isErr());
    ASSERT_TRUE(ring.append("info", "test", "good").isOk());
    const auto page = ring.read(0, 1, "debug");
    ASSERT_TRUE(page.isOk());
    ASSERT_TRUE(page.value()["records"][0]["details"].is_null());
}

static void test_runtime_log_ring_stops_before_sequence_wraparound() {
    // Break caught: sequence overflow emits zero or reuses a prior cursor.
    didi::godot::RuntimeLogRing ring(2, std::numeric_limits<uint64_t>::max());
    const auto final_append = ring.append("info", "test", "last");
    ASSERT_TRUE(final_append.isOk());
    ASSERT_EQ(final_append.value(), std::numeric_limits<uint64_t>::max());
    ASSERT_TRUE(ring.append("info", "test", "must not wrap").isErr());
    const auto page = ring.read(0, 1, "debug");
    ASSERT_TRUE(page.isOk());
    ASSERT_EQ(page.value()["records"][0]["sequence"], std::numeric_limits<uint64_t>::max());
    ASSERT_TRUE(page.value()["exhausted"]);
}

static void test_logger_sink_mirrors_records_below_console_threshold() {
    // Break caught: DIDI_LOG_LEVEL suppresses required extension mirror records.
    auto& logger = didi::Logger::instance();
    int mirrored = 0;
    logger.setLevel(didi::LogLevel::Error);
    logger.setSink([&mirrored](didi::LogLevel, std::string_view, std::string_view) { ++mirrored; });
    logger.info("test", "must reach runtime log sink");
    logger.setSink({});
    logger.setLevel(didi::LogLevel::Warn);
    ASSERT_EQ(mirrored, 1);
}

static void test_logger_sink_does_not_recurse_when_sink_logs() {
    // Break caught: a sink that emits a diagnostic re-enters itself until stack exhaustion.
    auto& logger = didi::Logger::instance();
    int invocations = 0;
    logger.setLevel(didi::LogLevel::None);
    logger.setSink([&](didi::LogLevel, std::string_view, std::string_view) {
        ++invocations;
        logger.info("nested", "nested diagnostic");
    });
    logger.info("outer", "outer diagnostic");
    logger.setSink({});
    logger.setLevel(didi::LogLevel::Warn);
    ASSERT_EQ(invocations, 1);
}

struct RegisterRuntimeLogTests {
    RegisterRuntimeLogTests() {
        registerTest("RuntimeLogs.GapAndFiltering", test_runtime_log_ring_reports_gap_and_advances_past_filtered_records);
        registerTest("RuntimeLogs.NoGapOnFirstRead",
                     test_runtime_log_ring_reports_no_gap_on_the_first_read_from_zero);
        registerTest("RuntimeLogs.BoundedPayloads", test_runtime_log_ring_bounds_message_and_details);
        registerTest("RuntimeLogs.Utf8Truncation", test_runtime_log_ring_keeps_utf8_payloads_serializable_after_truncation);
        registerTest("RuntimeLogs.MalformedUtf8", test_runtime_log_ring_replaces_malformed_utf8_scalars);
        registerTest("RuntimeLogs.PublicContract", test_runtime_log_ring_rejects_invalid_levels_and_queries);
        registerTest("RuntimeLogs.SequenceExhaustion", test_runtime_log_ring_stops_before_sequence_wraparound);
        registerTest("RuntimeLogs.LoggerSinkMirroring", test_logger_sink_mirrors_records_below_console_threshold);
        registerTest("RuntimeLogs.LoggerSinkRecursion", test_logger_sink_does_not_recurse_when_sink_logs);
    }
} g_registerRuntimeLogTests;
