// The change journal (Q15 in docs/BUILD_QUEUE.md).
//
// What is asserted here is the server's half: what an entry holds, what never
// reaches the file, that the file stays bounded and whole when two writers
// share it, and that a call records itself through the registry without its
// answer changing shape. Whether the editor agrees about what can be undone is
// the live harness's to assert, on every engine line.

#include "didi/common/ipc_channel.hpp"
#include "didi/mcp/change_journal.hpp"
#include "didi/mcp/tool_registry.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#define ASSERT_TRUE(cond) if (!(cond)) throw std::runtime_error("Assertion failed: " #cond);
#define ASSERT_FALSE(cond) ASSERT_TRUE(!(cond))
#define ASSERT_EQ(a, b) ASSERT_TRUE((a) == (b))

void registerTest(const std::string& name, std::function<void()> fn);

namespace {

using didi::json;
namespace journal = didi::mcp::journal;

class ScopedProject final {
public:
    explicit ScopedProject(const std::string& suffix)
        : m_original(std::filesystem::current_path()),
          m_root(m_original / "build" / "test-projects" /
                 ("didi-journal-" + suffix + "-" +
                  std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))) {
        std::filesystem::create_directories(m_root);
        std::filesystem::current_path(m_root);
        std::ofstream("project.godot") << "config_version=5" << std::endl;
    }
    ~ScopedProject() {
        std::error_code error;
        std::filesystem::current_path(m_original, error);
        std::filesystem::remove_all(m_root, error);
    }
    const std::filesystem::path& root() const { return m_root; }

private:
    std::filesystem::path m_original;
    std::filesystem::path m_root;
};

json undoStep(uint64_t serial, int64_t index, const std::string& action) {
    return {{"run", "run-1"},  {"serial", serial},      {"history_id", 2},
            {"index", index},  {"version", index + 2}, {"action", action},
            {"scene_path", "res://main.tscn"}};
}

json storedEntry(const std::filesystem::path& root, const std::string& tool) {
    journal::Call call;
    call.tool = tool;
    call.arguments = {{"target_node", "Player"}};
    call.answer = {{"value", 1}};
    auto stored = journal::append(root, journal::entryFor(call));
    if (stored.isErr()) throw std::runtime_error(stored.error().message);
    return stored.value();
}

void test_secrets_are_redacted_by_key_and_by_the_name_beside_them() {
    const json value = {
        {"keystore/release_password", "hunter2"},
        {"API_Key", "abc"},
        {"nested", {{"auth_token", "t"}, {"list", json::array({{{"client_secret", "s"}}, 3})}}},
        {"setting", "services/payment_api_key"},
        {"value", "sk-live"},
        {"old_value", "sk-old"},
        {"position", {{"x", 1}}},
        {"name", "Player"}};
    const auto out = journal::redactSecrets(value);
    ASSERT_EQ(out["keystore/release_password"], "[redacted]");
    ASSERT_EQ(out["API_Key"], "[redacted]");
    ASSERT_EQ(out["nested"]["auth_token"], "[redacted]");
    ASSERT_EQ(out["nested"]["list"][0]["client_secret"], "[redacted]");
    ASSERT_EQ(out["nested"]["list"][1], 3);
    // The setting's name says what its value is.
    ASSERT_EQ(out["value"], "[redacted]");
    ASSERT_EQ(out["old_value"], "[redacted]");
    // Nothing else is touched, the name that governs included.
    ASSERT_EQ(out["setting"], "services/payment_api_key");
    ASSERT_EQ(out["position"]["x"], 1);
    ASSERT_EQ(out["name"], "Player");
    // A plain setting keeps its value.
    const auto plain = journal::redactSecrets({{"setting", "application/config/name"}, {"value", "Game"}});
    ASSERT_EQ(plain["value"], "Game");
    ASSERT_FALSE(journal::isSecretKey("keystore/release"));
    ASSERT_TRUE(journal::isSecretKey("Passphrase"));
}

void test_a_large_value_becomes_a_preview_cut_on_a_character() {
    ASSERT_EQ(journal::boundedValue(json("short"), 64), json("short"));
    std::string text(40, 'a');
    text += "\xC3\xA9\xC3\xA9\xC3\xA9";  // three two-byte characters
    text += std::string(100, 'b');
    const auto bounded = journal::boundedValue(json(text), 43);
    ASSERT_EQ(bounded["truncated"], true);
    ASSERT_TRUE(bounded["bytes"].get<size_t>() > 43);
    const auto preview = bounded["preview"].get<std::string>();
    ASSERT_TRUE(preview.size() <= 43);
    // Never half a character: the last byte is not a lead byte left alone.
    ASSERT_TRUE((static_cast<unsigned char>(preview.back()) & 0xC0) != 0xC0);
}

