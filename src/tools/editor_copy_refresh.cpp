#include "didi/tools/editor_copy_refresh.hpp"

#include <algorithm>
#include <chrono>
#include <set>
#include <thread>

namespace didi::mcp {

namespace {

// The bridge's own bound on one request.
constexpr size_t kPathsPerRequest = 256;
constexpr int kRefreshTimeoutMs = 10000;
// How long the tabs still pending are asked about again. The bridge rebuilds
// one tab a request, because on 4.5 and 4.6 a second reload in the same frame
// does nothing, and each request is served on a later frame. A software-drawn
// editor takes about 0.6 s a frame, so this is headroom for a dozen tabs.
constexpr auto kPendingTabsDeadline = std::chrono::seconds(10);
constexpr auto kPendingTabsPause = std::chrono::milliseconds(20);

const char* const kStaleTab =
    " It still holds the scene from before the write, and saving it would put that back over "
    "the file. Close it with scene_close and discard_unsaved: true to keep the file as written.";

bool flag(const json& result, const char* key) {
    const auto found = result.find(key);
    return found != result.end() && found->is_boolean() && found->get<bool>();
}

std::string text(const json& result, const char* key) {
    const auto found = result.find(key);
    return found != result.end() && found->is_string() ? found->get<std::string>() : std::string();
}

// The scene the editor had current before a rebuild moved it. On 4.5 and 4.6
// rebuilding a tab can leave that tab current, and only a later frame can
// switch back, so it is opened once the tabs are done. The first move names
// the scene the caller was on; a later rebuild moves away from the first.
struct EditedSceneRestore {
    bool needed{false};
    std::string scene;
    std::string rebuilt;
};

// The tab half of one path's answer. A pending tab is asked about again.
void readTab(EditorCopyRefresh& refresh, const std::string& path, const json& result,
             std::vector<std::string>& pending, EditedSceneRestore& restore) {
    if (!flag(result, "scene_open")) return;
    if (flag(result, "scene_reloaded")) {
        refresh.scenes_reloaded.push_back(path);
        const auto discarded = result.find("scene_discarded_unsaved");
        if (discarded != result.end() && (discarded->is_boolean() || discarded->is_null())) {
            refresh.scenes_discarded_unsaved[path] = *discarded;
        }
        const auto moved = result.find("edited_scene_moved_from");
        if (!restore.needed && moved != result.end() && moved->is_string()) {
            restore = {true, moved->get<std::string>(), path};
        }
    } else if (flag(result, "scene_reload_pending")) {
        pending.push_back(path);
    } else {
        const auto reason = text(result, "scene_reload_error");
        refresh.failed.push_back(
            {{"path", path},
             {"reason", reason.empty()
                            ? std::string("The editor did not reload the scene's tab.") + kStaleTab
                            : reason}});
    }
}

void readResult(EditorCopyRefresh& refresh, const json& result, std::vector<std::string>& pending,
                EditedSceneRestore& restore, bool tab_only) {
    if (!result.is_object() || !result.contains("path") || !result["path"].is_string()) return;
    const auto path = result["path"].get<std::string>();
    readTab(refresh, path, result, pending, restore);
    if (tab_only) return;
    if (flag(result, "reloaded")) {
        refresh.reloaded.push_back(path);
        return;
    }
    // Not held is nothing to report: the editor has no copy to be stale.
    if (!flag(result, "cached")) return;
    const auto reason = result.contains("reload_error") && result["reload_error"].is_string()
                            ? result["reload_error"].get<std::string>()
                            : std::string("The editor did not reload its copy.");
    refresh.failed.push_back({{"path", path}, {"reason", reason}});
}

json refreshParams(const std::vector<std::string>& paths, size_t start, size_t end,
                   bool discard_unsaved) {
    // One path goes as `path`, the request a bridge older than `paths`
    // already answers.
    json params = end - start > 1
                      ? json{{"paths", std::vector<std::string>(paths.begin() + start,
                                                                paths.begin() + end)}}
                      : json{{"path", paths[start]}};
    if (discard_unsaved) params["discard_unsaved"] = true;
    return params;
}

bool readableAnswer(const Result<json>& answer, bool batch) {
    return answer.isOk() && answer.value().is_object() &&
           (!batch || (answer.value().contains("results") && answer.value()["results"].is_array()));
}

std::string sceneList(const std::vector<std::string>& scenes) {
    std::string listed;
    for (const auto& scene : scenes) listed += (listed.empty() ? "" : ", ") + scene;
    return listed;
}

} // namespace

EditorCopyRefresh refreshEditorCopies(const std::shared_ptr<ipc::IIpcClient>& ipc,
                                      const std::vector<std::string>& res_paths,
                                      bool discard_unsaved) {
    EditorCopyRefresh refresh;
    if (!ipc || res_paths.empty() || !ipc->isConnected()) return refresh;
    std::vector<std::string> pending;
    EditedSceneRestore restore;
    for (size_t start = 0; start < res_paths.size(); start += kPathsPerRequest) {
        const size_t end = std::min(res_paths.size(), start + kPathsPerRequest);
        const bool batch = end - start > 1;
        auto answer = ipc->sendRequest("resource.refreshCached",
                                       refreshParams(res_paths, start, end, discard_unsaved),
                                       kRefreshTimeoutMs);
        if (!readableAnswer(answer, batch)) {
            // Nothing asked yet has been answered, so nothing is known and
            // nothing is said. Once an editor has answered, a later request
            // it did not answer leaves those files unknown, and they are
            // named rather than dropped.
            if (!refresh.answered) return refresh;
            const std::string reason =
                "The editor did not answer the reload request" +
                (answer.isErr() ? ": " + answer.error().message : std::string("."));
            for (size_t index = start; index < end; ++index) {
                refresh.failed.push_back({{"path", res_paths[index]}, {"reason", reason}});
            }
            continue;
        }
        refresh.answered = true;
        if (batch) {
            for (const auto& result : answer.value()["results"]) {
                readResult(refresh, result, pending, restore, false);
            }
        } else {
            readResult(refresh, answer.value(), pending, restore, false);
        }
    }

    // The tabs the bridge did not get to, asked about again. It rebuilds one
    // a request and answers the rest pending.
    const auto deadline = std::chrono::steady_clock::now() + kPendingTabsDeadline;
    while (!pending.empty() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(kPendingTabsPause);
        std::vector<std::string> asked;
        asked.swap(pending);
        const bool batch = asked.size() > 1;
        auto answer = ipc->sendRequest("resource.refreshCached",
                                       refreshParams(asked, 0, asked.size(), discard_unsaved),
                                       kRefreshTimeoutMs);
        if (!readableAnswer(answer, batch)) {
            const std::string reason =
                "The editor did not answer the request to reload the scene's tab" +
                (answer.isErr() ? ": " + answer.error().message : std::string(".")) + kStaleTab;
            for (const auto& path : asked) {
                refresh.failed.push_back({{"path", path}, {"reason", reason}});
            }
            break;
        }
        if (batch) {
            for (const auto& result : answer.value()["results"]) {
                readResult(refresh, result, pending, restore, true);
            }
        } else {
            readResult(refresh, answer.value(), pending, restore, true);
        }
    }
    for (const auto& path : pending) {
        refresh.failed.push_back(
            {{"path", path},
             {"reason", std::string("The editor was still switching scenes and did not reload "
                                    "the scene's tab.") +
                            kStaleTab}});
    }
    if (restore.needed) {
        // A scene never saved has no path to open it by.
        auto back = restore.scene.empty()
                        ? Result<json>(Error(409, "it was never saved, so it has no path to open"))
                        : ipc->sendRequest("scene.open", {{"scene_path", restore.scene}},
                                           kRefreshTimeoutMs);
        if (back.isErr()) {
            refresh.failed.push_back(
                {{"path", restore.rebuilt},
                 {"reason", "Rebuilding the tab left it the edited scene in place of " +
                                (restore.scene.empty() ? std::string("an unsaved scene")
                                                       : restore.scene) +
                                ", and switching back failed: " + back.error().message +
                                ". Open the scene you were editing with scene_open."}});
        }
    }
    return refresh;
}

std::optional<Error> refuseUnsavedOpenScenes(const std::shared_ptr<ipc::IIpcClient>& ipc,
                                             const std::vector<std::string>& res_paths,
                                             bool discard_unsaved) {
    if (discard_unsaved || !ipc || res_paths.empty() || !ipc->isConnected()) return std::nullopt;
    auto answer = ipc->sendRequest("editor.openScenes", json::object(), kRefreshTimeoutMs);
    // No answer is nothing known, the same as the refresh: a game session, or
    // a bridge older than the request. The refresh after the write still names
    // a tab it could not reload.
    if (answer.isErr() || !answer.value().is_object() || !answer.value().contains("open_scenes") ||
        !answer.value()["open_scenes"].is_array()) {
        return std::nullopt;
    }
    const auto& state = answer.value();
    std::set<std::string> open;
    for (const auto& scene : state["open_scenes"]) {
        if (scene.is_string()) open.insert(scene.get<std::string>());
    }
    std::vector<std::string> rewritten_open;
    for (const auto& path : res_paths) {
        if (open.count(path) &&
            std::find(rewritten_open.begin(), rewritten_open.end(), path) == rewritten_open.end()) {
            rewritten_open.push_back(path);
        }
    }
    if (rewritten_open.empty()) return std::nullopt;

    const bool readable = flag(state, "unsaved_scenes_readable") &&
                          state.contains("unsaved_scenes") && state["unsaved_scenes"].is_array();
    if (!readable) {
        return Error(409,
                     "Nothing was written. This call rewrites " + sceneList(rewritten_open) +
                         ", open in the editor, and Godot before 4.7 cannot say whether a tab has "
                         "unsaved changes. The tab is reloaded from the new file, which would "
                         "lose any. If it has changes to keep, open it with scene_open and save "
                         "it with editor_save_scene, then send discard_unsaved: true.",
                     {{"code", "dirty_state_unavailable"},
                      {"field", "discard_unsaved"},
                      {"retry_with", {{"discard_unsaved", true}}},
                      {"open_scenes", rewritten_open}});
    }
    std::vector<std::string> unsaved;
    for (const auto& scene : state["unsaved_scenes"]) {
        if (scene.is_string() && std::find(rewritten_open.begin(), rewritten_open.end(),
                                           scene.get<std::string>()) != rewritten_open.end()) {
            unsaved.push_back(scene.get<std::string>());
        }
    }
    if (unsaved.empty()) return std::nullopt;
    return Error(409,
                 "Nothing was written. This call rewrites " + sceneList(unsaved) +
                     ", open in the editor with unsaved changes. The tab is reloaded from the new "
                     "file, which would lose them, and a tab left as it is would put the old "
                     "scene back on its next save. Open it with scene_open and save it with "
                     "editor_save_scene, or send discard_unsaved: true to lose the changes.",
                 {{"code", "unsaved_changes"},
                  {"field", "discard_unsaved"},
                  {"retry_with", {{"discard_unsaved", true}}},
                  {"open_scenes", rewritten_open},
                  {"unsaved_scenes", unsaved}});
}

void reportEditorCopy(json& payload, const EditorCopyRefresh& refresh) {
    if (!refresh.answered) return;
    payload["editor_copy_reloaded"] = !refresh.reloaded.empty();
    if (!refresh.scenes_reloaded.empty()) {
        payload["editor_scene_reloaded"] = true;
        // Only the writers that rebuild a tab whatever it holds get here with
        // a tab that had changes, and their answer said nothing about losing
        // them (#1082).
        const auto discarded = refresh.scenes_discarded_unsaved.find(refresh.scenes_reloaded.front());
        if (discarded != refresh.scenes_discarded_unsaved.end()) {
            payload["editor_scene_discarded_unsaved"] = *discarded;
        }
    }
    if (!refresh.failed.empty()) payload["editor_copy_error"] = refresh.failed.front()["reason"];
}

void reportEditorCopies(json& payload, const EditorCopyRefresh& refresh) {
    if (!refresh.answered) return;
    payload["editor_copies_reloaded"] = refresh.reloaded;
    payload["editor_scenes_reloaded"] = refresh.scenes_reloaded;
    if (!refresh.failed.empty()) payload["editor_copy_errors"] = refresh.failed;
}

} // namespace didi::mcp
