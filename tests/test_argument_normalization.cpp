#include "didi/mcp/argument_normalization.hpp"
#include "didi/mcp/tool_registry.hpp"
#include <functional>
#include <limits>
#include <stdexcept>

void registerTest(const std::string&, std::function<void()>);
#define CHECK(x) if (!(x)) throw std::runtime_error("Assertion failed: " #x)

namespace {
using didi::json;
auto normalize(const std::string& name, const json& args) {
    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();
    return didi::mcp::normalizeToolArguments(name, registry.getTool(name)->inputSchema, args);
}
void numeric_limits() {
    auto good = normalize("scene_get_hierarchy", {{"max_depth", "12"}, {"max_nodes", "100"}});
    CHECK(good.isOk());
    CHECK(good.value()["max_depth"] == 12);
    CHECK(good.value()["max_nodes"] == 100);
    for (const auto& value : json::array({"12junk", " 12", "12 ", "1.5", "1e1", "", "9223372036854775808", "-9223372036854775809", 1.5, true, 65, -1})) {
        CHECK(normalize("scene_get_hierarchy", {{"max_depth", value}}).isErr());
    }
}
void vector_components() {
    auto good = normalize("ui_hit_test", {{"point", json::array({"1.25", -2})}});
    CHECK(good.isOk());
    CHECK(good.value()["point"] == json({{"x", 1.25}, {"y", -2.0}}));
    CHECK(normalize("ui_hit_test", {{"point", {{"x", "1e2"}, {"y", 0}}}}).isOk());
    for (const auto& value : json::array({"nan", "inf", "1e999", "2junk", "9007199254740993", "-9007199254740993", "9007199254740993e0", "-9007199254740993e0", true, nullptr, 9007199254740993ULL})) {
        auto bad = normalize("ui_hit_test", {{"point", json::array({0, value})}});
        CHECK(bad.isErr());
        CHECK(bad.error().data["field"] == "/arguments/point/1");
    }
    CHECK(normalize("ui_hit_test", {{"point", json::array({"9007199254740991", "-9007199254740991"})}}).isOk());
    CHECK(normalize("ui_hit_test", {{"point", json::array({9007199254740992ULL, -9007199254740992LL})}}).isOk());
    CHECK(normalize("ui_hit_test", {{"point", json::array({1})}}).isErr());
    CHECK(normalize("ui_hit_test", {{"point", json::array({1, 2, 3})}}).isErr());
    CHECK(normalize("ui_hit_test", {{"point", 1}}).isErr());
    CHECK(normalize("ui_hit_test", {{"point", {{"x", 1}, {"y", 2}, {"z", 3}}}}).isErr());
}
void atomic_and_strict() {
    json args = {{"point", json::array({"1", "bad-secret"})}};
    const auto original = args;
    const auto bad = normalize("ui_hit_test", args);
    CHECK(bad.isErr());
    CHECK(args == original);
    CHECK(bad.error().data.dump().find("bad-secret") == std::string::npos);
    CHECK(bad.error().data.dump().size() < 2048);
    CHECK(normalize("ui_list_controls", {{"visible_only", "true"}}).isErr());
    CHECK(normalize("ui_list_controls", {{"typo", 1}}).isErr());
    CHECK(normalize("scene_get_hierarchy", {{"root_path", 12}}).isErr());
    CHECK(didi::mcp::normalizeToolArguments("scene_set_property", json::object(), json::object()).isErr());
    CHECK(didi::mcp::normalizeToolArguments("typo", json::object(), json::object()).isErr());
}
void budgets() {
    CHECK(normalize("ui_list_controls", {{"root_path", std::string(70000, 'x')}}).isErr());
    json wide = json::array();
    for (int i = 0; i < 4100; ++i) wide.push_back(0);
    CHECK(normalize("ui_list_controls", {{"unknown", wide}}).error().data["reason"] == "input_budget_exceeded");
    json deep = 0;
    for (int i = 0; i < 12; ++i) deep = json::array({deep});
    CHECK(normalize("ui_list_controls", {{"unknown", deep}}).error().data["reason"] == "input_budget_exceeded");
    json cumulative = json::object();
    for (int i = 0; i < 30; ++i) cumulative[std::to_string(i)] = std::string(500, 'x');
    CHECK(normalize("ui_list_controls", cumulative).error().data["reason"] == "input_budget_exceeded");
    CHECK(normalize("ui_hit_test", {{"point", json::array({0, std::numeric_limits<double>::infinity()})}}).isErr());
}
struct Registration {
    Registration() {
        registerTest("ElasticIngress.NumericLimits", numeric_limits);
        registerTest("ElasticIngress.VectorComponents", vector_components);
        registerTest("ElasticIngress.AtomicAndStrict", atomic_and_strict);
        registerTest("ElasticIngress.Budgets", budgets);
    }
} registration;
}
