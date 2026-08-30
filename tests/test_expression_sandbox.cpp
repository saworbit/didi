#include "didi/gdextension/expression_sandbox.hpp"

#include <functional>
#include <stdexcept>
#include <string>

#define ASSERT_TRUE(cond) if (!(cond)) throw std::runtime_error("Assertion failed: " #cond);
#define ASSERT_EQ(a, b) ASSERT_TRUE((a) == (b))

void registerTest(const std::string& name, std::function<void()> fn);

namespace {

void test_expression_policy_accepts_only_the_documented_read_only_vocabulary() {
    // Break caught: a documented read-only expression is rejected by an implementation-derived allowlist.
    for (const auto& source : {
        "node.get('process_priority')", "node.get_child_count()", "[1, 2, 3].size()",
        "'OS.execute is text'", "clamp(7, 0, 5)"
    }) {
        ASSERT_TRUE(didi::godot::ExpressionPolicy::validate(source).isOk());
    }

    // Break caught: filesystem, process, mutation, dispatch, assignment, or reflection reaches Expression.parse.
    for (const auto& source : {
        "OS.execute('cmd', [])", "FileAccess.open('res://x', 1)", "node.set('x', 1)",
        "node.call('queue_free')", "node.queue_free()", "load('res://x.gd')", "x = 1",
        "while true: pass", "node.get_script().get_source_code()", "str(node)",
        "'%s' % node", "node.get_property_list()", "node.get_child(0).get('name')",
        "{'a': 1}.get('a')", "node.dangerous_property", "node['dangerous_property']",
        "node.get_child(0).dangerous_property", "'dynamic_property' in node",
        "node.get_node('/root')", "node.get_node_or_null('..')", "node.has_node('/root')",
        "node.get_node('/root').get_child_count()", "node.get_node('..').get_path()",
        "node.get_child(0)", "node.get_children()", "node.get_meta('huge_metadata')",
        "node.get_signal_list()", "node.get_groups()", "node.get_child_count().size()",
        "node.get_path().size()", "node.find(1)", "{'a': 1}.keys().size()",
        "[node].find(node)", "tree.min(1)", "node.get_child_count().min(1)",
        "min(node, 1)", "Vector2(node.get_child_count(), 1)"
    }) {
        ASSERT_TRUE(didi::godot::ExpressionPolicy::validate(source).isErr());
    }
}

void test_expression_policy_treats_quoted_identifiers_as_data_and_tracks_escapes() {
    // Break caught: scanning raw source words rejects harmless strings or an escaped quote hides executable suffixes.
    for (const auto& source : {
        R"("FileAccess.open and node.set are inert")",
        R"('queue_free \' OS.execute is still inert')",
        R"gd({"danger": "node.call(\"free\")"})gd"
    }) {
        ASSERT_TRUE(didi::godot::ExpressionPolicy::validate(source).isOk());
    }

    for (const auto& source : {
        R"('safe' + OS.execute('cmd', []))",
        R"('safe\' + OS.execute('cmd', []))",
        R"('unterminated)",
        R"('bad\q')"
    }) {
        ASSERT_TRUE(didi::godot::ExpressionPolicy::validate(source).isErr());
    }
}

void test_expression_policy_rejects_malformed_or_oversized_source_and_obfuscation() {
    // Break caught: malformed bytes, statement punctuation, or split dangerous identifiers bypass validation.
    const std::string invalid_utf8 = std::string("node") + static_cast<char>(0xC0) + static_cast<char>(0xAF);
    const std::string embedded_nul("node\0.get_child_count()", 23);
    const std::string maximum_source = "'" + std::string(2046, 'a') + "'";
    const std::string oversized_source = "'" + std::string(2047, 'a') + "'";

    ASSERT_TRUE(didi::godot::ExpressionPolicy::validate("").isErr());
    ASSERT_TRUE(didi::godot::ExpressionPolicy::validate(invalid_utf8).isErr());
    ASSERT_TRUE(didi::godot::ExpressionPolicy::validate(embedded_nul).isErr());
    ASSERT_TRUE(didi::godot::ExpressionPolicy::validate(maximum_source).isOk());
    ASSERT_TRUE(didi::godot::ExpressionPolicy::validate(oversized_source).isErr());

    for (const auto& source : {
        "O\\u0053.execute('cmd', [])", "O/**/S.execute('cmd', [])",
        "FileAccess . open ('res://x', 1)", "node['queue_free']()",
        "node . callv (['queue_free'])", "node.set_deferred('x', 1)",
        "node.rpc('method')", "node.get_meta('callable').call()",
        "node.get_child_count(); node.queue_free()", "node.get_child_count() # hidden",
        "node.get_child_count()\nnode.queue_free()"
    }) {
        ASSERT_TRUE(didi::godot::ExpressionPolicy::validate(source).isErr());
    }
}

