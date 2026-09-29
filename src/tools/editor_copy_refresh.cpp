#include "didi/tools/editor_copy_refresh.hpp"

#include <algorithm>

namespace didi::mcp {

namespace {

// The bridge's own bound on one request.
constexpr size_t kPathsPerRequest = 256;
constexpr int kRefreshTimeoutMs = 10000;

bool flag(const json& result, const char* key) {
    const auto found = result.find(key);
    return found != result.end() && found->is_boolean() && found->get<bool>();
}

void readResult(EditorCopyRefresh& refresh, const json& result) {
    if (!result.is_object() || !result.contains("path") || !result["path"].is_string()) return;
    const auto path = result["path"].get<std::string>();
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

} // namespace

EditorCopyRefresh refreshEditorCopies(const std::shared_ptr<ipc::IIpcClient>& ipc,
                                      const std::vector<std::string>& res_paths) {
    EditorCopyRefresh refresh;
    if (!ipc || res_paths.empty() || !ipc->isConnected()) return refresh;
    for (size_t start = 0; start < res_paths.size(); start += kPathsPerRequest) {
        const size_t end = std::min(res_paths.size(), start + kPathsPerRequest);
        // One path goes as `path`, the request a bridge older than `paths`
        // already answers.
        const bool batch = end - start > 1;
        const json params =
            batch ? json{{"paths", std::vector<std::string>(res_paths.begin() + start,
                                                            res_paths.begin() + end)}}
                  : json{{"path", res_paths[start]}};
        auto answer = ipc->sendRequest("resource.refreshCached", params, kRefreshTimeoutMs);
        const bool readable = answer.isOk() && answer.value().is_object() &&
                              (!batch || (answer.value().contains("results") &&
                                          answer.value()["results"].is_array()));
        if (!readable) {
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
            for (const auto& result : answer.value()["results"]) readResult(refresh, result);
        } else {
            readResult(refresh, answer.value());
        }
    }
    return refresh;
}

void reportEditorCopy(json& payload, const EditorCopyRefresh& refresh) {
    if (!refresh.answered) return;
    payload["editor_copy_reloaded"] = !refresh.reloaded.empty();
    if (!refresh.failed.empty()) payload["editor_copy_error"] = refresh.failed.front()["reason"];
}

void reportEditorCopies(json& payload, const EditorCopyRefresh& refresh) {
    if (!refresh.answered) return;
    payload["editor_copies_reloaded"] = refresh.reloaded;
    if (!refresh.failed.empty()) payload["editor_copy_errors"] = refresh.failed;
}

} // namespace didi::mcp
