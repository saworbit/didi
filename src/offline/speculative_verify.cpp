#include "didi/offline/speculative_verify.hpp"

#include "didi/common/atomic_write.hpp"
#include "didi/common/logger.hpp"
#include "didi/common/project_path.hpp"
#include "didi/offline/test_runner.hpp"
#include "didi/offline/deep_domain_support.hpp"
#include "didi/offline/process_runner.hpp"
#include "didi/offline/resource_indexer.hpp"

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
constexpr size_t kMaxRunErrors = 32;
constexpr char kDetailSeparator = '\n';

// Truncates on a character boundary rather than mid-sequence, so a bounded
// diagnostic is still valid UTF-8 and still renders.
std::string boundedText(const std::string& text, size_t maximum_bytes) {
    if (text.size() <= maximum_bytes) return text;
    size_t cut = maximum_bytes;
    while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0u) == 0x80u) --cut;
    return text.substr(0, cut) + "...";
}

bool endsWithExtension(const std::string& path, const std::string& extension) {
    if (path.size() < extension.size()) return false;
    std::string tail = path.substr(path.size() - extension.size());
    for (auto& character : tail) {
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    }
    return tail == extension;
}

bool endsWithGdscript(const std::string& path) {
    return endsWithExtension(path, ".gd");
}

// The project-relative form of a path a caller wrote, under the same
// containment rules every writer applies. Used for the proposed files and for
// the scene to run, so a path one of them accepts is a path the other accepts.
Result<std::string> projectRelativePath(const std::string& path) {
    auto resolved = paths::resolveProjectFileForWrite(path);
    if (resolved.isErr()) return resolved.error();
    std::error_code error;
    const auto root = fs::weakly_canonical(fs::current_path(), error);
    if (error) return Error::internal("project root cannot be resolved");
    const auto relative = fs::relative(resolved.value(), root, error);
    if (error) return Error::internal("project-relative path cannot be resolved");
    return paths::projectPathToUtf8(relative.generic_string());
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

// The error-level lines Godot printed, in order and bounded.
//
// The exit code is not the whole answer and never was. `--check-only` exits 0
// for a plain syntax error and 1 for a preload that resolves to nothing, and a
// scene that throws at runtime exits 0 as well, so a verdict taken from the
// exit code alone calls a broken file fine. `script_check_syntax` has always
// read the stream for exactly this reason; this is the same rule.
//
// Anchored rather than a substring search, so a line that merely contains the
// word is not mistaken for the engine reporting one. A running game printing a
// line that starts with ERROR: still can be, and the run says its verdict comes
// from the error stream.
std::vector<std::string> engineErrorLines(const std::string& output, size_t limit) {
    std::vector<std::string> errors;
    std::istringstream lines(output);
    std::string line;
    while (std::getline(lines, line) && errors.size() < limit) {
        std::string text = trimmed(line);
        if (text.rfind("USER ", 0) == 0) text = text.substr(5);
        if (text.rfind("ERROR:", 0) != 0 && text.rfind("SCRIPT ERROR:", 0) != 0) continue;
        errors.push_back(boundedText(trimmed(line), kMaxDetailBytes));
    }
    return errors;
}

// Opens the proposed project in the copy and reports what the engine said.
//
// The copy is built from a commit and carries no import cache, because Godot
// keeps that in .godot, which a project gitignores. The first run therefore
// pays for the import of every asset the scene touches, and that time comes out
// of the same timeout as the run. A run that does not finish says so rather
// than being reported as a pass.
Result<SpeculativeSceneRun> runSceneInSandbox(const std::string& godot,
                                              const fs::path& sandbox_project,
                                              const SpeculativeVerifyRequest& request,
                                              bool scripts_parsed) {
    if (!fs::exists(sandbox_project / paths::projectPathFromUtf8(request.run_scene_relative))) {
        return Error(404, "run_scene names " + request.run_scene +
                              ", which is not in the project and is not one of the proposed "
                              "files, so there is nothing to run");
    }
    SpeculativeSceneRun run;
    run.path = request.run_scene;
    if (!scripts_parsed) {
        run.errors.push_back("Not run: a proposed script did not parse, so the scene could not "
                             "have loaded for a reason the parse had not already given.");
        return run;
    }
    run.ran = true;
    run.frames = request.run_frames;

    ProcessRequest play;
    play.executable = godot;
    play.arguments = {"--headless", "--quit-after", std::to_string(request.run_frames),
                      request.run_scene_relative};
    play.working_directory = sandbox_project;
    play.timeout = std::chrono::milliseconds(request.timeout_seconds * 1000);
    auto ran = runProcess(play);
    if (ran.isErr()) {
        run.errors.push_back("Godot could not be run: " + ran.error().message);
        return run;
    }
    run.exit_code = ran.value().exit_code;
    run.timed_out = ran.value().timed_out;
    run.errors = engineErrorLines(ran.value().output, kMaxRunErrors);
    run.ok = !run.timed_out && run.exit_code == 0 && run.errors.empty();
    return run;
}

} // namespace

