#include "didi/mcp/mcp_protocol.hpp"
#include "didi/mcp/change_journal.hpp"
#include "didi/common/ipc_channel.hpp"
#include "didi/common/logger.hpp"
#include "didi/offline/resource_indexer.hpp"

#include <filesystem>

namespace didi {
namespace mcp {

namespace {

// editor_undo with journal_entry (Q15): undoes the action one journal entry
// committed, and nothing else. The bridge judges it against the editor's
// histories before it runs the editor's own Undo, and refuses one that later
// history depends on, or that the editor's Undo would not reach, rather than
// undo something in its place. Its refusal is the answer, as every live
// refusal is, so the entry's id goes to the bridge to be named in it.
CallToolResult undoJournalEntry(const json& args, const std::shared_ptr<ipc::IIpcClient>& ipc) {
    const auto id = args["journal_entry"].get<int64_t>();
    std::error_code error;
    const auto root = std::filesystem::current_path(error);
    if (error) return CallToolResult::errorJson(500, "The project directory cannot be read.");
    auto entry = journal::findEntry(root, id);
    if (entry.isErr()) return CallToolResult::fromError(entry.error());
    const auto& found = entry.value();
    const auto undo = found.value("undo", json());
    if (!undo.is_array() || undo.empty()) {
        return CallToolResult::errorJson(
            409,
            "Journal entry " + std::to_string(id) + " (" + found.value("tool", std::string("?")) +
                ") has nothing to undo. " + found.value("undo_note", std::string()),
            {{"code", "journal_entry_not_undoable"}, {"journal_entry", id}});
    }
    // No tool commits more than one action in a call today. One that did
    // would need each step undone in turn and a refusal part way reported as
    // such, which is refused here until a tool needs it.
    if (undo.size() > 1) {
        return CallToolResult::errorJson(
            409,
            "Journal entry " + std::to_string(id) + " committed " + std::to_string(undo.size()) +
                " actions, and undo by entry undoes one.",
            {{"code", "journal_entry_multi_step"}, {"journal_entry", id}, {"steps", undo.size()}});
    }
    if (!ipc || !ipc->isConnected()) {
        return CallToolResult::notConnected(
            "Undoing a journal entry needs the editor whose history holds it. Attach that editor "
            "session first.");
    }
    auto res = ipc->sendRequest("editor.undo", {{"expect", undo.front()}, {"journal_entry", id}},
                                ::didi::ipc::kWaitForDefinitiveResponse);
    if (res.isErr()) return CallToolResult::fromError(res.error());
    json answer = res.value().is_object() ? res.value() : json::object();
    answer["journal_entry"] = id;
    return CallToolResult::successJson(answer);
}

}  // namespace

CallToolResult handleEditorUndo(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    if (args.is_object() && args.contains("journal_entry")) return undoJournalEntry(args, ipc);
    if (ipc && ipc->isConnected()) {
        auto res = ipc->sendRequest("editor.undo", args, ::didi::ipc::kWaitForDefinitiveResponse);
        if (res.isOk()) {
            return CallToolResult::successJson(res.value());
        }
        return CallToolResult::fromError(res.error(), "Editor undo failed: ");
    }
    return CallToolResult::notConnected("Godot Editor is offline. Launch Godot to execute Undo transactions.");
}

CallToolResult handleEditorRedo(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    if (ipc && ipc->isConnected()) {
        auto res = ipc->sendRequest("editor.redo", args, ::didi::ipc::kWaitForDefinitiveResponse);
        if (res.isOk()) {
            return CallToolResult::successJson(res.value());
        }
        return CallToolResult::fromError(res.error(), "Editor redo failed: ");
    }
    return CallToolResult::notConnected("Godot Editor is offline. Launch Godot to execute Redo transactions.");
}

CallToolResult handleEditorSaveScene(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    if (ipc && ipc->isConnected()) {
        auto res = ipc->sendRequest("editor.saveScene", args, ::didi::ipc::kWaitForDefinitiveResponse);
        if (res.isOk()) {
            return CallToolResult::successJson(res.value());
        }
        return CallToolResult::fromError(res.error(), "Editor save scene failed: ");
    }
    return CallToolResult::notConnected("Godot Editor is offline. Launch Godot to save active scene.");
}

CallToolResult handleEditorReloadProject(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    if (ipc && ipc->isConnected()) {
        auto res = ipc->sendRequest("editor.reloadProject", args, ::didi::ipc::kWaitForDefinitiveResponse);
        if (res.isOk()) {
            return CallToolResult::successJson(res.value());
        }
        return CallToolResult::fromError(res.error(), "Editor reload project failed: ");
    }
    // Offline this is the whole job. Callers reach for it after changing files
    // outside Didi, and it used to report a re-index while the cached index
    // carried on answering with what it read before.
    offline::ResourceIndexer::invalidateSharedIndex();
    return CallToolResult::successJson({
        {"status", "offline"},
        {"message", "Offline caches re-indexed."}
    });
}

} // namespace mcp
} // namespace didi
