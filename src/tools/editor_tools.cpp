#include "didi/mcp/mcp_protocol.hpp"
#include "didi/mcp/change_journal.hpp"
#include "didi/common/cancellation.hpp"
#include "didi/common/ipc_channel.hpp"
#include "didi/common/logger.hpp"
#include "didi/offline/resource_indexer.hpp"
#include "didi/runtime/session_client.hpp"

#include <chrono>
#include <filesystem>
#include <optional>
#include <thread>

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

namespace {

// editor_reload_project as a job (#1157). The editor applies a scan of many new
// scripts a frame at a time, and on a slow editor that runs past the twelve
// seconds a single call can wait, so every call answered 504. As a job the
// bridge answers at once with an id and the job reads editor.reloadStatus,
// which is answered off the editor's main thread, until the scan is applied.
// The same shape as asset_reimport's job.
constexpr int64_t kReloadJobMs = 300000;
constexpr auto kReloadPollInterval = std::chrono::milliseconds(250);

Result<json> reloadAsJob(const std::shared_ptr<ipc::IIpcClient>& ipc) {
    // The call's own route, held for the whole job: a session selected
    // meanwhile is an editor that never heard of this reload.
    const auto lease = runtime::acquireRuntimeRouteLease(ipc);
    if (!lease.has_value()) {
        return Error::notConnected("Godot Editor is offline. Launch Godot to reload the project.");
    }
    auto accepted = ipc->sendRequest("editor.reloadProject", {{"detach_timeout_ms", kReloadJobMs}},
                                     ipc::kWaitForDefinitiveResponse);
    if (accepted.isErr()) return accepted.error();
    const auto& started = accepted.value();
    if (!started.is_object() || !started.contains("reload_id") || !started["reload_id"].is_string()) {
        // Answered in full, by a bridge that ran it the old way.
        return started;
    }
    const auto id = started["reload_id"].get<std::string>();
    const auto named = [&id](Result<json> answer) -> Result<json> {
        if (answer.isOk()) {
            if (answer.value().is_object()) answer.value()["reload_id"] = id;
            return answer;
        }
        auto error = answer.error();
        if (error.data.is_null()) error.data = json::object();
        if (error.data.is_object()) error.data["reload_id"] = id;
        return error;
    };
    const auto finishedAnswer = [](const json& answer) -> Result<json> {
        if (answer.is_object() && answer.contains("error") && answer["error"].is_object()) {
            const auto& error = answer["error"];
            return Error(error.value("code", 500),
                         error.value("message", std::string("The bridge reported a failure")),
                         error.value("data", json()));
        }
        return answer;
    };
    // The bridge answers on its own deadline. This one is for a bridge that
    // stopped answering the reads altogether.
    const auto give_up = std::chrono::steady_clock::now() + std::chrono::milliseconds(kReloadJobMs) +
                         std::chrono::seconds(60);
    std::optional<Error> last_failure;
    while (!cancellationRequested()) {
        std::this_thread::sleep_for(kReloadPollInterval);
        auto read = runtime::sendLiveRouteRequest(*lease, "editor.reloadStatus", {{"reload_id", id}},
                                                  ipc::kWaitForDefinitiveResponse, true);
        if (read.response.isErr()) {
            // A read lost on the way says nothing about the scan, so the next
            // one asks again. A refusal from the bridge itself is the answer.
            if (!ipc::transportFailureState(read.response.error()).has_value()) {
                return named(read.response.error());
            }
            last_failure = read.response.error();
        } else if (read.response.value().value("state", std::string()) == "finished") {
            return named(finishedAnswer(read.response.value().value("answer", json::object())));
        }
        if (std::chrono::steady_clock::now() >= give_up) {
            return Error(504,
                         "The editor stopped answering reads of reload " + id +
                             " before its scan was applied, so whether it was is unknown." +
                             (last_failure.has_value() ? " The last read failed: " + last_failure->message
                                                       : std::string()),
                         {{"code", "reload_status_unanswered"},
                          {"outcome", "unknown_outcome"},
                          {"reload_id", id},
                          {"retryable", true}});
        }
    }
    // The scan is the editor's and carries on. Only the wait for it ends.
    return Error(409, "The reload job was cancelled before its scan was applied.",
                 {{"code", "job_cancelled"}, {"reload_id", id}, {"retryable", false}});
}

} // namespace

CallToolResult handleEditorReloadProject(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    if (ipc && ipc->isConnected() && runningAsJob()) {
        auto answered = reloadAsJob(ipc);
        offline::ResourceIndexer::invalidateSharedIndex();
        if (answered.isErr()) {
            return CallToolResult::fromError(answered.error(), "Editor reload project failed: ");
        }
        return CallToolResult::successJson(answered.value());
    }
    if (ipc && ipc->isConnected()) {
        auto res = ipc->sendRequest("editor.reloadProject", args, ::didi::ipc::kWaitForDefinitiveResponse);
        // As the job and the offline paths do: a reload is asked for after
        // files changed behind the server, so its own index is stale (#1250).
        offline::ResourceIndexer::invalidateSharedIndex();
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
