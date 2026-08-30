// Engine output capture.
//
// Didi has always been able to report what Didi did. It could not report what
// the *engine* said -- a `print()` in a running game, a script error with its
// file and line -- because nothing was subscribed to Godot's output. An agent
// that writes a script then runs it had no way to read what it printed, which
// is the ordinary inner loop of working on a game.
//
// These exercise the contract that closes that gap. The load-bearing one is
// that engine output is a *separate* record stream from Didi's own diagnostics:
// if the two share a ring, `runtime_read_output` merely repeats
// `runtime_read_logs` and the capability is an illusion.

#include "didi/gdextension/editor_hook.hpp"
#include "didi/gdextension/runtime_log.hpp"
#include "didi/mcp/tool_registry.hpp"
#include "didi/runtime/session_kind_policy.hpp"

#include <functional>
#include <stdexcept>
#include <string>

#define ASSERT_TRUE(cond) if (!(cond)) throw std::runtime_error("Assertion failed: " #cond);
#define ASSERT_FALSE(cond) ASSERT_TRUE(!(cond))
#define ASSERT_EQ(a, b) ASSERT_TRUE((a) == (b))

void registerTest(const std::string& name, std::function<void()> fn);

// The whole point of the tool. Engine output and Didi's own records are two
// different questions, so they must be two different streams.
static void test_engine_output_is_a_separate_stream_from_didi_records() {
    auto& hook = didi::godot::EditorHook::instance();
    didi::godot::EditorHookTestAccess::setSessionKind(hook, didi::runtime::SessionKind::editor);
    ASSERT_TRUE(hook.runtimeLogs().append("info", "didi", "a didi diagnostic").isOk());
    ASSERT_TRUE(hook.engineOutput().append("info", "godot", "an engine print").isOk());

    const auto output = didi::godot::EditorHookTestAccess::executeOnMainThread(
        hook, "runtime.getOutput", {{"cursor", 0}, {"limit", 100}});
    ASSERT_FALSE(output.contains("error"));

    bool saw_engine_line = false;
    for (const auto& record : output.at("records")) {
        const auto message = record.at("message").get<std::string>();
        // Didi's own diagnostics must never surface here.
        ASSERT_TRUE(message != "a didi diagnostic");
        if (message == "an engine print") saw_engine_line = true;
    }
    ASSERT_TRUE(saw_engine_line);
}

// The reverse direction: engine chatter must not pollute Didi's diagnostics,
// or `runtime_read_logs` becomes unusable the moment a game prints in a loop.
static void test_didi_records_do_not_carry_engine_output() {
    auto& hook = didi::godot::EditorHook::instance();
    didi::godot::EditorHookTestAccess::setSessionKind(hook, didi::runtime::SessionKind::editor);
    ASSERT_TRUE(hook.engineOutput().append("info", "godot", "engine chatter").isOk());

    const auto logs = didi::godot::EditorHookTestAccess::executeOnMainThread(
        hook, "runtime.getLogs", {{"cursor", 0}, {"limit", 500}});
    ASSERT_FALSE(logs.contains("error"));
    for (const auto& record : logs.at("records")) {
        ASSERT_TRUE(record.at("message").get<std::string>() != "engine chatter");
    }
}

// Cursor paging is what makes the tool usable in a loop rather than a
// one-shot dump, so it must behave like the log ring's.
static void test_engine_output_pages_by_cursor() {
    auto& hook = didi::godot::EditorHook::instance();
    didi::godot::EditorHookTestAccess::setSessionKind(hook, didi::runtime::SessionKind::editor);
    const auto first = didi::godot::EditorHookTestAccess::executeOnMainThread(
        hook, "runtime.getOutput", {{"cursor", 0}, {"limit", 500}});
    const auto cursor = first.at("next_cursor").get<uint64_t>();
    ASSERT_TRUE(hook.engineOutput().append("warning", "godot", "later line").isOk());

    const auto second = didi::godot::EditorHookTestAccess::executeOnMainThread(
        hook, "runtime.getOutput", {{"cursor", cursor}, {"limit", 500}});
    ASSERT_EQ(second.at("records").size(), 1u);
    ASSERT_EQ(second.at("records")[0].at("message").get<std::string>(), std::string("later line"));
}

static void test_engine_output_rejects_malformed_queries() {
    auto& hook = didi::godot::EditorHook::instance();
    didi::godot::EditorHookTestAccess::setSessionKind(hook, didi::runtime::SessionKind::editor);
    for (const didi::json params : {didi::json{{"limit", 0}},
                                    didi::json{{"limit", 100000}},
                                    didi::json{{"cursor", -1}},
                                    didi::json{{"minimum_level", "chatty"}}}) {
        const auto result = didi::godot::EditorHookTestAccess::executeOnMainThread(
            hook, "runtime.getOutput", params);
        ASSERT_TRUE(result.contains("error"));
        ASSERT_EQ(result["error"]["code"].get<int>(), 400);
    }
}

// Reading output changes nothing, so a client must be able to auto-approve it.
static void test_runtime_read_output_is_registered_as_a_read_only_tool() {
    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();
    const auto* tool = registry.getTool("runtime_read_output");
    ASSERT_TRUE(tool != nullptr);
    ASSERT_FALSE(tool->legacy);
    ASSERT_TRUE(tool->capability.implemented);
    const auto definition = tool->toJson();
    ASSERT_EQ(definition["annotations"]["readOnlyHint"].get<bool>(), true);
    ASSERT_EQ(definition["annotations"]["destructiveHint"].get<bool>(), false);
}

struct RegisterRuntimeOutputTests {
    RegisterRuntimeOutputTests() {
        registerTest("RuntimeOutput.SeparateFromDidiRecords", test_engine_output_is_a_separate_stream_from_didi_records);
        registerTest("RuntimeOutput.DidiRecordsStayClean", test_didi_records_do_not_carry_engine_output);
        registerTest("RuntimeOutput.CursorPaging", test_engine_output_pages_by_cursor);
        registerTest("RuntimeOutput.RejectsMalformedQueries", test_engine_output_rejects_malformed_queries);
        registerTest("RuntimeOutput.ToolIsReadOnly", test_runtime_read_output_is_registered_as_a_read_only_tool);
    }
} g_registerRuntimeOutputTests;