void test_an_entry_holds_the_target_the_values_the_files_and_the_undo_step() {
    journal::Call call;
    call.tool = "scene_set_property";
    call.arguments = {{"target_node", "Player"},
                      {"property_name", "position"},
                      {"value", {{"x", 3}, {"y", 4}}},
                      {"confirmation_token", "secret-ish"},
                      {"dry_run", false},
                      {"_meta", {{"didi", {}}}}};
    call.answer = {{"status", "success"},
                   {"execution_mode", "live"},
                   {"old_value", {{"x", 0}, {"y", 0}}},
                   {"value", {{"x", 3}, {"y", 4}}},
                   {"applied", true},
                   {"scene_path", "res://main.tscn"},
                   {"scene_saved", false},
                   {"follow_up", json::array()}};
    call.undo_steps = json::array({undoStep(1, 0, "Didi: set position")});
    call.execution_mode = "live";
    call.session_id = "abc";
    const auto entry = journal::entryFor(call);
    ASSERT_EQ(entry["tool"], "scene_set_property");
    ASSERT_EQ(entry["outcome"], "applied");
    ASSERT_EQ(entry["session"], "abc");
    ASSERT_EQ(entry["target"]["target_node"], "Player");
    ASSERT_EQ(entry["target"]["property_name"], "position");
    ASSERT_EQ(entry["before"]["x"], 0);
    ASSERT_EQ(entry["after"]["value"]["x"], 3);
    // The envelope describes the call, which the entry already records.
    ASSERT_FALSE(entry["after"].contains("execution_mode"));
    ASSERT_FALSE(entry["after"].contains("follow_up"));
    // Steering arguments are not the change.
    ASSERT_FALSE(entry["arguments"].contains("confirmation_token"));
    ASSERT_FALSE(entry["arguments"].contains("dry_run"));
    ASSERT_FALSE(entry["arguments"].contains("_meta"));
    // An unsaved scene is where the change is, not a file it wrote.
    ASSERT_TRUE(entry["files"].empty());
    ASSERT_EQ(entry["scene"], "res://main.tscn");
    ASSERT_EQ(entry["undo"].size(), 1u);
    ASSERT_EQ(entry["undo"][0]["action"], "Didi: set position");
    ASSERT_FALSE(entry.contains("undo_note"));
}

void test_files_come_from_what_the_answer_says_it_wrote() {
    journal::Call call;
    call.tool = "project_rename_symbol";
    call.answer = {{"written_to", "res://project.godot"},
                   {"updated_files", json::array({{{"path", "res://a.gd"}}, {{"path", "res://b.gd"}}})},
                   {"written", json::array({"res://a.gd", "res://c.gd"})}};
    const auto entry = journal::entryFor(call);
    ASSERT_EQ(entry["files"], json::array({"res://project.godot", "res://a.gd", "res://b.gd", "res://c.gd"}));
    ASSERT_TRUE(entry["undo"].is_null());
    ASSERT_TRUE(entry["undo_note"].get<std::string>().find("without an editor") != std::string::npos);

    journal::Call saved;
    saved.tool = "editor_save_scene";
    saved.execution_mode = "live";
    saved.answer = {{"scene_path", "res://main.tscn"}, {"file_bytes", 120}};
    const auto save = journal::entryFor(saved);
    ASSERT_EQ(save["files"], json::array({"res://main.tscn"}));
    ASSERT_TRUE(save["undo_note"].get<std::string>().find("no step") != std::string::npos);
}

void test_a_failed_call_records_its_error_and_an_undo_by_entry_names_it() {
    journal::Call failed;
    failed.tool = "scene_remove_node";
    failed.succeeded = false;
    failed.answer = {{"code", 500}, {"message", "postcondition failed"}, {"data", {{"code", "x_failed"}}}};
    failed.undo_steps = json::array({undoStep(3, 1, "Didi: remove node")});
    const auto entry = journal::entryFor(failed);
    ASSERT_EQ(entry["outcome"], "failed");
    ASSERT_EQ(entry["error"]["code"], 500);
    ASSERT_EQ(entry["error"]["reason"], "x_failed");
    ASSERT_FALSE(entry.contains("after"));
    ASSERT_EQ(entry["undo"].size(), 1u);

    journal::Call undo;
    undo.tool = "editor_undo";
    undo.arguments = {{"journal_entry", 4}};
    undo.answer = {{"status", "success"}, {"action", "undo"}};
    const auto undoing = journal::entryFor(undo);
    ASSERT_EQ(undoing["undoes"], 4);
    ASSERT_TRUE(undoing["undo_note"].get<std::string>().find("reverses") != std::string::npos);
}

