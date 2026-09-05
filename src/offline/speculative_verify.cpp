#include "didi/offline/speculative_verify.hpp"

#include "didi/common/logger.hpp"
#include "didi/common/project_path.hpp"
#include "didi/offline/test_runner.hpp"
#include "didi/offline/process_runner.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <random>
#include <fstream>
#include <sstream>

namespace didi::offline {

namespace {

namespace fs = std::filesystem;

constexpr size_t kMaxDetailBytes = 4096;

// Truncates on a character boundary rather than mid-sequence, so a bounded
// diagnostic is still valid UTF-8 and still renders.
std::string boundedText(const std::string& text, size_t maximum_bytes) {
    if (text.size() <= maximum_bytes) return text;
    size_t cut = maximum_bytes;
    while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0u) == 0x80u) --cut;
    return text.substr(0, cut) + "...";
}

bool endsWithGdscript(const std::string& path) {
    if (path.size() < 3) return false;
    std::string tail = path.substr(path.size() - 3);
    for (auto& character : tail) {
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    }
    return tail == ".gd";
}

Result<ProcessResult> runGit(const fs::path& working_directory,
                             const std::vector<std::string>& arguments,
                             int timeout_seconds) {
    ProcessRequest request;
    request.executable = "git";
    request.arguments = arguments;
    request.working_directory = working_directory;
    request.timeout = std::chrono::milliseconds(timeout_seconds * 1000);
    return runProcess(request);
}

std::string trimmed(const std::string& text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

std::string makeSandboxName() {
    static constexpr char digits[] = "0123456789abcdef";
    std::random_device entropy;
    std::string suffix(16, '0');
    for (size_t index = 0; index < 8; ++index) {
        const auto byte = static_cast<uint8_t>(entropy());
        suffix[index * 2] = digits[byte >> 4u];
        suffix[index * 2 + 1] = digits[byte & 0x0Fu];
    }
    return "didi-verify-" + suffix;
}

// Removes the worktree whatever happened, including on the paths that failed.
// A sandbox left behind is a directory the person never asked for and a git
// worktree entry that will confuse the next `git worktree list`.
class SandboxGuard {
public:
    SandboxGuard(fs::path repository, fs::path sandbox)
        : m_repository(std::move(repository)), m_sandbox(std::move(sandbox)) {}
    SandboxGuard(const SandboxGuard&) = delete;
    SandboxGuard& operator=(const SandboxGuard&) = delete;

    ~SandboxGuard() {
        if (!m_armed) return;
        auto removed = runGit(m_repository, {"worktree", "remove", "--force",
                                             paths::projectPathToUtf8(m_sandbox)}, 60);
        if (removed.isErr() || removed.value().exit_code != 0) {
            // Fall back to deleting the directory and letting git forget it, so
            // a failed remove does not leave the repository carrying a worktree
            // that is not there.
            std::error_code error;
            fs::remove_all(m_sandbox, error);
            (void)runGit(m_repository, {"worktree", "prune"}, 60);
            DIDI_LOG_WARN("SPECULATIVE",
                          "Verification sandbox needed a forced cleanup: ",
                          paths::projectPathToUtf8(m_sandbox));
        }
    }

    void disarm() { m_armed = false; }

private:
    fs::path m_repository;
    fs::path m_sandbox;
    bool m_armed{true};
};

} // namespace

json SpeculativeVerifyResult::toJson() const {
    json scripts_json = json::array();
    for (const auto& verdict : scripts) {
        json entry = {{"path", verdict.path}, {"ok", verdict.ok}};
        if (!verdict.detail.empty()) entry["detail"] = verdict.detail;
        scripts_json.push_back(std::move(entry));
    }
    return json{{"execution_mode", "offline"},
                {"base_commit", base_commit},
                {"carried_uncommitted", carried_uncommitted},
                {"untracked_excluded", untracked_excluded},
                {"written", written},
                {"scripts", std::move(scripts_json)},
                {"all_ok", all_ok},
                {"sandbox_removed", true}};
}

