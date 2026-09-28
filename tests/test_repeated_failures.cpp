// Repeated failure tests (Q6 in docs/BUILD_QUEUE.md, #1040).
//
// The second identical failure of the same call says so, and points at the fix
// the refusal already carries. Nothing is refused for being repeated.

#include "didi/mcp/mcp_server.hpp"
#include "didi/mcp/repeated_failures.hpp"

#include <functional>
#include <stdexcept>
#include <string>

#define ASSERT_TRUE(cond) if (!(cond)) throw std::runtime_error("Assertion failed: " #cond);
#define ASSERT_FALSE(cond) ASSERT_TRUE(!(cond))
#define ASSERT_EQ(a, b) ASSERT_TRUE((a) == (b))

void registerTest(const std::string& name, std::function<void()> fn);

namespace {

using didi::json;
using didi::mcp::CallToolResult;

CallToolResult refusal(const std::string& code, const json& remedy = json::object()) {
    json data = {{"code", code}};
    data.update(remedy);
    auto result = CallToolResult::error(json{{"error", {{"code", 409}, {"message", "no"}, {"data", data}}}}.dump());
    return result;
}

json repeatedOf(const CallToolResult& result) {
    const auto envelope = didi::mcp::errorEnvelopeOf(result);
    if (!envelope.has_value()) return nullptr;
    return envelope->value("data", json::object()).value("repeated", json());
}

}  // namespace

static void test_the_second_identical_failure_says_so_and_points_at_the_fix() {
    didi::mcp::RepeatedFailures tracker;
    const json arguments = {{"target_node", "Player"}};
    const json fix = {{"next_call", {{"tool", "scene_get_hierarchy"}}}};

    auto first = refusal("not_found", fix);
    tracker.observe("legacy", "scene_get_property", arguments, first);
    ASSERT_TRUE(repeatedOf(first).is_null());

    auto second = refusal("not_found", fix);
    tracker.observe("legacy", "scene_get_property", arguments, second);
    const auto repeated = repeatedOf(second);
    ASSERT_EQ(repeated["count"], 2);
    ASSERT_EQ(repeated["follow"], "next_call");
    ASSERT_TRUE(repeated["note"].get<std::string>().find("next_call") != std::string::npos);
    // Still the refusal it was: nothing is refused for being repeated.
    ASSERT_TRUE(second.isError);

    auto third = refusal("not_found", fix);
    tracker.observe("legacy", "scene_get_property", arguments, third);
    ASSERT_EQ(repeatedOf(third)["count"], 3);
}

static void test_only_the_same_call_failing_the_same_way_counts() {
    didi::mcp::RepeatedFailures tracker;
    const json arguments = {{"target_node", "Player"}};
    auto first = refusal("not_found");
    tracker.observe("legacy", "scene_get_property", arguments, first);

    // Other arguments are another call.
    auto other = refusal("not_found");
    tracker.observe("legacy", "scene_get_property", {{"target_node", "Enemy"}}, other);
    ASSERT_TRUE(repeatedOf(other).is_null());

    // Another tool is another call.
    auto tool = refusal("not_found");
    tracker.observe("legacy", "scene_set_property", arguments, tool);
    ASSERT_TRUE(repeatedOf(tool).is_null());

    // The same call failing another way starts again.
    auto changed = refusal("session_kind_rejected");
    tracker.observe("legacy", "scene_get_property", arguments, changed);
    ASSERT_TRUE(repeatedOf(changed).is_null());

    // A success ends the run, so the failure after it is a first.
    auto success = CallToolResult::successJson({{"value", 1}});
    tracker.observe("legacy", "scene_get_property", arguments, success);
    auto after = refusal("session_kind_rejected");
    tracker.observe("legacy", "scene_get_property", arguments, after);
    ASSERT_TRUE(repeatedOf(after).is_null());

    // Another conversation keeps its own count.
    auto elsewhere = refusal("session_kind_rejected");
    tracker.observe("modern/abc", "scene_get_property", arguments, elsewhere);
    ASSERT_TRUE(repeatedOf(elsewhere).is_null());
}

// A confirmation token is new on every attempt; it does not make a retry new.
static void test_a_new_confirmation_token_is_the_same_call() {
    didi::mcp::RepeatedFailures tracker;
    auto first = refusal("conflict", {{"retry_with", {{"replace", true}}}});
    tracker.observe("legacy", "scene_create", {{"scene_path", "res://a.tscn"}, {"confirmation_token", "one"}}, first);
    auto second = refusal("conflict", {{"retry_with", {{"replace", true}}}});
    tracker.observe("legacy", "scene_create", {{"scene_path", "res://a.tscn"}, {"confirmation_token", "two"}}, second);
    ASSERT_EQ(repeatedOf(second)["follow"], "retry_with");
}

// Remembering is bounded, and forgetting the oldest call is all it costs.
static void test_what_is_remembered_is_bounded() {
    didi::mcp::RepeatedFailures tracker;
    for (int i = 0; i < 5000; ++i) {
        auto failure = refusal("not_found");
        tracker.observe("legacy", "scene_get_property", {{"target_node", std::to_string(i)}}, failure);
        auto success = CallToolResult::successJson({{"value", i}});
        if (i % 2 == 0) {
            tracker.observe("legacy", "scene_get_property", {{"target_node", std::to_string(i)}}, success);
        }
    }
    // The most recent failure is still remembered.
    auto again = refusal("not_found");
    tracker.observe("legacy", "scene_get_property", {{"target_node", "4999"}}, again);
    ASSERT_EQ(repeatedOf(again)["count"], 2);
}

// Through a real server: the same offline refusal, twice.
static void test_a_server_marks_the_second_identical_refusal() {
    didi::mcp::McpServer server;
    didi::mcp::JsonRpcRequest init;
    init.id = 1;
    init.method = "initialize";
    init.params = {{"protocolVersion", didi::mcp::kProtocolVersion}};
    server.handleRequest(init);
    auto call = [&](int id) {
        didi::mcp::JsonRpcRequest req;
        req.id = id;
        req.method = "tools/call";
        req.params = {{"name", "scene_get_property"},
                      {"arguments", {{"target_node", "/root/Nope"}, {"property_name", "position"}}}};
        const auto answer = server.handleRequest(req).result;
        return json::parse(answer["content"][0]["text"].get<std::string>());
    };
    const auto first = call(2);
    ASSERT_TRUE(first.contains("error"));
    ASSERT_FALSE(first["error"]["data"].contains("repeated"));
    const auto second = call(3);
    ASSERT_EQ(second["error"]["data"]["repeated"]["count"], 2);
    ASSERT_TRUE(second["error"]["data"]["repeated"].contains("follow"));
}

struct RegisterRepeatedFailureTests {
    RegisterRepeatedFailureTests() {
        registerTest("RepeatedFailures.SecondIdenticalFailureSaysSo",
                     test_the_second_identical_failure_says_so_and_points_at_the_fix);
        registerTest("RepeatedFailures.OnlyTheSameCallTheSameWay",
                     test_only_the_same_call_failing_the_same_way_counts);
        registerTest("RepeatedFailures.NewTokenIsTheSameCall",
                     test_a_new_confirmation_token_is_the_same_call);
        registerTest("RepeatedFailures.RememberingIsBounded", test_what_is_remembered_is_bounded);
        registerTest("RepeatedFailures.ServerMarksTheSecondRefusal",
                     test_a_server_marks_the_second_identical_refusal);
    }
} g_register_repeated_failure_tests;