void test_an_entry_stays_under_its_size_bound() {
    journal::Call call;
    call.tool = "resource_create";
    json big = json::object();
    for (int i = 0; i < 40; ++i) big["key" + std::to_string(i)] = std::string(400, 'x');
    call.arguments = big;
    call.answer = big;
    const auto entry = journal::entryFor(call);
    ASSERT_TRUE(entry.dump().size() <= journal::kMaxEntryBytes + 64);
}

void test_entries_are_numbered_bounded_and_mark_what_undid_them() {
    ScopedProject project("bounded");
    const auto first = storedEntry(project.root(), "scene_set_property");
    ASSERT_EQ(first["id"], 1);
    ASSERT_TRUE(first["at"].get<std::string>().size() == 24);  // 2026-10-03T10:00:00.000Z

    journal::Call undo;
    undo.tool = "editor_undo";
    undo.arguments = {{"journal_entry", 1}};
    ASSERT_TRUE(journal::append(project.root(), journal::entryFor(undo)).isOk());
    auto marked = journal::findEntry(project.root(), 1);
    ASSERT_TRUE(marked.isOk());
    ASSERT_EQ(marked.value()["undone_by"], 2);

    for (size_t i = 0; i < journal::kMaxEntries; ++i) storedEntry(project.root(), "scene_set_property");
    auto doc = journal::load(project.root());
    ASSERT_TRUE(doc.isOk());
    ASSERT_EQ(doc.value()["entries"].size(), journal::kMaxEntries);
    ASSERT_EQ(doc.value()["dropped"], 2);
    ASSERT_EQ(doc.value()["entries"].front()["id"], 3);
    ASSERT_EQ(doc.value()["next_id"], static_cast<int64_t>(journal::kMaxEntries) + 3);

    const auto dropped = journal::findEntry(project.root(), 1);
    ASSERT_TRUE(dropped.isErr());
    ASSERT_EQ(dropped.error().code, 404);
    ASSERT_TRUE(dropped.error().message.find("dropped") != std::string::npos);
    const auto never = journal::findEntry(project.root(), 9999);
    ASSERT_TRUE(never.isErr());
    ASSERT_EQ(never.error().data["code"], "journal_entry_not_found");
}

