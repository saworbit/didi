#include "didi/gdextension/runtime_log.hpp"
#include "didi/common/logger.hpp"

#include <functional>
#include <stdexcept>
#include <string>

#define ASSERT_TRUE(cond) if (!(cond)) throw std::runtime_error("Assertion failed: " #cond);
#define ASSERT_EQ(a, b) ASSERT_TRUE((a) == (b))

void registerTest(const std::string& name, std::function<void()> fn);

static void test_runtime_log_ring_reports_gap_and_advances_past_filtered_records() {
    // Break caught: returning only matching records without advancing the cursor loops forever.
    didi::godot::RuntimeLogRing ring(3);
    ring.append("debug", "test", "one");
    ring.append("info", "test", "two");
    ring.append("warning", "test", "three");
    ring.append("error", "test", "four");

    const auto page = ring.read(0, 2, "warning");
    ASSERT_TRUE(page["dropped_before_cursor"]);
    ASSERT_EQ(page["oldest_cursor"], 2u);
    ASSERT_EQ(page["records"].size(), 2u);
    ASSERT_EQ(page["records"][0]["sequence"], 3u);
    ASSERT_EQ(page["records"][1]["sequence"], 4u);
    ASSERT_EQ(page["next_cursor"], 5u);

    const auto next_page = ring.read(page["next_cursor"].get<uint64_t>(), 2, "warning");
    ASSERT_EQ(next_page["records"].size(), 0u);
    ASSERT_EQ(next_page["next_cursor"], 5u);

    const auto filtered = ring.read(2, 2, "error");
    ASSERT_EQ(filtered["records"].size(), 1u);
    ASSERT_EQ(filtered["records"][0]["sequence"], 4u);
    ASSERT_EQ(filtered["next_cursor"], 5u);
}

static void test_runtime_log_ring_bounds_message_and_details() {
    // Break caught: an unbounded message or detail payload can exhaust the extension log buffer.
    didi::godot::RuntimeLogRing ring(1);
    ring.append("info", "test", std::string(17000, 'm'), {{"blob", std::string(70000, 'd')}});

    const auto record = ring.read(0, 1, "debug")["records"][0];
    ASSERT_TRUE(record["message"].get<std::string>().size() <= 16384u);
    ASSERT_TRUE(record.contains("details"));
    ASSERT_TRUE(record["details"].dump().size() <= 65536u);
    ASSERT_TRUE(record["details"].value("truncated", false));
}

static void test_runtime_log_ring_keeps_utf8_payloads_serializable_after_truncation() {
    // Break caught: byte truncation splits a UTF-8 code point and makes JSON serialization throw.
    didi::godot::RuntimeLogRing ring(1);
    const std::string euro = "\xE2\x82\xAC";
    ring.append("info", "test", std::string(16383, 'm') + euro,
                {{"blob", std::string(25000, 'd') + euro + std::string(25000, 'd') + euro}});

    const auto record = ring.read(0, 1, "debug")["records"][0];
    ASSERT_TRUE(record["message"].get<std::string>().size() <= 16384u);
    ASSERT_TRUE(record["details"].dump().size() <= 65536u);
    ASSERT_TRUE(!record.dump().empty());
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

struct RegisterRuntimeLogTests {
    RegisterRuntimeLogTests() {
        registerTest("RuntimeLogs.GapAndFiltering", test_runtime_log_ring_reports_gap_and_advances_past_filtered_records);
        registerTest("RuntimeLogs.BoundedPayloads", test_runtime_log_ring_bounds_message_and_details);
        registerTest("RuntimeLogs.Utf8Truncation", test_runtime_log_ring_keeps_utf8_payloads_serializable_after_truncation);
        registerTest("RuntimeLogs.LoggerSinkMirroring", test_logger_sink_mirrors_records_below_console_threshold);
    }
} g_registerRuntimeLogTests;
