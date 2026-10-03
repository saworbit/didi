// The change journal's undo references (Q15 in docs/BUILD_QUEUE.md).
//
// What is asserted here is everything that can be decided from a history's
// numbers and names: which actions a command committed, and whether one of
// them can still be undone on its own. The engine facts these rules stand on
// -- a version that falls on undo and repeats after the next commit, an Undo
// that takes the newer of two histories, a cleared history that keeps counting
// -- were measured on 4.5.1, 4.6.2 and 4.7.2, and the live harness asserts the
// editor agrees on every engine line.

#include "didi/gdextension/undo_ledger.hpp"

#include <functional>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#define ASSERT_TRUE(cond) if (!(cond)) throw std::runtime_error("Assertion failed: " #cond);
#define ASSERT_FALSE(cond) ASSERT_TRUE(!(cond))
#define ASSERT_EQ(a, b) ASSERT_TRUE((a) == (b))

void registerTest(const std::string& name, std::function<void()> fn);

namespace {

using didi::json;
using namespace didi::godot;

// A history as a list of action names with a current index, kept the way
// UndoRedo keeps it: a commit drops the redo entries and raises the version,
// an undo lowers it, a clear raises it and empties the list. Histories start at
// version 1, as the editor's do.
struct FakeHistory {
    int64_t id = 7;
    std::string scene = "res://main.tscn";
    std::vector<std::string> actions;
    int64_t current = -1;
    int64_t version = 1;

