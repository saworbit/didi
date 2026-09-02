#include "didi/runtime/profiler_collector.hpp"
#include "didi/mcp/tool_registry.hpp"

#include <cmath>
#include <functional>
#include <limits>
#include <stdexcept>
#include <string>

#define ASSERT_TRUE(cond) \
    if (!(cond)) throw std::runtime_error("Assertion failed: " #cond);
#define ASSERT_EQ(a, b) \
    if (!((a) == (b))) throw std::runtime_error("Assertion failed: " #a " == " #b);

void registerTest(const std::string& name, std::function<void()> fn);

namespace {

using didi::json;
using didi::runtime::kProfilerMetrics;
using didi::runtime::parseProfilerRequest;
using didi::runtime::ProfilerCollector;

std::vector<double> readings(size_t count, double value) {
    return std::vector<double>(count, value);
}

void test_defaults() {
    auto parsed = parseProfilerRequest(json::object());
    ASSERT_TRUE(parsed.isOk());
    ASSERT_EQ(parsed.value().duration_ms, 1000);
    ASSERT_EQ(parsed.value().sample_count, 30);
    ASSERT_EQ(parsed.value().metric_indices.size(), 10u);
}

void test_rejects_bad_requests() {
    const json bad[] = {
        {{"duration_ms", -1}},
        {{"duration_ms", 5001}},
        {{"duration_ms", 1.5}},
        {{"sample_count", 0}},
        {{"sample_count", 121}},
        {{"categories", json::array()}},
        {{"categories", {"frame", "frame"}}},
        {{"categories", {"gpu"}}},
        {{"categories", {"frame", "process", "physics", "render", "frame"}}},
        {{"categories", "frame"}},
        {{"unknown", true}},
        // Duration zero is one sample on the next callback, nothing else.
        {{"duration_ms", 0}, {"sample_count", 2}},
    };
    for (const auto& params : bad) {
        auto parsed = parseProfilerRequest(params);
        ASSERT_TRUE(parsed.isErr());
        ASSERT_EQ(parsed.error().code, 400);
    }
    ASSERT_TRUE(parseProfilerRequest(json::array()).isErr());
    ASSERT_TRUE(parseProfilerRequest({{"duration_ms", 0}, {"sample_count", 1}}).isOk());
}

void test_output_order_ignores_request_order() {
    auto parsed = parseProfilerRequest({{"categories", {"render", "frame"}}});
    ASSERT_TRUE(parsed.isOk());
    ProfilerCollector collector(parsed.value());
    const auto response = collector.response();
    std::vector<std::string> response_names;
    for (const auto& metric : response["metrics"]) {
        response_names.push_back(metric["name"].get<std::string>());
    }
    const std::vector<std::string> expected = {
        "TIME_FPS", "RENDER_TOTAL_OBJECTS_IN_FRAME",
        "RENDER_TOTAL_PRIMITIVES_IN_FRAME", "RENDER_TOTAL_DRAW_CALLS_IN_FRAME"};
    ASSERT_EQ(response_names, expected);
    const std::vector<int64_t> monitors = {0, 11, 12, 13};
    ASSERT_EQ(collector.monitors(), monitors);
}

void test_fixed_metric_table() {
    // The full table in contract order, with the enum values the feasibility
    // gate pinned. A reorder or a renumber here changes what an agent reads.
    const std::vector<std::pair<std::string, int64_t>> expected = {
        {"TIME_FPS", 0}, {"TIME_PROCESS", 1}, {"TIME_PHYSICS_PROCESS", 2},
        {"PHYSICS_2D_ACTIVE_OBJECTS", 17}, {"PHYSICS_2D_COLLISION_PAIRS", 18},
        {"PHYSICS_3D_ACTIVE_OBJECTS", 20}, {"PHYSICS_3D_COLLISION_PAIRS", 21},
        {"RENDER_TOTAL_OBJECTS_IN_FRAME", 11}, {"RENDER_TOTAL_PRIMITIVES_IN_FRAME", 12},
        {"RENDER_TOTAL_DRAW_CALLS_IN_FRAME", 13}};
    ASSERT_EQ(kProfilerMetrics.size(), expected.size());
    for (size_t index = 0; index < expected.size(); ++index) {
        ASSERT_EQ(std::string(kProfilerMetrics[index].name), expected[index].first);
        ASSERT_EQ(kProfilerMetrics[index].monitor, expected[index].second);
    }
}

void test_cadence_is_round_i_times_duration_over_n_minus_one() {
    auto parsed = parseProfilerRequest({{"duration_ms", 100}, {"sample_count", 4},
                                        {"categories", {"frame"}}});
    ASSERT_TRUE(parsed.isOk());
    ProfilerCollector collector(parsed.value());
    // Offsets 0, 33, 67, 100.
    ASSERT_EQ(collector.nextOffsetMs(), 0);
    ASSERT_TRUE(!collector.observe(0, readings(1, 60.0)));
    ASSERT_EQ(collector.nextOffsetMs(), 33);
    // A callback before the offset collects nothing.
    ASSERT_TRUE(!collector.due(32));
    ASSERT_TRUE(!collector.observe(32, readings(1, 1.0)));
    ASSERT_EQ(collector.samplesCollected(), 1);
    // The first callback at or after the offset collects.
    ASSERT_TRUE(!collector.observe(40, readings(1, 30.0)));
    ASSERT_EQ(collector.nextOffsetMs(), 67);
    // One slow frame that crosses the two remaining offsets satisfies both
    // with the same reading and completes the window without stretching it.
    ASSERT_TRUE(collector.observe(150, readings(1, 90.0)));
    ASSERT_TRUE(collector.complete());
    ASSERT_EQ(collector.nextOffsetMs(), -1);
    const auto response = collector.response();
    ASSERT_EQ(response["samples_requested"], 4);
    ASSERT_EQ(response["samples_collected"], 4);
    ASSERT_EQ(response["actual_elapsed_ms"], 150);
    ASSERT_EQ(response["duration_ms"], 100);
    const auto& fps = response["metrics"][0];
    ASSERT_EQ(fps["valid_samples"], 4);
    ASSERT_EQ(fps["invalid_samples"], 0);
    ASSERT_EQ(fps["min"], 30.0);
    ASSERT_EQ(fps["max"], 90.0);
    ASSERT_EQ(fps["last"], 90.0);
    ASSERT_TRUE(std::fabs(fps["mean"].get<double>() - 67.5) < 1e-9);
    // Once complete, a late reading is ignored rather than published.
    ASSERT_TRUE(collector.observe(200, readings(1, 5.0)));
    ASSERT_EQ(collector.response()["samples_collected"], 4);
}

void test_duration_zero_is_one_sample_on_the_next_callback() {
    auto parsed = parseProfilerRequest({{"duration_ms", 0}, {"sample_count", 1},
                                        {"categories", {"physics"}}});
    ASSERT_TRUE(parsed.isOk());
    ProfilerCollector collector(parsed.value());
    ASSERT_TRUE(collector.due(0));
    ASSERT_TRUE(collector.observe(0, readings(4, 0.0)));
    const auto response = collector.response();
    ASSERT_EQ(response["samples_collected"], 1);
    ASSERT_EQ(response["metrics"].size(), 4u);
    // Zero is a legitimate reading, not an unavailable monitor.
    for (const auto& metric : response["metrics"]) {
        ASSERT_EQ(metric["available"], true);
        ASSERT_EQ(metric["availability_basis"], "api_bind_and_enum");
        ASSERT_EQ(metric["valid_samples"], 1);
        ASSERT_EQ(metric["min"], 0.0);
        ASSERT_EQ(metric["mean"], 0.0);
    }
}

void test_non_finite_readings_are_invalid_and_null_statistics_are_explicit() {
    auto parsed = parseProfilerRequest({{"duration_ms", 10}, {"sample_count", 3},
                                        {"categories", {"process"}}});
    ASSERT_TRUE(parsed.isOk());
    ProfilerCollector collector(parsed.value());
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    // TIME_PROCESS never gets a finite reading; TIME_PHYSICS_PROCESS gets two.
    ASSERT_TRUE(!collector.observe(0, {nan, 0.016}));
    ASSERT_TRUE(!collector.observe(5, {inf, nan}));
    ASSERT_TRUE(collector.observe(10, {-inf, 0.020}));
    const auto response = collector.response();
    const auto& process = response["metrics"][0];
    ASSERT_EQ(process["name"], "TIME_PROCESS");
    ASSERT_EQ(process["valid_samples"], 0);
    ASSERT_EQ(process["invalid_samples"], 3);
    for (const auto* key : {"min", "max", "mean", "last"}) {
        ASSERT_TRUE(process.contains(key));
        ASSERT_TRUE(process[key].is_null());
    }
    const auto& physics = response["metrics"][1];
    ASSERT_EQ(physics["valid_samples"], 2);
    ASSERT_EQ(physics["invalid_samples"], 1);
    ASSERT_EQ(physics["min"], 0.016);
    ASSERT_EQ(physics["max"], 0.020);
    ASSERT_EQ(physics["last"], 0.020);
    // Every metric carries exactly the contract fields.
    for (const auto& metric : response["metrics"]) {
        ASSERT_EQ(metric.size(), 10u);
    }
    ASSERT_EQ(response.size(), 5u);
}

void test_mean_stays_inside_the_observed_range() {
    // Three identical readings whose running sum, divided by three, rounds
    // one ulp above the value. Godot 4.6.2 produced exactly this for
    // TIME_PROCESS in the live harness, and a mean above max is a lie.
    auto parsed = parseProfilerRequest({{"duration_ms", 2}, {"sample_count", 3},
                                        {"categories", {"frame"}}});
    ASSERT_TRUE(parsed.isOk());
    ProfilerCollector collector(parsed.value());
    const double value = 2.1060533511106927e-05;
    ASSERT_TRUE((value + value + value) / 3.0 > value);
    collector.observe(0, readings(1, value));
    collector.observe(1, readings(1, value));
    ASSERT_TRUE(collector.observe(2, readings(1, value)));
    const auto response = collector.response();
    const auto& fps = response["metrics"][0];
    ASSERT_EQ(fps["mean"], value);
    ASSERT_EQ(fps["max"], value);
}

void test_largest_response_fits_the_contract_cap() {
    auto parsed = parseProfilerRequest({{"duration_ms", 5000}, {"sample_count", 120}});
    ASSERT_TRUE(parsed.isOk());
    ProfilerCollector collector(parsed.value());
    for (int64_t elapsed = 0; !collector.complete(); elapsed += 1) {
        collector.observe(elapsed, readings(10, 123456.789));
    }
    ASSERT_EQ(collector.response()["samples_collected"], 120);
    ASSERT_TRUE(collector.response().dump().size() < 256u * 1024u);
}

void test_registry_advertises_a_live_read_only_tool() {
    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();
    const auto* tool = registry.getTool("runtime_read_profiler");
    ASSERT_TRUE(tool != nullptr);
    ASSERT_TRUE(tool->capability.implemented);
    const auto description = tool->toJson();
    ASSERT_EQ(description["_meta"]["didi"]["executionModes"], json::array({"live"}));
    ASSERT_TRUE(description["description"].get<std::string>().rfind("UNIMPLEMENTED:", 0) != 0);
    // The request schema is the approved contract, and a read never carries
    // mutation controls.
    ASSERT_EQ(description["inputSchema"]["properties"]["duration_ms"]["maximum"], 5000);
    ASSERT_EQ(description["inputSchema"]["properties"]["sample_count"]["maximum"], 120);
    ASSERT_TRUE(!description["inputSchema"]["properties"].contains("dry_run"));
    // Without a session the call fails for want of a route, not an implementation.
    const auto result = registry.callTool("runtime_read_profiler", json::object());
    ASSERT_TRUE(result.isError);
    ASSERT_TRUE(result.content[0].text.find("no trustworthy execution path") == std::string::npos);
}

struct RegisterPhase7cProfiler {
    RegisterPhase7cProfiler() {
        registerTest("phase7c_profiler.defaults", test_defaults);
        registerTest("phase7c_profiler.rejects_bad_requests", test_rejects_bad_requests);
        registerTest("phase7c_profiler.output_order_ignores_request_order",
                     test_output_order_ignores_request_order);
        registerTest("phase7c_profiler.fixed_metric_table", test_fixed_metric_table);
        registerTest("phase7c_profiler.cadence",
                     test_cadence_is_round_i_times_duration_over_n_minus_one);
        registerTest("phase7c_profiler.duration_zero",
                     test_duration_zero_is_one_sample_on_the_next_callback);
        registerTest("phase7c_profiler.non_finite_and_null_statistics",
                     test_non_finite_readings_are_invalid_and_null_statistics_are_explicit);
        registerTest("phase7c_profiler.mean_within_range", test_mean_stays_inside_the_observed_range);
        registerTest("phase7c_profiler.response_cap", test_largest_response_fits_the_contract_cap);
        registerTest("phase7c_profiler.registry_live_read_only",
                     test_registry_advertises_a_live_read_only_tool);
    }
} g_registerPhase7cProfiler;

} // namespace