void test_expression_policy_accepts_read_only_containers_math_and_unicode_strings() {
    // Break caught: the scanner becomes ASCII-source-only or rejects non-call expression operators.
    for (const auto& source : {
        R"({"label": "caf\u00e9", "values": [1, 2, 3]})",
        "Vector2(1, 2)", "Vector3(1, 2, 3)", "Color(1, 0.5, 0.25, 1)",
        "1 + 2", "+1", "node.get_child_count() + 1", "[+1].size()", "Vector2(+1, 2)",
        "node.get_child_count() >= 0", "node.get_path()", "node.get_class()",
        "node.is_class('Node')", "node.is_in_group('group')", "node.has_method('get_path')",
        "node.has_meta('key')", "[1, 2, 3].find(2) == 1", "[1, 2, 3].size()",
        "{'a': 1}.has('a')", "'abc'.size()", "'x'.repeat(3)"
    }) {
        ASSERT_TRUE(didi::godot::ExpressionPolicy::validate(source).isOk());
    }
}

void test_expression_policy_allows_math_on_source_local_vectors_and_colors() {
    // Break caught: the sandbox advertised Vector2, Vector3 and Color
    // constructors but forbade every dot after them, so reading a component or
    // calling a pure geometry method on a value the expression itself built was
    // rejected as an object property read.
    for (const auto& source : {
        "Vector2(10, 20).x", "Vector2(10, 20).y",
        "Vector3(1, 2, 3).x", "Vector3(1, 2, 3).z",
        "Color(1, 0.5, 0.25, 1).r", "Color(1, 0.5, 0.25, 1).a",
        "Vector2(3, 4).length()", "Vector2(3, 4).length_squared()",
        "Vector3(1, 0, 0).normalized()", "Vector2(-1, 2).abs()",
        "Vector2(0, 0).distance_to(Vector2(3, 4))",
        "Vector3(1, 0, 0).dot(Vector3(0, 1, 0))",
        "Vector2(1, 0).angle_to(Vector2(0, 1))",
        "Vector2(3, 4).length() > 4", "Vector2(10, 20).x + Vector2(1, 2).y"
    }) {
        ASSERT_TRUE(didi::godot::ExpressionPolicy::validate(source).isOk());
    }

    // Break caught: the relaxation leaks into real object property reads, into
    // components that do not exist, or lets a dynamic value into an argument.
    for (const auto& source : {
        "node.position.x", "node.get_path().x", "node.get_child(0).position.y",
        "Vector2(1, 2).z", "Vector2(1, 2).w", "Vector3(1, 2, 3).a", "Color(1, 1, 1, 1).x",
        "Vector2(1, 2).dot(node)", "Vector2(1, 2).distance_to(node.get_path())",
        "Vector2(node.get_child_count(), 1).x",
        "Vector2(1, 2).queue_free()", "Vector2(1, 2).set(0, 5)",
        "Vector2(1, 2).length(3)", "Vector2(1, 2).dot()",
        "'abc'.x", "[1, 2].x", "{'a': 1}.x"
    }) {
        ASSERT_TRUE(didi::godot::ExpressionPolicy::validate(source).isErr());
    }
}

struct RegisterExpressionSandboxTests {
    RegisterExpressionSandboxTests() {
        registerTest("ExpressionSandbox.SourceLocalVectorMath",
                     test_expression_policy_allows_math_on_source_local_vectors_and_colors);
        registerTest("ExpressionSandbox.DocumentedVocabulary",
                     test_expression_policy_accepts_only_the_documented_read_only_vocabulary);
        registerTest("ExpressionSandbox.StringAndEscapeScanning",
                     test_expression_policy_treats_quoted_identifiers_as_data_and_tracks_escapes);
        registerTest("ExpressionSandbox.MalformedAndObfuscatedSource",
                     test_expression_policy_rejects_malformed_or_oversized_source_and_obfuscation);
        registerTest("ExpressionSandbox.ReadOnlyContainersAndMath",
                     test_expression_policy_accepts_read_only_containers_math_and_unicode_strings);
    }
} g_registerExpressionSandboxTests;

} // namespace