    void commit(const std::string& name) {
        actions.resize(static_cast<size_t>(current + 1));
        actions.push_back(name);
        current = static_cast<int64_t>(actions.size()) - 1;
        ++version;
    }
    void undo() {
        --current;
        --version;
    }
    void redo() {
        ++current;
        ++version;
    }
    void clear() {
        actions.clear();
        current = -1;
        ++version;
    }
    UndoHistoryState state() const {
        UndoHistoryState out;
        out.id = id;
        out.version = version;
        out.current = current;
        out.count = static_cast<int64_t>(actions.size());
        out.action = current >= 0 ? actions[static_cast<size_t>(current)] : std::string();
        out.scene_path = id == kGlobalUndoHistory ? std::string() : scene;
        return out;
    }
    std::function<std::string(int64_t)> names() const {
        return [this](int64_t index) {
            return index >= 0 && index < static_cast<int64_t>(actions.size())
                       ? actions[static_cast<size_t>(index)]
                       : std::string();
        };
    }
};

FakeHistory globalHistory() {
    FakeHistory global;
    global.id = kGlobalUndoHistory;
    global.scene.clear();
    return global;
}

// Commits through the ledger the way the editor hook does: read before, act,
// read after.
std::vector<UndoCommit> committed(UndoLedger& ledger, FakeHistory& history,
                                  const std::vector<std::string>& names) {
    const auto before = history.state();
    for (const auto& name : names) history.commit(name);
    return ledger.observe(before, history.state(), history.names());
}

UndoVerdict judge(const UndoLedger& ledger, const UndoCommit& ref, const FakeHistory& own,
                  bool candidate, const FakeHistory* other) {
    return ledger.evaluate(ref, own.state(), candidate,
                           other ? std::optional<UndoHistoryState>(other->state()) : std::nullopt,
                           own.names());
}

void test_a_command_that_commits_once_is_one_step_with_its_place_in_the_history() {
    UndoLedger ledger("run-a");
    FakeHistory scene;
    const auto steps = committed(ledger, scene, {"Didi: set position"});
    ASSERT_EQ(steps.size(), 1u);
    ASSERT_EQ(steps[0].run, std::string("run-a"));
    ASSERT_EQ(steps[0].serial, 1u);
    ASSERT_EQ(steps[0].history_id, 7);
    ASSERT_EQ(steps[0].index, 0);
    ASSERT_EQ(steps[0].version, 2);
    ASSERT_EQ(steps[0].action, std::string("Didi: set position"));
    ASSERT_EQ(steps[0].scene_path, std::string("res://main.tscn"));
}

void test_a_command_that_commits_twice_names_both_in_order() {
    UndoLedger ledger("run-a");
    FakeHistory scene;
    scene.commit("before");
    const auto steps = committed(ledger, scene, {"first", "second"});
    ASSERT_EQ(steps.size(), 2u);
    ASSERT_EQ(steps[0].action, std::string("first"));
    ASSERT_EQ(steps[0].index, 1);
    ASSERT_EQ(steps[0].version, 3);
    ASSERT_EQ(steps[1].action, std::string("second"));
    ASSERT_EQ(steps[1].index, 2);
    ASSERT_EQ(steps[1].version, 4);
    ASSERT_TRUE(steps[0].serial < steps[1].serial);
}

void test_reads_undos_and_clears_commit_nothing() {
    UndoLedger ledger("run-a");
    FakeHistory scene;
    scene.commit("a");
    ASSERT_TRUE(ledger.observe(scene.state(), scene.state(), scene.names()).empty());
    auto before = scene.state();
    scene.undo();
    ASSERT_TRUE(ledger.observe(before, scene.state(), scene.names()).empty());
    // A clear raises the version, as a commit does, and leaves nothing to
    // point at.
    before = scene.state();
    scene.clear();
    ASSERT_TRUE(scene.version > before.version);
    ASSERT_TRUE(ledger.observe(before, scene.state(), scene.names()).empty());
    // Two different histories are never compared.
    auto other = globalHistory();
    other.commit("g");
    ASSERT_TRUE(ledger.observe(scene.state(), other.state(), other.names()).empty());
}

void test_the_newest_action_of_a_history_with_nothing_else_to_weigh_is_available() {
    UndoLedger ledger("run-a");
    FakeHistory scene;
    const auto ref = committed(ledger, scene, {"Didi: set position"}).front();
    const auto verdict = judge(ledger, ref, scene, true, nullptr);
    ASSERT_TRUE(verdict.state == UndoState::available);
    // An empty global history is nothing to weigh either.
    const auto global = globalHistory();
    ASSERT_TRUE(judge(ledger, ref, scene, true, &global).state == UndoState::available);
}

void test_an_older_didi_action_in_the_other_history_does_not_block() {
    UndoLedger ledger("run-a");
    auto global = globalHistory();
    FakeHistory scene;
    committed(ledger, global, {"Didi: set shader uniform"});
    const auto ref = committed(ledger, scene, {"Didi: set position"}).front();
    ASSERT_TRUE(judge(ledger, ref, scene, true, &global).state == UndoState::available);
}

void test_a_newer_action_in_the_other_history_blocks_and_is_named() {
    UndoLedger ledger("run-a");
    auto global = globalHistory();
    FakeHistory scene;
    const auto ref = committed(ledger, scene, {"Didi: set position"}).front();
    committed(ledger, global, {"Didi: set shader uniform"});
    const auto newer = judge(ledger, ref, scene, true, &global);
    ASSERT_TRUE(newer.state == UndoState::blocked);
    ASSERT_EQ(newer.newer_history, std::string("global"));
    ASSERT_EQ(newer.newer_action, std::string("Didi: set shader uniform"));
    ASSERT_FALSE(newer.needs_scene_tab);

    // An action Didi did not make cannot be dated, so it blocks too: the
    // editor's Undo might take it.
    UndoLedger second("run-b");
    auto foreign = globalHistory();
    foreign.commit("Change Project Setting");
    FakeHistory other_scene;
    const auto ref2 = committed(second, other_scene, {"Didi: set position"}).front();
    const auto unknown = judge(second, ref2, other_scene, true, &foreign);
    ASSERT_TRUE(unknown.state == UndoState::blocked);
    ASSERT_TRUE(unknown.reason.find("not made by Didi") != std::string::npos);
}

void test_a_scene_that_is_not_the_current_tab_blocks_with_a_scene_switch() {
    UndoLedger ledger("run-a");
    FakeHistory scene;
    const auto ref = committed(ledger, scene, {"Didi: set position"}).front();
    const auto verdict = judge(ledger, ref, scene, false, nullptr);
    ASSERT_TRUE(verdict.state == UndoState::blocked);
    ASSERT_TRUE(verdict.needs_scene_tab);
    ASSERT_EQ(verdict.toJson().value("needs_scene_tab", false), true);
}

void test_later_actions_in_the_same_history_are_counted() {
    UndoLedger ledger("run-a");
    FakeHistory scene;
    const auto ref = committed(ledger, scene, {"Didi: set position"}).front();
    scene.commit("Move Node");  // a person's edit
    committed(ledger, scene, {"Didi: set rotation"});
    const auto verdict = judge(ledger, ref, scene, true, nullptr);
    ASSERT_TRUE(verdict.state == UndoState::later_history);
    ASSERT_EQ(verdict.later_actions, 2);
    ASSERT_EQ(verdict.toJson()["later_actions"].get<int64_t>(), 2);
}

void test_an_undone_action_is_undone_until_something_replaces_it() {
    UndoLedger ledger("run-a");
    FakeHistory scene;
    const auto ref = committed(ledger, scene, {"Didi: set position"}).front();
    scene.undo();
    ASSERT_TRUE(judge(ledger, ref, scene, true, nullptr).state == UndoState::undone);
    // Redone, it is the same action again.
    scene.redo();
    ASSERT_TRUE(judge(ledger, ref, scene, true, nullptr).state == UndoState::available);
}

void test_an_action_replaced_after_an_undo_is_gone_even_with_the_same_name() {
    // Undo and a new commit put the new action at the same index and version.
    UndoLedger ledger("run-a");
    FakeHistory scene;
    const auto ref = committed(ledger, scene, {"Didi: set position"}).front();
    scene.undo();
    const auto again = committed(ledger, scene, {"Didi: set position"}).front();
    ASSERT_EQ(again.index, ref.index);
    ASSERT_EQ(again.version, ref.version);
    ASSERT_TRUE(judge(ledger, ref, scene, true, nullptr).state == UndoState::gone);
    ASSERT_TRUE(judge(ledger, again, scene, true, nullptr).state == UndoState::available);

    // Replaced by a person's edit, the name tells them apart.
    UndoLedger second("run-b");
    FakeHistory other;
    const auto ref2 = committed(second, other, {"Didi: set position"}).front();
    other.undo();
    other.commit("Move Node");
    ASSERT_TRUE(judge(second, ref2, other, true, nullptr).state == UndoState::gone);
}

void test_a_cleared_history_or_an_earlier_run_is_gone() {
    UndoLedger ledger("run-a");
    FakeHistory scene;
    const auto ref = committed(ledger, scene, {"Didi: set position"}).front();
    FakeHistory cleared = scene;
    cleared.clear();
    ASSERT_TRUE(judge(ledger, ref, cleared, true, nullptr).state == UndoState::gone);
    // Cleared and refilled to the same length, the version gives it away.
    cleared.commit("Didi: set position");
    ASSERT_TRUE(judge(ledger, ref, cleared, true, nullptr).state == UndoState::gone);

    UndoLedger restarted("run-b");
    const auto verdict = judge(restarted, ref, scene, true, nullptr);
    ASSERT_TRUE(verdict.state == UndoState::gone);
    ASSERT_TRUE(verdict.reason.find("earlier editor run") != std::string::npos);
}

void test_a_commit_the_ledger_no_longer_remembers_cannot_be_dated() {
    UndoLedger ledger("run-a", 2);
    auto global = globalHistory();
    FakeHistory scene;
    committed(ledger, global, {"Didi: set shader uniform"});
    const auto ref = committed(ledger, scene, {"Didi: set position"}).front();
    committed(ledger, scene, {"x"});
    scene.undo();
    // The global action has been forgotten, so it might be the newer one.
    ASSERT_FALSE(ledger.topOf(global.state()).has_value());
    ASSERT_TRUE(judge(ledger, ref, scene, true, &global).state == UndoState::blocked);
}

void test_a_reference_round_trips_and_a_damaged_one_names_its_field() {
    UndoCommit commit;
    commit.run = "abc";
    commit.serial = 12;
    commit.history_id = 3;
    commit.scene_path = "res://level.tscn";
    commit.index = 4;
    commit.version = 9;
    commit.action = "Didi: remove node";
    const auto back = UndoCommit::fromJson(commit.toJson());
    ASSERT_TRUE(back.has_value());
    ASSERT_EQ(back->toJson(), commit.toJson());

    auto global = commit.toJson();
    global.erase("scene_path");
    global["history_id"] = 0;
    ASSERT_TRUE(UndoCommit::fromJson(global).has_value());

    for (const char* field : {"run", "serial", "history_id", "index", "version", "action"}) {
        auto damaged = commit.toJson();
        damaged.erase(field);
        std::string problem;
        ASSERT_FALSE(UndoCommit::fromJson(damaged, &problem).has_value());
        ASSERT_EQ(problem, std::string(field));
    }
    auto wrong = commit.toJson();
    wrong["version"] = "9";
    std::string problem;
    ASSERT_FALSE(UndoCommit::fromJson(wrong, &problem).has_value());
    ASSERT_EQ(problem, std::string("version"));
    auto negative = commit.toJson();
    negative["serial"] = -1;
    ASSERT_FALSE(UndoCommit::fromJson(negative).has_value());
}

struct RegisterUndoLedgerTests {
    RegisterUndoLedgerTests() {
        registerTest("UndoLedger.OneCommitIsOneStep",
                     test_a_command_that_commits_once_is_one_step_with_its_place_in_the_history);
        registerTest("UndoLedger.TwoCommitsNamedInOrder",
                     test_a_command_that_commits_twice_names_both_in_order);
        registerTest("UndoLedger.ReadsUndosAndClearsCommitNothing",
                     test_reads_undos_and_clears_commit_nothing);
        registerTest("UndoLedger.NewestActionIsAvailable",
                     test_the_newest_action_of_a_history_with_nothing_else_to_weigh_is_available);
        registerTest("UndoLedger.OlderOtherHistoryDoesNotBlock",
                     test_an_older_didi_action_in_the_other_history_does_not_block);
        registerTest("UndoLedger.NewerOtherHistoryBlocks",
                     test_a_newer_action_in_the_other_history_blocks_and_is_named);
        registerTest("UndoLedger.SceneNotCurrentTabBlocks",
                     test_a_scene_that_is_not_the_current_tab_blocks_with_a_scene_switch);
        registerTest("UndoLedger.LaterActionsCounted", test_later_actions_in_the_same_history_are_counted);
        registerTest("UndoLedger.UndoneUntilReplaced",
                     test_an_undone_action_is_undone_until_something_replaces_it);
        registerTest("UndoLedger.ReplacedAfterUndoIsGone",
                     test_an_action_replaced_after_an_undo_is_gone_even_with_the_same_name);
        registerTest("UndoLedger.ClearedOrEarlierRunIsGone", test_a_cleared_history_or_an_earlier_run_is_gone);
        registerTest("UndoLedger.ForgottenCommitCannotBeDated",
                     test_a_commit_the_ledger_no_longer_remembers_cannot_be_dated);
        registerTest("UndoLedger.ReferenceRoundTrips",
                     test_a_reference_round_trips_and_a_damaged_one_names_its_field);
    }
} g_register_undo_ledger_tests;

}  // namespace
