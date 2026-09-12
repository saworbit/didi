#include "didi/mcp/tool_registry.hpp"
#include "didi/offline/speculative_verify.hpp"

#include <chrono>
#include <cstdlib>
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

    // A path the active Windows code page has no mapping for. The relative
    // form was built with narrow generic_string(), which throws
    // std::system_error for exactly these characters, so a proposal that
    // touched one failed with an internal error instead of being checked.
    const std::string outside_code_page = "\xE9\xA1\xB9\xE7\x9B\xAE/\xE6\x96\xB0\xE8\x84\x9A\xE6\x9C\xAC.gd";
    auto unmappable = parseSpeculativeVerifyRequest(
        {{"changes", json::array({change("res://" + outside_code_page, "extends Node\n")})}});
    ASSERT_TRUE(unmappable.isOk());
    ASSERT_EQ(unmappable.value().changes[0].relative, outside_code_page);
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


// A git repository built for the test, so which work tree encloses the project
// is a fact about the fixture rather than about the machine the test runs on.
class ScopedGitProject {
public:
    // `track_project` decides whether the repository knows anything about the
    // nested project, which is the whole distinction under test. Tracking it
    // means staging it, not committing it, so the run stops at the next check
    // instead of building a worktree and shelling out to Godot.
    ScopedGitProject(const std::string& suffix, bool track_project) {
        m_previous = std::filesystem::current_path();
        m_repository = std::filesystem::temp_directory_path() /
                       ("didi-verify-scope-" + suffix + "-" +
                        std::to_string(std::chrono::steady_clock::now()
                                           .time_since_epoch()
                                           .count()));
        m_project = m_repository / "nested" / "game";
        std::filesystem::create_directories(m_project);
        std::ofstream(m_repository / "unrelated.txt", std::ios::binary) << "not the project\n";
        std::ofstream(m_project / "project.godot", std::ios::binary) << "config_version=5\n";

        git("init");
        git("config user.email didi@example.invalid");
        git("config user.name Didi");
        // Deliberately no commit. The enclosing check runs before the
        // missing-commit check, so the refusal under test still fires, and the
        // tracked case stops at the next check instead of building a worktree
        // and shelling out to Godot.
        git("add unrelated.txt");
        if (track_project) git("add nested");
        std::filesystem::current_path(m_project);
    }

    ~ScopedGitProject() {
        std::error_code ignored;
        std::filesystem::current_path(m_previous, ignored);
        std::filesystem::remove_all(m_repository, ignored);
    }

    bool usable() const { return m_usable; }

private:
    void git(const std::string& arguments) {
        const std::string command = "git -C \"" +
                                    m_repository.string() + "\" " + arguments +
#if defined(_WIN32)
                                    " >NUL 2>NUL";
#else
                                    " >/dev/null 2>&1";
#endif
        if (std::system(command.c_str()) != 0) m_usable = false;
    }

    std::filesystem::path m_repository;
    std::filesystem::path m_project;
    std::filesystem::path m_previous;
    bool m_usable{true};
};

// Break caught: whichever work tree enclosed the project was adopted, however
// far above it sat, and was reported only as "the repository". A stray git init
// in a home directory made that directory the repository: the next step would
// have been a worktree of it plus a copy of its uncommitted state, reported as
// all_ok (#450).
void test_an_enclosing_repository_is_refused_and_named() {
    didi::offline::SpeculativeVerifyRequest request;
    didi::offline::SpeculativeChange change;
    change.path = "res://v1.gd";
    change.relative = "v1.gd";
    change.content = "extends Node\n";
    request.changes.push_back(change);

    {
        ScopedGitProject enclosing("encloses", false);
        if (enclosing.usable()) {
            const auto refused = didi::offline::verifyChangesInSandbox(request);
            ASSERT_TRUE(refused.isErr());
            ASSERT_EQ(refused.error().code, 409);
            // Named, in the sentence and in the data, so "the repository" is
            // identifiable.
            ASSERT_TRUE(refused.error().message.find("tracks nothing under") !=
                        std::string::npos);
            ASSERT_TRUE(refused.error().data.is_object());
            ASSERT_TRUE(refused.error().data.contains("repository_root"));
            ASSERT_TRUE(!refused.error()
                             .data["repository_root"]
                             .get<std::string>()
                             .empty());
        }
    }

    // The nested project case #450 calls intended behaviour is unchanged: a
    // repository that holds the project gets past this check and on to the
    // next one. Here that next one is the missing-commit refusal, which is
    // also where "the repository" used to be an unidentifiable phrase and now
    // names the tree.
    {
        ScopedGitProject holding("holds", true);
        if (holding.usable()) {
            const auto later = didi::offline::verifyChangesInSandbox(request);
            ASSERT_TRUE(later.isErr());
            ASSERT_TRUE(later.error().message.find("tracks nothing under") ==
                        std::string::npos);
            ASSERT_TRUE(later.error().message.find("has no commit") != std::string::npos);
            ASSERT_TRUE(later.error().data.is_object());
            ASSERT_TRUE(later.error().data.contains("repository_root"));
        }
    }
}