Result<SpeculativeVerifyRequest> parseSpeculativeVerifyRequest(const json& params) {
    if (!params.is_object()) {
        return Error::invalidArgument("Verification params must be an object");
    }
    for (auto it = params.begin(); it != params.end(); ++it) {
        if (it.key() != "changes" && it.key() != "timeout_seconds") {
            return Error::invalidArgument("Verification request contains an unknown property");
        }
    }
    if (!params.contains("changes") || !params["changes"].is_array()) {
        return Error::invalidArgument("changes must be an array");
    }
    const auto& entries = params["changes"];
    if (entries.empty() || entries.size() > kMaxSpeculativeChanges) {
        return Error::invalidArgument("changes must contain 1 to " +
                                      std::to_string(kMaxSpeculativeChanges) + " entries");
    }

    SpeculativeVerifyRequest request;
    if (params.contains("timeout_seconds")) {
        const auto& value = params["timeout_seconds"];
        if (!value.is_number_integer() && !value.is_number_unsigned()) {
            return Error::invalidArgument("timeout_seconds must be an integer");
        }
        const auto seconds = value.get<int64_t>();
        if (seconds < 1 || seconds > 600) {
            return Error::invalidArgument("timeout_seconds must be from 1 to 600");
        }
        request.timeout_seconds = static_cast<int>(seconds);
    }

    request.changes.reserve(entries.size());
    for (size_t index = 0; index < entries.size(); ++index) {
        const std::string where = "changes[" + std::to_string(index) + "]";
        const auto& entry = entries[index];
        if (!entry.is_object()) return Error::invalidArgument(where + " must be an object");
        for (auto it = entry.begin(); it != entry.end(); ++it) {
            if (it.key() != "path" && it.key() != "content") {
                return Error::invalidArgument(where + " contains an unknown property");
            }
        }
        if (!entry.contains("path") || !entry["path"].is_string()) {
            return Error::invalidArgument(where + ".path is required and must be a string");
        }
        if (!entry.contains("content") || !entry["content"].is_string()) {
            return Error::invalidArgument(where + ".content is required and must be a string");
        }
        SpeculativeChange change;
        change.path = entry["path"].get<std::string>();
        change.content = entry["content"].get<std::string>();
        if (change.content.size() > kMaxSpeculativeContentBytes) {
            return Error::invalidArgument(where + ".content is larger than 1 MiB");
        }
        // The same containment rules every writer uses, so a path this accepts
        // is a path script_create would have accepted.
        auto resolved = paths::resolveProjectFileForWrite(change.path);
        if (resolved.isErr()) {
            return Error::invalidArgument(where + ".path is not inside the project: " +
                                          resolved.error().message);
        }
        std::error_code error;
        const auto root = fs::weakly_canonical(fs::current_path(), error);
        if (error) return Error::internal("project root cannot be resolved");
        const auto relative = fs::relative(resolved.value(), root, error);
        if (error) return Error::internal("project-relative path cannot be resolved");
        change.relative = paths::projectPathToUtf8(relative.generic_string());

        for (const auto& existing : request.changes) {
            if (existing.relative == change.relative) {
                return Error::invalidArgument(where + " names " + change.path +
                                              " a second time; one file takes one content");
            }
        }
        request.changes.push_back(std::move(change));
    }
    return request;
}

