#include "didi/runtime/invariant_watch.hpp"

#include <functional>
#include <stdexcept>
#include <string>

#define ASSERT_TRUE(cond) if (!(cond)) throw std::runtime_error("Assertion failed: " #cond);
#define ASSERT_EQ(a, b) ASSERT_TRUE((a) == (b))

void registerTest(const std::string& name, std::function<void()> fn);

namespace {

using didi::runtime::InvariantReading;
using didi::runtime::InvariantSample;
using didi::runtime::InvariantWatch;
using didi::runtime::parseInvariantWatchRequest;

didi::runtime::InvariantWatchRequest parsed(const didi::json& params) {
    auto request = parseInvariantWatchRequest(params);
    ASSERT_TRUE(request.isOk());
    return request.value();
}

InvariantSample reading(double value) {
    InvariantSample sample;
    sample.readings.push_back(InvariantReading{value, ""});
    return sample;
}

static void test_watch_rejects_a_request_nothing_could_violate() {
    // Every one of these is a request that would produce a report saying an
    // invariant held, on a condition that could never have failed.
    const auto rejected = [](const didi::json& params) {
        return parseInvariantWatchRequest(params).isErr();
    };

    // A range with no bound cannot be broken.
    ASSERT_TRUE(rejected({{"invariants", didi::json::array({
        {{"kind", "performance_between"}, {"metric", "TIME_FPS"}}})}}));
    // An inverted range cannot be satisfied, which is the same mistake the
    // other way up.
    ASSERT_TRUE(rejected({{"invariants", didi::json::array({
        {{"kind", "performance_between"}, {"metric", "TIME_FPS"},
         {"minimum", 60}, {"maximum", 30}}})}}));
    // A monitor this build cannot read is named, not silently dropped.
    ASSERT_TRUE(rejected({{"invariants", didi::json::array({
        {{"kind", "performance_between"}, {"metric", "TIME_MADE_UP"}, {"minimum", 1}}})}}));
    ASSERT_TRUE(rejected({{"invariants", didi::json::array({
        {{"kind", "not_a_kind"}, {"minimum", 1}}})}}));
    ASSERT_TRUE(rejected({{"invariants", didi::json::array()}}));
    ASSERT_TRUE(rejected({{"duration_ms", 0}, {"invariants", didi::json::array({
        {{"kind", "no_engine_errors"}}})}}));
    ASSERT_TRUE(rejected({{"duration_ms", 30001}, {"invariants", didi::json::array({
        {{"kind", "no_engine_errors"}}})}}));

    // no_engine_errors needs no bounds: it is violated by any error at all.
    ASSERT_TRUE(parseInvariantWatchRequest(
        {{"invariants", didi::json::array({{{"kind", "no_engine_errors"}}})}}).isOk());
    // One-sided ranges are the common case and must be accepted.
    ASSERT_TRUE(parseInvariantWatchRequest(
        {{"invariants", didi::json::array({
            {{"kind", "expression_between"}, {"expression", "node.get('position').y"},
             {"context_node", "/root/Main/Player"}, {"minimum", -1000}}})}}).isOk());
}

static void test_watch_stops_on_the_frame_that_broke_the_condition() {
    // The point of sampling in the engine rather than polling from outside: the
    // frame that broke it is the frame that gets reported, and the watch stops
    // there rather than running the window out.
    InvariantWatch watch(parsed({{"duration_ms", 1000}, {"invariants", didi::json::array({
        {{"name", "player_alive"}, {"kind", "expression_between"},
         {"expression", "node.get('health')"}, {"context_node", "/root/Main/Player"},
         {"minimum", 1}, {"maximum", 100}}})}}));

    ASSERT_TRUE(!watch.observe(10, reading(100.0)));
    ASSERT_TRUE(!watch.observe(20, reading(42.0)));
    ASSERT_TRUE(watch.observe(30, reading(0.0)));
    ASSERT_TRUE(watch.violated());

    const auto report = watch.response(true);
    ASSERT_EQ(report["outcome"], "violated");
    ASSERT_EQ(report["violation"]["name"], "player_alive");
    ASSERT_EQ(report["violation"]["observed"].get<double>(), 0.0);
    ASSERT_EQ(report["violation"]["bound"], "minimum");
    ASSERT_EQ(report["violation"]["limit"].get<double>(), 1.0);
    ASSERT_EQ(report["violation"]["elapsed_ms"].get<int64_t>(), 30);
    ASSERT_EQ(report["violation"]["sample"].get<int>(), 3);
    ASSERT_EQ(report["paused"], true);
    // The range it did hold over is evidence the watch was actually looking.
    ASSERT_EQ(report["invariants"][0]["readings"].get<int>(), 3);
    ASSERT_EQ(report["invariants"][0]["maximum_observed"].get<double>(), 100.0);

    // A watch that has already stopped stays stopped, so a late frame cannot
    // overwrite the reproduction with a later one.
    ASSERT_TRUE(watch.observe(40, reading(50.0)));
    ASSERT_EQ(watch.response(true)["violation"]["observed"].get<double>(), 0.0);
}

static void test_a_condition_never_read_is_not_a_condition_that_held() {
    // Break the report would otherwise tell: an expression that fails every
    // frame, on a node path that does not exist, would come back as an
    // invariant that held for the whole window. That is the same false success
    // as an empty impact list read as permission.
    InvariantWatch watch(parsed({{"duration_ms", 30}, {"invariants", didi::json::array({
        {{"name", "unreadable"}, {"kind", "expression_between"},
         {"expression", "node.get('health')"}, {"context_node", "/root/Missing"},
         {"minimum", 1}}})}}));

    InvariantSample unread;
    unread.readings.push_back(InvariantReading{std::nullopt, "context node was not found"});
    ASSERT_TRUE(!watch.observe(10, unread));
    ASSERT_TRUE(watch.observe(30, unread));
    ASSERT_TRUE(!watch.violated());

    const auto report = watch.response(false);
    ASSERT_EQ(report["outcome"], "inconclusive");
    ASSERT_EQ(report["invariants"][0]["readings"].get<int>(), 0);
    ASSERT_EQ(report["invariants"][0]["read_error"], "context node was not found");
    ASSERT_TRUE(!report["invariants"][0].contains("minimum_observed"));

    // And the same watch, once it can read, reports what it saw.
    InvariantWatch readable(parsed({{"duration_ms", 30}, {"invariants", didi::json::array({
        {{"kind", "expression_between"}, {"expression", "node.get('health')"},
         {"minimum", 1}}})}}));
    ASSERT_TRUE(!readable.observe(10, reading(5.0)));
    ASSERT_TRUE(readable.observe(30, reading(7.0)));
    ASSERT_EQ(readable.response(false)["outcome"], "held");
}

static void test_engine_errors_break_the_watch_and_frame_rate_is_a_minimum() {
    // An unhandled script error is not something a script can be asked about
    // from outside, but the engine says so on its error stream, and that is
    // what this kind reads.
    InvariantWatch errors(parsed({{"duration_ms", 100}, {"invariants", didi::json::array({
        {{"name", "clean_run"}, {"kind", "no_engine_errors"}}})}}));
    InvariantSample quiet;
    quiet.engine_errors = 0;
    ASSERT_TRUE(!errors.observe(10, quiet));
    InvariantSample noisy;
    noisy.engine_errors = 2;
    ASSERT_TRUE(errors.observe(20, noisy));
    const auto report = errors.response(true);
    ASSERT_EQ(report["outcome"], "violated");
    ASSERT_EQ(report["violation"]["kind"], "no_engine_errors");
    ASSERT_EQ(report["violation"]["engine_errors"].get<int64_t>(), 2);
    // A count is not a range, so it carries no bound to misread.
    ASSERT_TRUE(!report["violation"].contains("limit"));

    // A minimum frame rate is a one-sided range on a Performance monitor, and
    // the report names the monitor so a reader is not left with a number.
    InvariantWatch frames(parsed({{"duration_ms", 100}, {"invariants", didi::json::array({
        {{"name", "playable"}, {"kind", "performance_between"},
         {"metric", "TIME_FPS"}, {"minimum", 50}}})}}));
    ASSERT_TRUE(!frames.observe(10, reading(60.0)));
    ASSERT_TRUE(frames.observe(20, reading(31.0)));
    const auto frame_report = frames.response(false);
    ASSERT_EQ(frame_report["outcome"], "violated");
    ASSERT_EQ(frame_report["violation"]["metric"], "TIME_FPS");
    ASSERT_EQ(frame_report["violation"]["observed"].get<double>(), 31.0);
    // Asked to pause and reporting that it did not is the honest pair.
    ASSERT_EQ(frame_report["paused"], false);
}

struct RegisterInvariantWatchTests {
    RegisterInvariantWatchTests() {
        registerTest("InvariantWatch.RejectsUnviolatableRequests",
                     test_watch_rejects_a_request_nothing_could_violate);
        registerTest("InvariantWatch.StopsOnTheBreakingFrame",
                     test_watch_stops_on_the_frame_that_broke_the_condition);
        registerTest("InvariantWatch.UnreadIsNotHeld",
                     test_a_condition_never_read_is_not_a_condition_that_held);
        registerTest("InvariantWatch.EngineErrorsAndFrameRate",
                     test_engine_errors_break_the_watch_and_frame_rate_is_a_minimum);
    }
} g_registerInvariantWatchTests;

} // namespace