// Break caught: the dry run answered with an envelope and the confirmed call
// did not. project_apply_changes built one by hand and only when the error
// carried data, so the same condition reached through verify was wrapped and
// through apply was a bare string (#449).
void test_apply_and_verify_fail_in_the_same_shape() {
    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();

    const json arguments = {{"changes", json::array({change("res://v1.gd", "extends Node\n")})}};

    ScopedGitProject project("same-shape", false);
    if (!project.usable()) return;

    const auto verified = registry.callTool("project_verify_changes", arguments);
    auto applied_arguments = arguments;
    applied_arguments["confirmation_token"] = "not-a-real-token";
    const auto applied = registry.callTool("project_apply_changes", applied_arguments);

    ASSERT_TRUE(verified.isError);
    ASSERT_TRUE(applied.isError);
    for (const auto* result : {&verified, &applied}) {
        ASSERT_TRUE(!result->content.empty());
        const auto payload = json::parse(result->content[0].text, nullptr, false);
        // The whole complaint: one of these used to be a sentence and the other
        // JSON, so a client parsing error text got two different things.
        ASSERT_TRUE(!payload.is_discarded());
        ASSERT_TRUE(payload.contains("error"));
        ASSERT_TRUE(payload["error"].contains("code"));
        ASSERT_TRUE(payload["error"].contains("data"));
        ASSERT_TRUE(payload["error"]["data"].contains("retryable"));
    }
}

// Break caught: the preview had no target to read, so it bound the arguments to
// a token and said so honestly. What it never did was check the precondition its
// sibling checks before doing anything at all. project_verify_changes refuses a
// project no git work tree holds, with no mutation and no token; the preview
// issued a token for exactly that call, and spending it returned the same 409.
// A caller following the documented dry-run then confirm path spent two calls
// and a token to learn what the first could have said (#491).
void test_the_apply_preview_runs_the_check_its_sibling_runs() {
    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();

    const json arguments = {{"changes", json::array({change("res://v1.gd", "extends Node\n")})},
                            {"dry_run", true}};

    ScopedGitProject project("preview-precondition", false);
    if (!project.usable()) return;

    const auto refused = registry.callTool("project_apply_changes", arguments);
    ASSERT_TRUE(refused.isError);
    ASSERT_TRUE(!refused.content.empty());
    const auto payload = json::parse(refused.content[0].text, nullptr, false);
    ASSERT_TRUE(!payload.is_discarded());
    ASSERT_EQ(payload["error"]["code"], 409);
    ASSERT_TRUE(payload["error"]["message"].get<std::string>().find("tracks nothing under") !=
                std::string::npos);

    // The whole point: no token was minted for a call that cannot be applied.
    ASSERT_TRUE(refused.content[0].text.find("confirmation_token") == std::string::npos);

    // The sibling refuses the same thing for the same reason, which is what
    // makes the preview's silence a defect rather than a difference.
    const auto verified = registry.callTool("project_verify_changes",
                                            json{{"changes", arguments["changes"]}});
    ASSERT_TRUE(verified.isError);
    const auto verify_payload = json::parse(verified.content[0].text, nullptr, false);
    ASSERT_EQ(verify_payload["error"]["code"], 409);
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
        registerTest("SpeculativeVerify.EnclosingRepositoryRefusedAndNamed",
                     test_an_enclosing_repository_is_refused_and_named);
        registerTest("SpeculativeVerify.ApplyAndVerifyFailAlike",
                     test_apply_and_verify_fail_in_the_same_shape);
        registerTest("SpeculativeVerify.ApplyPreviewRunsTheRepositoryCheck",
                     test_the_apply_preview_runs_the_check_its_sibling_runs);
    }
} g_registerSpeculativeVerify;

} // namespace