json SpeculativeVerifyResult::toJson() const {
    json scripts_json = json::array();
    for (const auto& verdict : scripts) {
        json entry = {{"path", verdict.path}, {"ok", verdict.ok}};
        if (!verdict.detail.empty()) entry["detail"] = verdict.detail;
        scripts_json.push_back(std::move(entry));
    }
    json payload = {{"execution_mode", "offline"},
                    {"base_commit", base_commit},
                    {"carried_uncommitted", carried_uncommitted},
                    {"untracked_excluded", untracked_excluded},
                    {"written", written},
                    {"scripts", std::move(scripts_json)},
                    {"all_ok", all_ok},
                    {"sandbox_removed", true}};
    if (scene_run.has_value()) {
        payload["scene_run"] = {{"path", scene_run->path},
                                {"ran", scene_run->ran},
                                {"ok", scene_run->ok},
                                {"exit_code", scene_run->exit_code},
                                {"frames", scene_run->frames},
                                {"timed_out", scene_run->timed_out},
                                {"errors", scene_run->errors}};
    }
    return payload;
}

json SpeculativeApplyResult::toJson() const {
    json payload = verification.toJson();
    payload["applied"] = applied;
    payload["applied_files"] = written;
    return payload;
}

Result<SpeculativeVerifyRequest> parseSpeculativeVerifyRequest(const json& params) {
    if (!params.is_object()) {
        return Error::invalidArgument("Verification params must be an object");
    }
    for (auto it = params.begin(); it != params.end(); ++it) {
        if (it.key() != "changes" && it.key() != "timeout_seconds" &&
            it.key() != "run_scene" && it.key() != "run_frames") {
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

    if (params.contains("run_frames")) {
        const auto& value = params["run_frames"];
        if (!value.is_number_integer() && !value.is_number_unsigned()) {
            return Error::invalidArgument("run_frames must be an integer");
        }
        const auto frames = value.get<int64_t>();
        if (frames < 1 || frames > 6000) {
            return Error::invalidArgument("run_frames must be from 1 to 6000");
        }
        request.run_frames = static_cast<int>(frames);
    }
    if (params.contains("run_scene")) {
        if (!params["run_scene"].is_string()) {
            return Error::invalidArgument("run_scene must be a string");
        }
        request.run_scene = params["run_scene"].get<std::string>();
        if (request.run_scene.empty()) {
            return Error::invalidArgument("run_scene must name a scene, or be left out");
        }
        auto relative = projectRelativePath(request.run_scene);
        if (relative.isErr()) {
            return Error::invalidArgument("run_scene is not inside the project: " +
                                          relative.error().message);
        }
        request.run_scene_relative = relative.value();
        if (!endsWithExtension(request.run_scene_relative, ".tscn") &&
            !endsWithExtension(request.run_scene_relative, ".scn")) {
            return Error::invalidArgument("run_scene must be a .tscn or .scn scene");
        }
    } else if (params.contains("run_frames")) {
        return Error::invalidArgument("run_frames only means something with run_scene");
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
        auto relative = projectRelativePath(change.path);
        if (relative.isErr()) {
            return Error::invalidArgument(where + ".path is not inside the project: " +
                                          relative.error().message);
        }
        change.relative = relative.value();

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

    // Neither the checks nor the run wants a Godot that publishes a runtime
    // session. Both are engines Didi started to answer a question, and a
    // session from one of them is a session the next discovery would find.
    const ScopedOfflineHelperEnvironment offline_helper;

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
            // Break caught: this judged the exit code alone, and Godot exits 0
            // for a script with a plain syntax error while printing the parse
            // error. A proposal full of them read back as all_ok, which is the
            // one answer this tool must never give wrongly.
            const auto errors = engineErrorLines(ran.value().output, kMaxRunErrors);
            verdict.ok = ran.value().exit_code == 0 && errors.empty();
            if (!verdict.ok) {
                std::string detail;
                for (const auto& error : errors) {
                    if (!detail.empty()) detail.push_back(kDetailSeparator);
                    detail += error;
                }
                if (detail.empty()) detail = trimmed(ran.value().output);
                verdict.detail = boundedText(detail, kMaxDetailBytes);
            }
        }
        if (!verdict.ok) result.all_ok = false;
        result.scripts.push_back(std::move(verdict));
    }

    if (!request.run_scene_relative.empty()) {
        auto run = runSceneInSandbox(godot, sandbox_project, request, result.all_ok);
        if (run.isErr()) return run.error();
        if (!run.value().ok) result.all_ok = false;
        result.scene_run = std::move(run.value());
    }
    return result;
}

Result<SpeculativeApplyResult> applyVerifiedChanges(const SpeculativeVerifyRequest& request) {
    auto verified = verifyChangesInSandbox(request);
    if (verified.isErr()) return verified.error();

    SpeculativeApplyResult result;
    result.verification = std::move(verified.value());
    // A proposal that did not pass is reported exactly as it stands. Writing it
    // anyway would make the check decoration.
    if (!result.verification.all_ok) return result;

    std::error_code error;
    const auto project_root = fs::weakly_canonical(fs::current_path(), error);
    if (error) return Error::internal("The project root cannot be resolved");

    // Everything that can fail on the way to disk fails here, before any
    // destination is replaced, so the proposal cannot stop half applied because
    // the last file was the one that could not be written.
    std::vector<files::StagedWrite> staged;
    staged.reserve(request.changes.size());
    for (const auto& change : request.changes) {
        const auto target = project_root / paths::projectPathFromUtf8(change.relative);
        std::error_code directory_error;
        fs::create_directories(target.parent_path(), directory_error);
        if (directory_error) {
            return Error(500, "Could not create the directory for " + change.path +
                                  ", so nothing was written");
        }
        auto write = files::stageFileWrite(target, change.content);
        if (write.isErr()) {
            return Error(500, "Preparing the write failed at " + change.path +
                                  ", so nothing was written: " + write.error().message);
        }
        staged.push_back(std::move(write.value()));
    }

    for (size_t index = 0; index < staged.size(); ++index) {
        auto done = staged[index].commit();
        if (done.isErr()) {
            // The one outcome that is neither all nor nothing, so it says which
            // files moved rather than reporting a failure that sounds total.
            json remaining = json::array();
            for (size_t rest = index; rest < request.changes.size(); ++rest) {
                remaining.push_back(request.changes[rest].path);
            }
            return Error(500, "The proposal was staged but a file could not be replaced. The "
                              "files in committed_files were written and the rest were not.",
                         {{"committed_files", result.written},
                          {"unchanged_files", std::move(remaining)},
                          {"failed_file", request.changes[index].path}});
        }
        result.written.push_back(request.changes[index].path);
    }

    ResourceIndexer::invalidateSharedIndex();
    result.applied = true;
    return result;
}

} // namespace didi::offline