Result<SpeculativeVerifyResult> verifyChangesInSandbox(const SpeculativeVerifyRequest& request) {
    std::error_code error;
    const auto project_root = fs::weakly_canonical(fs::current_path(), error);
    if (error) return Error::internal("The project root cannot be resolved");

    auto toplevel = runGit(project_root, {"rev-parse", "--show-toplevel"}, 30);
    if (toplevel.isErr()) {
        return Error(501, "git is required to build an isolated copy of the project and could "
                          "not be run: " + toplevel.error().message);
    }
    if (toplevel.value().exit_code != 0) {
        return Error(409, "The project is not inside a git work tree, so there is no cheap way to "
                          "build an isolated copy of it to check against");
    }
    const fs::path repository = paths::projectPathFromUtf8(trimmed(toplevel.value().output));

    auto head = runGit(repository, {"rev-parse", "HEAD"}, 30);
    if (head.isErr() || head.value().exit_code != 0) {
        return Error(409, "The repository has no commit to build an isolated copy from");
    }

    SpeculativeVerifyResult result;
    result.base_commit = trimmed(head.value().output);

    // Where the project sits inside the repository, so the copy can be pointed
    // at the same place rather than at the repository root.
    const auto project_within_repository = fs::relative(project_root, repository, error);
    if (error) return Error::internal("The project is not inside the repository it reports");

    const auto sandbox_root = fs::temp_directory_path(error) / makeSandboxName();
    if (error) return Error::internal("No temporary directory is available for the sandbox");

    auto added = runGit(repository, {"worktree", "add", "--detach",
                                     paths::projectPathToUtf8(sandbox_root), result.base_commit},
                        request.timeout_seconds);
    if (added.isErr() || added.value().exit_code != 0) {
        return Error(500, "Could not create an isolated copy of the project: " +
                              (added.isErr() ? added.error().message
                                             : boundedText(added.value().output, 512)));
    }
    SandboxGuard guard(repository, sandbox_root);

    // Uncommitted work is carried across, because a check that ignored it would
    // be answering a question about a project nobody has open.
    auto diff = runGit(repository, {"diff", "HEAD"}, request.timeout_seconds);
    if (diff.isErr()) return Error(500, "Could not read the uncommitted changes to carry across");
    if (!trimmed(diff.value().output).empty()) {
        if (diff.value().output_truncated) {
            return Error(413, "The uncommitted changes are too large to carry into an isolated "
                              "copy; commit or stash them first");
        }
        const auto patch_path = sandbox_root / ".didi-uncommitted.patch";
        std::ofstream patch(patch_path, std::ios::binary);
        if (!patch) return Error(500, "Could not stage the uncommitted changes for the copy");
        patch << diff.value().output;
        patch.close();
        auto applied = runGit(sandbox_root, {"apply", "--whitespace=nowarn",
                                             paths::projectPathToUtf8(patch_path)},
                              request.timeout_seconds);
        std::error_code remove_error;
        fs::remove(patch_path, remove_error);
        if (applied.isErr() || applied.value().exit_code != 0) {
            return Error(409, "The uncommitted changes could not be carried into an isolated copy, "
                              "so the check would have run against a project nobody has: " +
                              (applied.isErr() ? applied.error().message
                                               : boundedText(trimmed(applied.value().output), 512)));
        }
        result.carried_uncommitted = true;
    }

    // Untracked files are not in the copy. A proposal that depends on one would
    // be checked against a project missing it, so they are named.
    auto untracked = runGit(project_root, {"ls-files", "--others", "--exclude-standard"},
                            request.timeout_seconds);
    if (untracked.isOk() && untracked.value().exit_code == 0) {
        std::istringstream lines(untracked.value().output);
        std::string line;
        while (std::getline(lines, line) && result.untracked_excluded.size() < 64) {
            const auto name = trimmed(line);
            if (!name.empty()) result.untracked_excluded.push_back(name);
        }
    }

    const auto sandbox_project = sandbox_root / project_within_repository;
    for (const auto& change : request.changes) {
        const auto target = sandbox_project / paths::projectPathFromUtf8(change.relative);
        std::error_code create_error;
        fs::create_directories(target.parent_path(), create_error);
        std::ofstream out(target, std::ios::binary | std::ios::trunc);
        if (!out) {
            return Error(500, "Could not write " + change.path + " into the isolated copy");
        }
        out << change.content;
        out.close();
        ++result.written;
    }

    // Every proposed script is checked with the whole proposal present, which
    // is the part a single-file check cannot do: a script that preloads a
    // sibling has to see the proposed sibling, not the one still on disk.
    const std::string godot = resolveGodotExecutable();
    result.all_ok = true;
    for (const auto& change : request.changes) {
        if (!endsWithGdscript(change.relative)) continue;
        SpeculativeScriptVerdict verdict;
        verdict.path = change.path;
        ProcessRequest check;
        check.executable = godot;
        check.arguments = {"--headless", "--check-only", "-s", change.relative};
        check.working_directory = sandbox_project;
        check.timeout = std::chrono::milliseconds(request.timeout_seconds * 1000);
        auto ran = runProcess(check);
        if (ran.isErr()) {
            verdict.ok = false;
            verdict.detail = "Godot could not be run: " + ran.error().message;
        } else if (ran.value().timed_out) {
            verdict.ok = false;
            verdict.detail = "The check did not finish within the timeout";
        } else {
            verdict.ok = ran.value().exit_code == 0;
            if (!verdict.ok) {
                verdict.detail = boundedText(trimmed(ran.value().output), kMaxDetailBytes);
            }
        }
        if (!verdict.ok) result.all_ok = false;
        result.scripts.push_back(std::move(verdict));
    }
    return result;
}

} // namespace didi::offline