void test_a_damaged_journal_is_moved_aside_and_a_newer_one_left_alone() {
    ScopedProject project("damaged");
    const auto file = journal::journalPath(project.root());
    std::filesystem::create_directories(file.parent_path());
    std::ofstream(file) << "{not json";
    ASSERT_TRUE(journal::load(project.root()).isErr());
    const auto stored = storedEntry(project.root(), "scene_set_property");
    ASSERT_EQ(stored["id"], 1);
    auto doc = journal::load(project.root());
    ASSERT_TRUE(doc.isOk());
    const auto moved = doc.value()["recovered"]["moved_to"].get<std::string>();
    ASSERT_TRUE(std::filesystem::exists(file.parent_path() / moved));

    std::ofstream(file, std::ios::trunc) << R"({"format": 99, "next_id": 5, "entries": []})";
    const auto refused = journal::append(project.root(), journal::entryFor(journal::Call{}));
    ASSERT_TRUE(refused.isErr());
    ASSERT_EQ(refused.error().data["code"], "journal_format_newer");
    std::ifstream in(file);
    const std::string kept((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    ASSERT_TRUE(kept.find("\"format\": 99") != std::string::npos);
}

void test_two_writers_lose_no_entry() {
    ScopedProject project("writers");
    std::vector<std::thread> writers;
    std::atomic<int> failures{0};
    std::string first_failure;
    std::mutex failure_mutex;
    for (int w = 0; w < 2; ++w) {
        writers.emplace_back([&] {
            for (int i = 0; i < 30; ++i) {
                journal::Call call;
                call.tool = "scene_set_property";
                auto stored = journal::append(project.root(), journal::entryFor(call));
                if (stored.isErr()) {
                    std::lock_guard<std::mutex> lock(failure_mutex);
                    if (failures++ == 0) first_failure = stored.error().message;
                }
            }
        });
    }
    for (auto& writer : writers) writer.join();
    if (failures.load() != 0) throw std::runtime_error("append failed: " + first_failure);
    auto doc = journal::load(project.root());
    ASSERT_TRUE(doc.isOk());
    std::set<int64_t> ids;
    for (const auto& entry : doc.value()["entries"]) ids.insert(entry["id"].get<int64_t>());
    ASSERT_EQ(ids.size(), 60u);
    ASSERT_EQ(*ids.rbegin(), 60);
}

void test_the_view_is_newest_first_and_says_why_nothing_was_judged() {
    ScopedProject project("view");
    storedEntry(project.root(), "project_set_setting");
    journal::Call live;
    live.tool = "scene_set_property";
    live.execution_mode = "live";
    live.undo_steps = json::array({undoStep(1, 0, "Didi: set position")});
    ASSERT_TRUE(journal::append(project.root(), journal::entryFor(live)).isOk());

    const auto view = journal::view(project.root(), nullptr, 1);
    ASSERT_EQ(view["total"], 2);
    ASSERT_EQ(view["truncated"], true);
    ASSERT_EQ(view["entries"].size(), 1u);
    ASSERT_EQ(view["entries"][0]["id"], 2);
    ASSERT_EQ(view["entries"][0]["undo_state"]["state"], "unknown");
    ASSERT_EQ(view["editor"]["judged"], false);

    const auto both = journal::view(project.root(), nullptr, 10, true);
    ASSERT_EQ(both["truncated"], false);
    ASSERT_EQ(both["entries"][1]["undo"], "none");
    ASSERT_FALSE(both["entries"][1].contains("after"));
    ASSERT_EQ(both["entries"][0]["undo"], "unknown");

    // A damaged journal is reported in the view, not thrown.
    std::ofstream(journal::journalPath(project.root()), std::ios::trunc) << "[]";
    const auto damaged = journal::view(project.root(), nullptr);
    ASSERT_EQ(damaged["status"], "unreadable");
    ASSERT_TRUE(damaged["entries"].empty());
}

// An editor that answers every request with what it was given to answer.
class ScriptedEditor final : public didi::ipc::IIpcClient {
public:
    bool connect(const std::string&, int) override { return true; }
    void disconnect() override {}
    bool isConnected() const override { return true; }
    didi::Result<json> sendRequest(const std::string& method, const json& params, int) override {
        requests.push_back({{"method", method}, {"params", params}});
        if (answers.empty()) return didi::Error(500, "unscripted request " + method);
        auto next = answers.front();
        answers.erase(answers.begin());
        return next;
    }
    std::vector<didi::Result<json>> answers;
    std::vector<json> requests;
};

void test_a_call_journals_itself_and_its_answer_keeps_its_shape() {
    ScopedProject project("registry");
    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();
    auto editor = std::make_shared<ScriptedEditor>();
    editor->answers.push_back(json{{"status", "success"},
                                   {"action", "redo"},
                                   {"history", "scene"},
                                   {"undo_steps", json::array({undoStep(1, 0, "Didi: set position")})}});
    registry.setIpcClient(editor);
    const auto result = registry.callTool("editor_redo", json::object());
    registry.setIpcClient(nullptr);
    ASSERT_FALSE(result.isError);
    // Taken off the answer before any caller sees it.
    ASSERT_TRUE(result.content[0].text.find("undo_steps") == std::string::npos);
    ASSERT_TRUE(result.structuredContent.has_value());
    ASSERT_FALSE(result.structuredContent->contains("undo_steps"));
    ASSERT_FALSE(result.structuredContent->contains("journal"));

    auto doc = journal::load(project.root());
    ASSERT_TRUE(doc.isOk());
    ASSERT_EQ(doc.value()["entries"].size(), 1u);
    const auto& entry = doc.value()["entries"][0];
    ASSERT_EQ(entry["tool"], "editor_redo");
    ASSERT_EQ(entry["undo"][0]["action"], "Didi: set position");

    // A read is not journalled.
    editor->answers.push_back(json{{"status", "success"}, {"value", 1}});
    registry.setIpcClient(editor);
    (void)registry.callTool("scene_get_property", {{"target_node", "."}, {"property_name", "name"}});
    registry.setIpcClient(nullptr);
    ASSERT_EQ(journal::load(project.root()).value()["entries"].size(), 1u);
}

void test_undo_by_entry_sends_its_reference_and_refuses_what_has_none() {
    ScopedProject project("undo-entry");
    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();
    journal::Call live;
    live.tool = "scene_set_property";
    live.execution_mode = "live";
    live.undo_steps = json::array({undoStep(5, 0, "Didi: set position")});
    ASSERT_TRUE(journal::append(project.root(), journal::entryFor(live)).isOk());
    storedEntry(project.root(), "project_set_setting");  // entry 2, no undo step

    auto editor = std::make_shared<ScriptedEditor>();
    editor->answers.push_back(json{{"status", "success"}, {"action", "undo"}, {"history", "scene"}});
    registry.setIpcClient(editor);
    const auto undone = registry.callTool("editor_undo", {{"journal_entry", 1}});
    ASSERT_FALSE(undone.isError);
    ASSERT_EQ(editor->requests.size(), 1u);
    ASSERT_EQ(editor->requests[0]["method"], "editor.undo");
    ASSERT_EQ(editor->requests[0]["params"]["expect"]["serial"], 5);
    // Named to the bridge, whose refusal is the answer a refused undo gets.
    ASSERT_EQ(editor->requests[0]["params"]["journal_entry"], 1);
    ASSERT_EQ((*undone.structuredContent)["journal_entry"], 1);

    const auto nothing = registry.callTool("editor_undo", {{"journal_entry", 2}});
    ASSERT_TRUE(nothing.isError);
    ASSERT_TRUE(nothing.content[0].text.find("journal_entry_not_undoable") != std::string::npos);
    const auto missing = registry.callTool("editor_undo", {{"journal_entry", 77}});
    ASSERT_TRUE(missing.isError);
    ASSERT_TRUE(missing.content[0].text.find("journal_entry_not_found") != std::string::npos);
    // Two actions in one entry are refused before the editor is asked.
    journal::Call twice;
    twice.tool = "scene_set_property";
    twice.execution_mode = "live";
    twice.undo_steps = json::array({undoStep(6, 1, "first"), undoStep(7, 2, "second")});
    const auto multi = journal::append(project.root(), journal::entryFor(twice));
    ASSERT_TRUE(multi.isOk());
    const auto refused = registry.callTool("editor_undo", {{"journal_entry", multi.value()["id"]}});
    ASSERT_TRUE(refused.isError);
    ASSERT_TRUE(refused.content[0].text.find("journal_entry_multi_step") != std::string::npos);
    registry.setIpcClient(nullptr);
    ASSERT_EQ(editor->requests.size(), 1u);

    auto doc = journal::load(project.root());
    ASSERT_TRUE(doc.isOk());
    // The undo is an entry of its own, and the entry it undid says so.
    ASSERT_EQ(doc.value()["entries"].size(), 4u);
    ASSERT_EQ(doc.value()["entries"][0]["undone_by"], 3);
    ASSERT_EQ(doc.value()["entries"][2]["undoes"], 1);
}

struct RegisterChangeJournalTests {
    RegisterChangeJournalTests() {
        registerTest("ChangeJournal.SecretsRedactedByKeyAndName",
                     test_secrets_are_redacted_by_key_and_by_the_name_beside_them);
        registerTest("ChangeJournal.LargeValueBecomesPreview",
                     test_a_large_value_becomes_a_preview_cut_on_a_character);
        registerTest("ChangeJournal.EntryHoldsTargetValuesFilesUndo",
                     test_an_entry_holds_the_target_the_values_the_files_and_the_undo_step);
        registerTest("ChangeJournal.FilesFromWhatTheAnswerWrote",
                     test_files_come_from_what_the_answer_says_it_wrote);
        registerTest("ChangeJournal.FailedCallAndUndoByEntry",
                     test_a_failed_call_records_its_error_and_an_undo_by_entry_names_it);
        registerTest("ChangeJournal.EntryStaysUnderSizeBound", test_an_entry_stays_under_its_size_bound);
        registerTest("ChangeJournal.NumberedBoundedMarksUndone",
                     test_entries_are_numbered_bounded_and_mark_what_undid_them);
        registerTest("ChangeJournal.DamagedMovedAsideNewerLeftAlone",
                     test_a_damaged_journal_is_moved_aside_and_a_newer_one_left_alone);
        registerTest("ChangeJournal.TwoWritersLoseNoEntry", test_two_writers_lose_no_entry);
        registerTest("ChangeJournal.ViewNewestFirstSaysWhyUnjudged",
                     test_the_view_is_newest_first_and_says_why_nothing_was_judged);
        registerTest("ChangeJournal.CallJournalsItselfAnswerKeepsShape",
                     test_a_call_journals_itself_and_its_answer_keeps_its_shape);
        registerTest("ChangeJournal.UndoByEntrySendsReference",
                     test_undo_by_entry_sends_its_reference_and_refuses_what_has_none);
    }
} g_register_change_journal_tests;

}  // namespace
