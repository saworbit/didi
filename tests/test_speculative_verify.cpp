#include "didi/mcp/tool_registry.hpp"
#include "didi/offline/speculative_verify.hpp"

#include <filesystem>
#include <fstream>
#include <functional>
#include <stdexcept>
#include <string>

#define ASSERT_TRUE(cond) \
    if (!(cond)) throw std::runtime_error("Assertion failed: " #cond);
#define ASSERT_EQ(a, b) \
    if (!((a) == (b))) throw std::runtime_error("Assertion failed: " #a " == " #b);

void registerTest(const std::string& name, std::function<void()> fn);

namespace {

using didi::json;
using didi::offline::parseSpeculativeVerifyRequest;

json change(const std::string& path, const std::string& content) {
    return {{"path", path}, {"content", content}};
}

void test_speculative_request_describes_a_whole_proposal() {
    auto request = parseSpeculativeVerifyRequest(
        {{"changes", json::array({change("res://player.gd", "extends Node\n"),
                                  change("res://enemy.gd", "extends Node\n")})}});
    ASSERT_TRUE(request.isOk());
    ASSERT_EQ(request.value().changes.size(), 2u);
    ASSERT_EQ(request.value().timeout_seconds, 120);
    // The res:// prefix is not part of a path on disk, and the sandbox writes
    // to disk.
    ASSERT_EQ(request.value().changes[0].relative, std::string("player.gd"));
    ASSERT_EQ(request.value().changes[0].path, std::string("res://player.gd"));

    auto timed = parseSpeculativeVerifyRequest(
        {{"changes", json::array({change("res://a.gd", "")})}, {"timeout_seconds", 30}});
    ASSERT_TRUE(timed.isOk());
    ASSERT_EQ(timed.value().timeout_seconds, 30);

    // A file with no res:// prefix is the same file.
    auto bare = parseSpeculativeVerifyRequest(
        {{"changes", json::array({change("scripts/a.gd", "")})}});
    ASSERT_TRUE(bare.isOk());
    ASSERT_EQ(bare.value().changes[0].relative, std::string("scripts/a.gd"));
}

void test_speculative_request_refuses_what_it_could_not_honestly_check() {
    const auto rejected = [](const json& params) {
        return parseSpeculativeVerifyRequest(params).isErr();
    };
    ASSERT_TRUE(rejected(json::object()));
    ASSERT_TRUE(rejected({{"changes", json::array()}}));
    ASSERT_TRUE(rejected({{"changes", "not an array"}}));
    ASSERT_TRUE(rejected({{"changes", json::array({change("res://a.gd", "")})}, {"unknown", 1}}));
    ASSERT_TRUE(rejected({{"changes", json::array({{{"path", "res://a.gd"}}})}}));
    ASSERT_TRUE(rejected({{"changes", json::array({{{"content", "x"}}})}}));
    ASSERT_TRUE(rejected({{"changes", json::array({{{"path", "res://a.gd"}, {"content", "x"},
                                                    {"mode", "overwrite"}}})}}));
    // Writing outside the project is the one thing a sandbox must never be
    // asked to do, because the path is resolved before anything is isolated.
    ASSERT_TRUE(rejected({{"changes", json::array({change("res://../escape.gd", "")})}}));
    ASSERT_TRUE(rejected({{"changes", json::array({change("../escape.gd", "")})}}));
    // One file takes one content. Two entries for the same path would leave the
    // result depending on which was written last.
    ASSERT_TRUE(rejected({{"changes", json::array({change("res://a.gd", "one"),
                                                   change("res://a.gd", "two")})}}));
    ASSERT_TRUE(rejected({{"changes", json::array({change("res://a.gd", "")})},
                          {"timeout_seconds", 0}}));
    ASSERT_TRUE(rejected({{"changes", json::array({change("res://a.gd", "")})},
                          {"timeout_seconds", 601}}));
    ASSERT_TRUE(rejected({{"changes", json::array({change("res://a.gd", "")})},
                          {"timeout_seconds", "soon"}}));
    // A file larger than the cap is refused rather than truncated, because a
    // truncated script would be checked and the verdict would be about
    // something the caller never proposed.
    ASSERT_TRUE(rejected({{"changes", json::array({change(
        "res://a.gd", std::string(didi::offline::kMaxSpeculativeContentBytes + 1, 'x'))})}}));

    // The index of the bad entry is named.
    auto refused = parseSpeculativeVerifyRequest(
        {{"changes", json::array({change("res://a.gd", "ok"), change("res://../out.gd", "")})}});
    ASSERT_TRUE(refused.isErr());
    ASSERT_TRUE(refused.error().message.find("changes[1]") != std::string::npos);
}

void test_speculative_request_takes_a_scene_to_run() {
    auto with_scene = parseSpeculativeVerifyRequest(
        {{"changes", json::array({change("res://a.gd", "")})},
         {"run_scene", "res://levels/main.tscn"},
         {"run_frames", 30}});
    ASSERT_TRUE(with_scene.isOk());
    ASSERT_EQ(with_scene.value().run_scene_relative, std::string("levels/main.tscn"));
    ASSERT_EQ(with_scene.value().run_frames, 30);

    auto without = parseSpeculativeVerifyRequest(
        {{"changes", json::array({change("res://a.gd", "")})}});
    ASSERT_TRUE(without.isOk());
    ASSERT_TRUE(without.value().run_scene_relative.empty());
    // A default that only applies when a run was asked for.
    ASSERT_EQ(without.value().run_frames, 120);

    const auto rejected = [](const json& params) {
        return parseSpeculativeVerifyRequest(params).isErr();
    };
    const auto proposal = json::array({change("res://a.gd", "")});
    // Only a scene can be run. A script handed to run_scene would start Godot
    // and produce a failure about the wrong thing.
    ASSERT_TRUE(rejected({{"changes", proposal}, {"run_scene", "res://a.gd"}}));
    ASSERT_TRUE(rejected({{"changes", proposal}, {"run_scene", "res://../out.tscn"}}));
    ASSERT_TRUE(rejected({{"changes", proposal}, {"run_scene", ""}}));
    ASSERT_TRUE(rejected({{"changes", proposal}, {"run_scene", 7}}));
    ASSERT_TRUE(rejected({{"changes", proposal}, {"run_scene", "res://m.tscn"}, {"run_frames", 0}}));
    ASSERT_TRUE(rejected({{"changes", proposal}, {"run_scene", "res://m.tscn"}, {"run_frames", 6001}}));
    // run_frames without run_scene is a caller who thinks they asked for a run
    // and did not. Accepting it silently would let them believe the parse-only
    // result was a run.
    ASSERT_TRUE(rejected({{"changes", proposal}, {"run_frames", 30}}));
}

void test_apply_tool_is_registered_as_a_confirmed_mutation() {
    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();
    const auto* tool = registry.getTool("project_apply_changes");
    ASSERT_TRUE(tool != nullptr);
    ASSERT_TRUE(tool->capability.implemented);
    const auto description = tool->toJson();
    // It writes several files at once with no undo stack behind them, so it
    // gets both halves of the mutation contract rather than just a dry run.
    ASSERT_TRUE(description["inputSchema"]["properties"].contains("dry_run"));
    ASSERT_TRUE(description["inputSchema"]["properties"].contains("confirmation_token"));
    ASSERT_TRUE(!description["annotations"]["readOnlyHint"].get<bool>());

    // The check half is still a read, and must not have acquired a dry run by
    // sharing a request shape with the write half.
    const auto* verify = registry.getTool("project_verify_changes");
    ASSERT_TRUE(verify != nullptr);
    ASSERT_TRUE(!verify->toJson()["inputSchema"]["properties"].contains("dry_run"));
}

void test_speculative_tool_is_registered_as_an_offline_read() {
    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();
    const auto* tool = registry.getTool("project_verify_changes");
    ASSERT_TRUE(tool != nullptr);
    ASSERT_TRUE(tool->capability.implemented);
    const auto description = tool->toJson();
    // Nothing it does reaches the project, so there is no mutation to preview
    // and no token to confirm.
    ASSERT_TRUE(!description["inputSchema"]["properties"].contains("dry_run"));
    ASSERT_TRUE(description["description"].get<std::string>().rfind("UNIMPLEMENTED:", 0) != 0);
}

struct RegisterSpeculativeVerify {
    RegisterSpeculativeVerify() {
        registerTest("SpeculativeVerify.RequestDescribesAProposal",
                     test_speculative_request_describes_a_whole_proposal);
        registerTest("SpeculativeVerify.RequestRefusesUncheckable",
                     test_speculative_request_refuses_what_it_could_not_honestly_check);
        registerTest("SpeculativeVerify.RegisteredOffline",
                     test_speculative_tool_is_registered_as_an_offline_read);
        registerTest("SpeculativeVerify.RequestTakesASceneToRun",
                     test_speculative_request_takes_a_scene_to_run);
        registerTest("SpeculativeVerify.ApplyIsAConfirmedMutation",
                     test_apply_tool_is_registered_as_a_confirmed_mutation);
    }
} g_registerSpeculativeVerify;

} // namespace
