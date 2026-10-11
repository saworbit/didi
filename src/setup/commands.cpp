#include "didi/setup/commands.hpp"

#include "didi/common/ipc_channel.hpp"
#include "didi/common/logger.hpp"
#include "didi/common/project_path.hpp"
#include "didi/mcp/mcp_protocol.hpp"
#include "didi/offline/class_reference.hpp"
#include "didi/offline/test_runner.hpp"
#include "didi/runtime/session_client.hpp"
#include "didi/setup/addon_install.hpp"
#include "didi/setup/build_identity.hpp"
#include "didi/setup/client_config.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <thread>

namespace didi::setup {
namespace {

using Clock = std::chrono::steady_clock;

std::string utf8(const std::filesystem::path& path) { return paths::projectPathToUtf8(path); }

// One line of the report. `state` is one of done, unchanged, ok, warn, fail
// and skipped: what setup changed, what it found already right, and what it
// could not do. A step that passes says what it looked at, the way the dock's
// Diagnostics page does, so two machines can be compared line by line.
struct Step {
    std::string id;
    std::string title;
    std::string state;
    std::string detail;
    json data = json::object();
};

struct Report {
    std::string command;
    std::string project;
    std::vector<Step> steps;

    Step& add(std::string id, std::string title, std::string state, std::string detail) {
        steps.push_back({std::move(id), std::move(title), std::move(state), std::move(detail), json::object()});
        return steps.back();
    }
    bool failed() const {
        for (const auto& step : steps) {
            if (step.state == "fail") return true;
        }
        return false;
    }
};

int finish(const Report& report, bool as_json) {
    const auto exe = offline::executablePath();
    if (as_json) {
        json steps = json::array();
        for (const auto& step : report.steps) {
            json entry = {{"step", step.id}, {"title", step.title}, {"state", step.state}, {"detail", step.detail}};
            for (const auto& item : step.data.items()) entry[item.key()] = item.value();
            steps.push_back(std::move(entry));
        }
        json answer = {
            {"command", report.command},
            {"project", report.project},
            {"server", {{"executable", utf8(exe)}, {"version", mcp::kServerVersion}, {"build_id", ownBuildId().text}}},
            {"ok", !report.failed()},
            {"steps", std::move(steps)},
        };
        std::cout << answer.dump(2, ' ', false) << std::endl;
    } else {
        std::cout << "didi " << report.command << ": " << report.project << "\n";
        size_t failures = 0;
        for (const auto& step : report.steps) {
            if (step.state == "fail") ++failures;
            std::cout << "  [" << step.state << "] " << step.title << ": " << step.detail << "\n";
        }
        if (failures > 0) {
            std::cout << failures << (failures == 1 ? " step" : " steps") << " failed.\n";
        }
        std::cout.flush();
    }
    return report.failed() ? 1 : 0;
}

int refuse(const std::string& command, const std::string& message) {
    std::cerr << "didi " << command << " refused: " << message << "\n"
              << "Run didi " << command << " --help for the supported options." << std::endl;
    return 2;
}

bool takeValue(const std::vector<std::string>& arguments, size_t& index, std::string& out) {
    if (index + 1 >= arguments.size()) return false;
    const auto& value = arguments[index + 1];
    if (value.empty() || value[0] == '-') return false;
    out = value;
    ++index;
    return true;
}

int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// A Mach-O library whose code this machine cannot run. Existence is not the
// question on macOS: the release builds one arm64 dylib, and the dock's
// Diagnostics page said "Extension binary: OK" for it on an Intel Mac (#648).
std::string machoMismatch(const std::filesystem::path& library) {
#if defined(__APPLE__)
    std::ifstream input(library, std::ios::binary);
    unsigned char bytes[8] = {};
    if (!input.read(reinterpret_cast<char*>(bytes), 8)) return "";
    const auto word = [&bytes](int at) {
        return static_cast<uint32_t>(bytes[at]) | static_cast<uint32_t>(bytes[at + 1]) << 8 |
               static_cast<uint32_t>(bytes[at + 2]) << 16 | static_cast<uint32_t>(bytes[at + 3]) << 24;
    };
    const uint32_t magic = word(0);
    if (magic != 0xFEEDFACFu) return "";  // fat binaries hold every slice; not this check's question
    const uint32_t cpu = word(4);
    const char* holds = cpu == 0x0100000Cu ? "arm64" : cpu == 0x01000007u ? "x86_64" : nullptr;
    if (!holds) return "";
#if defined(__aarch64__) || defined(__arm64__)
    const char* running = "arm64";
#else
    const char* running = "x86_64";
#endif
    if (std::string(holds) == running) return "";
    return utf8(library) + " holds " + holds + " code and this machine is " + running +
           ", so Godot cannot load it. The published macOS archive is arm64 only; build Didi from source "
           "for this machine.";
#else
    (void)library;
    return "";
#endif
}

struct ListedSession {
    std::string session_id;
    std::string kind;
    uint64_t pid{0};
    std::string build_id;
    std::string engine_version;
    int64_t started_at_ms{0};
    bool alive{false};
};

std::vector<ListedSession> listSessions(const std::shared_ptr<runtime::IRuntimeSessionClient>& sessions,
                                        const std::optional<std::string>& project) {
    std::vector<ListedSession> found;
    auto listed = sessions->listSessions(project);
    if (listed.isErr() || !listed.value().contains("sessions")) return found;
    for (const auto& entry : listed.value()["sessions"]) {
        if (!entry.is_object()) continue;
        ListedSession session;
        session.session_id = entry.value("session_id", std::string());
        session.kind = entry.value("kind", std::string());
        session.pid = entry.value("pid", static_cast<uint64_t>(0));
        session.build_id = entry.value("build_id", std::string());
        session.engine_version = entry.value("engine_version", std::string());
        session.started_at_ms = entry.value("started_at_ms", static_cast<int64_t>(0));
        session.alive = entry.value("alive", false);
        found.push_back(std::move(session));
    }
    return found;
}

std::vector<ListedSession> liveEditors(const std::shared_ptr<runtime::IRuntimeSessionClient>& sessions,
                                       const std::string& project) {
    std::vector<ListedSession> editors;
    for (auto& session : listSessions(sessions, project)) {
        if (session.alive && session.kind == "editor") editors.push_back(std::move(session));
    }
    return editors;
}

// Whether one published editor answers, asked the way a server asks: attach,
// which is an authenticated handshake, then one request the editor's main
// thread answers. The route is released before returning, so the session lock
// is free for the server a client starts next.
struct Answer {
    enum class Kind { Answered, Held, Silent } kind{Kind::Silent};
    std::string reason;
};

Answer askEditor(const std::shared_ptr<runtime::IRuntimeSessionClient>& sessions, const ListedSession& editor) {
    Answer answer;
    auto attached = sessions->attachSession(editor.session_id);
    if (attached.isErr()) {
        // 423 is the session lock: another Didi server, most likely the one a
        // client started, holds the bridge. The editor is up and owned.
        answer.kind = attached.error().code == 423 ? Answer::Kind::Held : Answer::Kind::Silent;
        answer.reason = attached.error().message;
        return answer;
    }
    if (const auto lease = runtime::acquireRuntimeRouteLease(sessions)) {
        // The scenes open in its tabs: answered on the editor's main thread,
        // and answerable by a project with no scene at all, which editor.getState
        // is not. A refusal is an answer too. Only a transport that never
        // carried one back (503) or a deadline (504) means nobody answered.
        auto open = lease->sendRequest("editor.openScenes", json::object(), ipc::withAcceptAllowance(3000));
        if (open.isOk() || (open.error().code != 503 && open.error().code != 504)) {
            answer.kind = Answer::Kind::Answered;
        } else {
            answer.reason = open.error().message;
        }
    } else {
        answer.reason = "the route closed before the editor was asked anything";
    }
    (void)sessions->detachSession();
    return answer;
}

void describeEditor(Step& step, const ListedSession& editor) {
    step.data["editor"] = {{"pid", editor.pid},
                           {"session_id", editor.session_id},
                           {"engine_version", editor.engine_version},
                           {"build_id", editor.build_id}};
}

// Waits for an editor on this project to publish a session and answer.
// `launched_at_ms` set means only an editor that started after that moment
// counts, so a descriptor some earlier editor left behind cannot pass for the
// one just started.
void waitForEditor(Report& report, const std::shared_ptr<runtime::IRuntimeSessionClient>& sessions,
                   const std::string& project, std::optional<int64_t> launched_at_ms, int timeout_seconds,
                   const BuildId& own) {
    const auto deadline = Clock::now() + std::chrono::seconds(timeout_seconds);
    bool published = false;
    std::string last_reason;
    while (true) {
        for (const auto& editor : liveEditors(sessions, project)) {
            if (launched_at_ms && editor.started_at_ms < *launched_at_ms) continue;
            published = true;
            const auto answer = askEditor(sessions, editor);
            if (answer.kind == Answer::Kind::Silent) {
                last_reason = answer.reason;
                continue;
            }
            if (!editor.build_id.empty() && editor.build_id != own.text) {
                auto& step = report.add("editor", "Editor", "fail",
                                        "Godot (pid " + std::to_string(editor.pid) + ") loaded build " +
                                            editor.build_id + ", not this server's " + own.text +
                                            ". Close it and open the project again so it loads the addon "
                                            "setup installed.");
                describeEditor(step, editor);
                return;
            }
            auto& step = report.add(
                "editor", "Editor", "ok",
                answer.kind == Answer::Kind::Answered
                    ? (editor.engine_version.empty() ? std::string("Godot") : editor.engine_version) + " (pid " +
                          std::to_string(editor.pid) + ") answers with build " + editor.build_id + "."
                    : "Godot (pid " + std::to_string(editor.pid) + ") answers, and another Didi server holds "
                      "its bridge, which is how a client's server holds it.");
            describeEditor(step, editor);
            return;
        }
        if (Clock::now() >= deadline) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    const auto waited = std::to_string(timeout_seconds) + " s";
    if (!published) {
        report.add("editor", "Editor", "fail",
                   "No Godot editor published a Didi session for this project within " + waited +
                       ". The editor may still be importing the project, may have exited, or may not have "
                       "loaded the extension: check its output, then run didi doctor --project " +
                       report.project + ".");
    } else {
        report.add("editor", "Editor", "fail",
                   "An editor published a session for this project but did not answer within " + waited + ": " +
                       last_reason);
    }
}

// Whether each configured client's instructions file reaches the didi block
// (#1277). Claude Code reads the project's CLAUDE.md files and what they
// import; every other client here reads AGENTS.md.
void reportAgentGuides(Report& report, const std::filesystem::path& root, const std::vector<Client>& named) {
    const auto agents = root / "AGENTS.md";
    const auto relative = [&root](const std::filesystem::path& file) { return utf8(std::filesystem::relative(file, root)); };
    if (std::find(named.begin(), named.end(), Client::ClaudeCode) != named.end()) {
        const auto claude = projectClaudeFiles(root);
        std::string state = "warn";
        std::string detail;
        std::optional<std::filesystem::path> through = claude.empty() ? std::nullopt : std::optional(claude.front());
        for (const auto& file : claude) {
            const auto holder = agentGuideReachedFrom(file);
            if (!holder) continue;
            state = "ok";
            through = file;
            detail = *holder == file ? relative(file) + " holds the didi block"
                                    : relative(file) + " imports " + relative(*holder) + ", which holds the didi block";
            break;
        }
        if (state == "ok") {
            // Named above, with the file it was found through.
        } else if (!claude.empty()) {
            std::string names;
            for (size_t i = 0; i < claude.size(); ++i) names += (i ? ", " : "") + relative(claude[i]);
            detail = "Claude Code reads " + names + " here and not AGENTS.md, and " +
                     (claude.size() == 1 ? "it neither holds the didi block nor imports"
                                         : "none of them holds the didi block or imports") +
                     " a file that does. Run didi setup --client claude-code.";
        } else if (hasAgentGuide(agents)) {
            if (const auto above = claudeFileAbove(root)) {
                detail = "The guide is in AGENTS.md, which Claude Code never reads below " + utf8(*above) + ".";
            } else {
                detail = "The guide is in AGENTS.md, which Claude Code reads on its own only from v2.1.277, and "
                         "not in every session.";
            }
            detail += " Run didi setup --client claude-code to add a CLAUDE.md that imports it.";
        } else {
            detail = "No CLAUDE.md or AGENTS.md here holds the didi block. Run didi setup --client claude-code.";
        }
        auto& step = report.add("agent-guide:claude-code", "Agent guide (Claude Code)", state, detail);
        if (through) step.data["file"] = utf8(*through);
    }
    std::string readers;
    std::string first;
    for (const auto client : named) {
        if (client == Client::ClaudeCode) continue;
        readers += (readers.empty() ? "" : ", ") + std::string(clientSpec(client).display);
        if (first.empty()) first = clientSpec(client).name;
    }
    if (readers.empty()) return;
    const bool held = hasAgentGuide(agents);
    auto& step = report.add("agent-guide:agents-md", "Agent guide (" + readers + ")", held ? "ok" : "warn",
                            held ? "AGENTS.md holds the didi block"
                                 : "AGENTS.md holds no didi block, and " + readers +
                                       " read it. Run didi setup --client " + first + ".");
    step.data["file"] = utf8(agents);
}

void printSetupHelp() {
    std::cout
        << "Usage: didi setup --project <dir> [options]\n\n"
           "Installs the addon that matches this binary into the project, enables it, writes the\n"
           "configuration for each named client and an agent guide, and can start the editor and\n"
           "wait until it answers. A rerun changes nothing that is already right.\n\n"
           "Options:\n"
           "  -p, --project <dir>   The Godot project: the directory holding project.godot\n"
           "  --client <name>       Write this client's project configuration. Repeat it, or give a\n"
           "                        comma-separated list: claude-code, cursor, vscode, codex, or all\n"
           "  --godot <exe>         Start this Godot editor on the project and wait until it answers\n"
           "  --headless            Start that editor without a window\n"
           "  --wait                Wait for an editor someone else starts\n"
           "  --timeout <seconds>   How long to wait for the editor (default 120)\n"
           "  --replace-addon       Install this build over an addon that is newer, or whose build\n"
           "                        cannot be told\n"
           "  --no-agent-guide      Do not write the agent guide\n"
           "  --json                Print the report as JSON\n";
}

void printDoctorHelp() {
    std::cout << "Usage: didi doctor --project <dir> [--json]\n\n"
                 "Runs the checks the dock's Diagnostics page runs, from the command line: the addon and\n"
                 "its build, the plugin, the session directory, the live editor, and each client\n"
                 "configuration in the project. Exits 1 when a check fails.\n";
}

}  // namespace

int runSetupCommand(const std::vector<std::string>& arguments) {
    Logger::instance().setLevel(LogLevel::Warn);
    // Named on the command line and nowhere else. These commands write into the
    // project, and a variable left over in a shell is not a choice of project.
    std::string project;
    std::vector<Client> clients;
    std::string godot;
    bool headless = false;
    bool wait = false;
    bool replace_addon = false;
    bool agent_guide = true;
    bool as_json = false;
    int timeout_seconds = 120;

    for (size_t i = 0; i < arguments.size(); ++i) {
        const auto& arg = arguments[i];
        if (arg == "--help" || arg == "-h") {
            printSetupHelp();
            return 0;
        } else if (arg == "--project" || arg == "-p") {
            if (!takeValue(arguments, i, project)) return refuse("setup", arg + " expects a directory");
        } else if (arg == "--client") {
            std::string names;
            if (!takeValue(arguments, i, names)) return refuse("setup", "--client expects a client name");
            for (const auto& name : strings::split(names, ',')) {
                const auto trimmed = strings::trim(name);
                std::vector<Client> named;
                if (trimmed == "all") {
                    for (const auto& spec : clientSpecs()) named.push_back(spec.id);
                } else if (const auto client = parseClient(trimmed)) {
                    named.push_back(*client);
                } else {
                    std::string known;
                    for (const auto& spec : clientSpecs()) known += std::string(spec.name) + ", ";
                    return refuse("setup", "unknown client " + trimmed + ". Known: " + known + "all");
                }
                for (const auto client : named) {
                    if (std::find(clients.begin(), clients.end(), client) == clients.end()) clients.push_back(client);
                }
            }
        } else if (arg == "--godot") {
            if (!takeValue(arguments, i, godot)) return refuse("setup", "--godot expects a Godot executable");
        } else if (arg == "--headless") {
            headless = true;
        } else if (arg == "--wait") {
            wait = true;
        } else if (arg == "--timeout") {
            std::string value;
            if (!takeValue(arguments, i, value)) return refuse("setup", "--timeout expects a number of seconds");
            char* end = nullptr;
            const long parsed = std::strtol(value.c_str(), &end, 10);
            if (end == value.c_str() || *end != '\0' || parsed < 1 || parsed > 3600) {
                return refuse("setup", "--timeout expects whole seconds from 1 to 3600, not " + value);
            }
            timeout_seconds = static_cast<int>(parsed);
        } else if (arg == "--replace-addon") {
            replace_addon = true;
        } else if (arg == "--no-agent-guide") {
            agent_guide = false;
        } else if (arg == "--json") {
            as_json = true;
        } else {
            return refuse("setup", (arg.rfind('-', 0) == 0 ? "unknown option " : "unexpected argument ") + arg);
        }
    }
    if (headless && godot.empty()) return refuse("setup", "--headless applies to the editor --godot starts");

    auto resolved = paths::resolveExplicitProjectRoot(project);
    if (resolved.isErr()) return refuse("setup", resolved.error().message);
    const auto root = resolved.value();
    const auto root_native = paths::nativePathToUtf8(root);

    Report report;
    report.command = "setup";
    report.project = utf8(root);
    const auto own = ownBuildId();
    const auto executable = offline::executablePath();

    // The addon that came with this binary, and proof that it is this build.
    // An addon from another build is the stale bridge #326 described, and
    // installing it is how a project gets one.
    const auto search = findBundledAddon(executable.parent_path());
    if (!search.found) {
        std::string looked;
        for (const auto& place : search.searched) looked += (looked.empty() ? "" : ", ") + utf8(place);
        report.add("addon", "Addon", "fail",
                   "No addon beside this binary. Looked in " + looked +
                       ". Run the didi from a release archive or a build directory, which carry one.");
        return finish(report, as_json);
    }
    const auto source = inspectAddon(*search.found);
    if (!source.build || source.build->text != own.text) {
        report.add("addon", "Addon", "fail",
                   "The addon at " + utf8(*search.found) + " is " +
                       (source.build ? "build " + source.build->text
                                     : std::string("of no readable build (") + source.build_problem + ")") +
                       ", not this server's " + own.text +
                       ". Rebuild, or run the didi that came with that addon.");
        return finish(report, as_json);
    }

    // An open editor holds the extension library and rewrites project.godot
    // when it saves its settings. Both writes wait for it to close; when
    // neither is needed, the editor is welcome to stay open.
    auto sessions = runtime::createRuntimeSessionClient(root_native);
    const auto target = root / "addons" / "didi";
    const auto installed = inspectAddon(target);
    const bool addon_current = installed.build && installed.build->text == own.text &&
                               !addonDifference(*search.found, target);
    const bool plugin_current = pluginEnabled(root);
    const auto open_editors = liveEditors(sessions, root_native);
    if (!open_editors.empty() && (!addon_current || !plugin_current)) {
        report.add("editor", "Editor", "fail",
                   "Godot (pid " + std::to_string(open_editors.front().pid) +
                       ") has this project open. Close it and run setup again: it holds the extension "
                       "library, and saving its settings would overwrite project.godot. Nothing was changed.");
        return finish(report, as_json);
    }

    auto addon = installAddon(root, source, replace_addon);
    if (addon.isErr()) {
        report.add("addon", "Addon", "fail", addon.error().message);
        return finish(report, as_json);
    }
    {
        const auto& value = addon.value();
        const char* state = value.action == AddonAction::Unchanged  ? "unchanged"
                            : value.action == AddonAction::Refused ? "fail"
                                                                   : "done";
        auto& step = report.add("addon", "Addon", state, value.detail);
        step.data["build_id"] = own.text;
        if (value.previous) step.data["previous_build_id"] = value.previous->text;
        if (value.action == AddonAction::Refused) return finish(report, as_json);
    }

    auto plugin = enablePlugin(root);
    if (plugin.isErr()) {
        report.add("plugin", "Plugin", "fail", plugin.error().message);
        return finish(report, as_json);
    }
    report.add("plugin", "Plugin", plugin.value().changed ? "done" : "unchanged",
               plugin.value().changed ? "enabled " + std::string(kPluginConfigPath) + " in project.godot"
                                      : std::string(kPluginConfigPath) + " is already enabled");

    const auto command = utf8(executable);
    if (paths::isWithinProject(root, executable)) {
        // The dock says the same and does not refuse: a binary under res:// is
        // one Didi's own file tools can write, so whoever chose it should know.
        report.add("server", "Server", "warn",
                   command + " is inside the project, where an assistant's file tools can reach it. Keep "
                   "the server outside any project.");
    }
    for (const auto client : clients) {
        const auto& spec = clientSpec(client);
        auto written = writeClientConfig(root, client, command);
        if (written.isErr()) {
            report.add(std::string("client:") + spec.name, spec.display, "fail", written.error().message);
            continue;
        }
        const auto& value = written.value();
        std::string detail;
        if (value.action == FileAction::Unchanged) {
            detail = std::string(spec.config_file) + " already starts this server";
        } else {
            detail = std::string(fileActionWord(value.action)) + " " + spec.config_file;
            if (value.previous_command) detail += ", whose didi server started " + *value.previous_command;
            detail += ". " + std::string(spec.next_step);
        }
        auto& step = report.add(std::string("client:") + spec.name, spec.display,
                                value.action == FileAction::Unchanged ? "unchanged" : "done", detail);
        step.data["file"] = utf8(value.file);
    }

    if (agent_guide) {
        // A project with no CLAUDE.md of its own gets one that imports the
        // AGENTS.md the guide goes into (#1277).
        const auto import_file = agentsImportFile(root, clients);
        bool guide_failed = false;
        for (const auto& file : agentGuideFiles(root, clients)) {
            const auto relative = utf8(std::filesystem::relative(file, root));
            auto written = writeAgentGuide(file);
            if (written.isErr()) {
                report.add("agent-guide", "Agent guide", "fail", written.error().message);
                guide_failed = true;
                continue;
            }
            const bool unchanged = written.value() == FileAction::Unchanged;
            auto& step = report.add("agent-guide", "Agent guide", unchanged ? "unchanged" : "done",
                                    unchanged ? relative + " already holds the didi block"
                                              : (written.value() == FileAction::Created ? "created " : "wrote the didi block into ") +
                                                    relative);
            step.data["file"] = utf8(file);
        }
        // Without the guide there is nothing for the import to reach, and the
        // next run would find the import and take it as done.
        if (import_file && !guide_failed) {
            auto written = writeAgentsImport(*import_file);
            if (written.isErr()) {
                report.add("agent-guide", "Agent guide", "fail", written.error().message);
            } else {
                const auto relative = utf8(std::filesystem::relative(*import_file, root));
                std::string detail = written.value() == FileAction::Unchanged
                                         ? relative + " already imports AGENTS.md"
                                         : "created " + relative +
                                               ", which imports AGENTS.md. Claude Code reads AGENTS.md on its own "
                                               "only from v2.1.277, and not in every session";
                if (const auto above = claudeFileAbove(root); above && written.value() == FileAction::Created) {
                    detail += ", and never below " + utf8(*above);
                }
                auto& step = report.add("agent-guide", "Agent guide",
                                        written.value() == FileAction::Unchanged ? "unchanged" : "done", detail);
                step.data["file"] = utf8(*import_file);
            }
        }
    }

    if (!open_editors.empty()) {
        // Already open, and nothing above needed it closed. A second editor on
        // the same project would be two processes writing one .godot folder,
        // so --godot is not acted on; the open one is asked instead.
        waitForEditor(report, sessions, root_native, std::nullopt, 10, own);
        if (!godot.empty()) report.steps.back().detail += " It was already open, so --godot started nothing.";
    } else if (!godot.empty()) {
        // The engine resolution every other Didi launch uses, pointed at the
        // executable named here, so a path it would reject is rejected with
        // the same reason.
#if defined(_WIN32)
        // Wide, because that is how the resolver reads it; the narrow form goes
        // through the ANSI code page and mangles any path outside it (#611).
        _wputenv_s(L"GODOT_BIN", paths::projectPathFromUtf8(godot).c_str());
#else
        setenv("GODOT_BIN", godot.c_str(), 1);
#endif
        const auto engine = offline::resolveGodotExecutableDetailed();
        if (!engine.configured_rejected.empty()) {
            report.add("editor", "Editor", "fail", "--godot " + godot + " was not used: " + engine.configured_rejected);
            return finish(report, as_json);
        }
        const auto launched_at = nowMs();
        offline::TestRunner runner;
        const auto launch = runner.runSession("", timeout_seconds, headless, false,
                                              {"--editor", "--path", root_native}, true);
        if (launch.launch_failed) {
            report.add("editor", "Editor", "fail",
                       "Godot could not be started from " + engine.executable + ": " + launch.launch_error);
            return finish(report, as_json);
        }
        waitForEditor(report, sessions, root_native, launched_at, timeout_seconds, own);
        report.steps.back().data["launched_pid"] = launch.pid;
    } else if (wait) {
        waitForEditor(report, sessions, root_native, std::nullopt, timeout_seconds, own);
    } else {
        report.add("editor", "Editor", "skipped",
                   "not started. Open the project in Godot to load the addon, or run setup again with "
                   "--godot <editor executable>.");
    }
    sessions->disconnect();
    return finish(report, as_json);
}

int runDoctorCommand(const std::vector<std::string>& arguments) {
    Logger::instance().setLevel(LogLevel::Warn);
    std::string project;
    bool as_json = false;
    for (size_t i = 0; i < arguments.size(); ++i) {
        const auto& arg = arguments[i];
        if (arg == "--help" || arg == "-h") {
            printDoctorHelp();
            return 0;
        } else if (arg == "--project" || arg == "-p") {
            if (!takeValue(arguments, i, project)) return refuse("doctor", arg + " expects a directory");
        } else if (arg == "--json") {
            as_json = true;
        } else {
            return refuse("doctor", (arg.rfind('-', 0) == 0 ? "unknown option " : "unexpected argument ") + arg);
        }
    }
    auto resolved = paths::resolveExplicitProjectRoot(project);
    if (resolved.isErr()) return refuse("doctor", resolved.error().message);
    const auto root = resolved.value();
    const auto root_native = paths::nativePathToUtf8(root);
    const auto own = ownBuildId();

    Report report;
    report.command = "doctor";
    report.project = utf8(root);

    // The titles are the dock's own where a check is the same check, so a
    // person comparing the two sees the same rows.
    const auto executable = offline::executablePath();
    report.add("server", "Server executable", "ok",
               utf8(executable) + ", didi " + mcp::kServerVersion + ", build " + own.text);

    const auto addon = inspectAddon(root / "addons" / "didi");
    const auto library = root / "addons" / "didi" / "bin" / extensionLibraryName();
    if (!addon.present) {
        report.add("extension", "Extension binary", "fail",
                   "res://addons/didi is not installed. Run didi setup --project " + report.project + ".");
    } else if (!addon.library_present) {
        report.add("extension", "Extension binary", "fail",
                   utf8(library) + " is missing. Run didi setup --project " + report.project +
                       ", or copy the released library into res://addons/didi/bin/.");
    } else if (const auto mismatch = machoMismatch(library); !mismatch.empty()) {
        report.add("extension", "Extension binary", "fail", mismatch);
    } else {
        report.add("extension", "Extension binary", "ok", utf8(library));
    }

    if (addon.library_present) {
        if (!addon.build) {
            report.add("addon-build", "Addon build", "warn", addon.build_problem);
        } else {
            const auto order = compareBuilds(*addon.build, own);
            if (order == BuildOrder::Same) {
                report.add("addon-build", "Addon build", "ok", "build " + own.text + ", the same as this server");
            } else {
                report.add("addon-build", "Addon build", "warn",
                           "the addon is build " + addon.build->text + ", " + buildOrderWord(order) +
                               " this server's " + own.text +
                               ". The bridge will serve that build's tool contract. Run didi setup from the "
                               "newer one.");
            }
        }
    }

    report.add("plugin", "Plugin enabled", pluginEnabled(root) ? "ok" : "fail",
               pluginEnabled(root) ? std::string(kPluginConfigPath) + " is in editor_plugins/enabled"
                                   : std::string(kPluginConfigPath) +
                                         " is not in editor_plugins/enabled. Run didi setup, or enable it "
                                         "under Project > Project Settings > Plugins.");

    const auto directory = runtime::resolveSessionDescriptorDirectory();
    const auto search = runtime::describeSessionDescriptorSearch();
    if (directory.isErr()) {
        report.add("sessions", "Session directory", "fail", directory.error().message);
    } else {
        std::error_code error;
        const bool exists = std::filesystem::is_directory(directory.value(), error) && !error;
        std::string detail = utf8(directory.value());
        if (!exists) detail += " does not exist yet, so no Godot with Didi loaded has run as this user";
        if (!search.elsewhere.empty()) {
            detail += ". Sessions were published elsewhere too: ";
            for (size_t i = 0; i < search.elsewhere.size(); ++i) detail += (i ? ", " : "") + search.elsewhere[i];
            detail += ". Set DIDI_SESSION_DIR so the editor and the server agree.";
        }
        report.add("sessions", "Session directory", exists && search.elsewhere.empty() ? "ok" : "warn", detail);
    }

    auto sessions = runtime::createRuntimeSessionClient(root_native);
    const auto editors = liveEditors(sessions, root_native);
    if (editors.empty()) {
        report.add("bridge", "Live bridge", "warn",
                   "No Godot editor has this project open with Didi loaded, so live tools have nothing to "
                   "reach. Open the project in Godot.");
    } else {
        const auto& editor = editors.front();
        const auto answer = askEditor(sessions, editor);
        std::string detail = "Godot (pid " + std::to_string(editor.pid) + ", session " + editor.session_id + ")";
        std::string state = "ok";
        if (answer.kind == Answer::Kind::Answered) {
            detail += " answers";
        } else if (answer.kind == Answer::Kind::Held) {
            detail += " is attached to another Didi server, which is how a client's server holds it";
        } else {
            state = "fail";
            detail += " published a session and did not answer: " + answer.reason;
        }
        if (!editor.build_id.empty() && editor.build_id != own.text) {
            if (state == "ok") state = "warn";
            detail += ". It loaded build " + editor.build_id + ", not this server's " + own.text +
                      "; restart the editor after an upgrade";
        }
        describeEditor(report.add("bridge", "Live bridge", state, detail + "."), editor);
    }
    size_t other = 0;
    for (const auto& session : listSessions(sessions, std::nullopt)) {
        if (session.alive) ++other;
    }
    other -= std::min(other, editors.size());
    if (other > 0) {
        report.add("other-sessions", "Other sessions", "ok",
                   std::to_string(other) + " other Didi session" + (other == 1 ? " is" : "s are") +
                       " published on this machine.");
    }
    sessions->disconnect();

    bool any_client = false;
    std::vector<Client> named;
    for (const auto& spec : clientSpecs()) {
        const auto configured = readClientConfig(root, spec.id);
        if (!configured.file_present) continue;
        any_client = true;
        if (configured.entry_present) named.push_back(spec.id);
        const auto id = std::string("client:") + spec.name;
        const auto title = std::string(spec.display) + " (" + spec.config_file + ")";
        if (!configured.problem.empty()) {
            report.add(id, title, configured.entry_present ? "warn" : "fail", configured.problem);
            continue;
        }
        if (!configured.entry_present) {
            report.add(id, title, "warn", "names no didi server. Run didi setup --client " + std::string(spec.name) + ".");
            continue;
        }
        std::string state = "ok";
        std::string detail = "starts " + configured.command;
        std::optional<std::string> project_arg;
        for (size_t i = 0; i + 1 < configured.args.size(); ++i) {
            if (configured.args[i] == "--project" || configured.args[i] == "-p") project_arg = configured.args[i + 1];
        }
        if (!project_arg) {
            state = "fail";
            detail += ", with no --project, so the server refuses to start";
        } else {
            std::error_code error;
            const auto named = std::filesystem::weakly_canonical(paths::projectPathFromUtf8(*project_arg), error);
            if (error || paths::normalizedProjectPath(named) != paths::normalizedProjectPath(root)) {
                state = "fail";
                detail += ", for another project: " + *project_arg;
            }
        }
        std::error_code error;
        const auto command_path = paths::projectPathFromUtf8(configured.command);
        if (!std::filesystem::is_regular_file(command_path, error) || error) {
            if (state == "ok") state = "warn";
            detail += ". That is not a file here; a client looks a bare name up on its own PATH";
        } else {
            const auto build = readBuildIdFromBinary(command_path);
            if (build.isOk() && build.value() && addon.build && build.value()->text != addon.build->text) {
                if (state == "ok") state = "warn";
                detail += ", build " + build.value()->text + ", while the addon is " + addon.build->text +
                          "; one of them is stale";
            } else if (build.isOk() && build.value()) {
                detail += ", build " + build.value()->text;
            }
        }
        report.add(id, title, state, detail);
    }
    if (!any_client) {
        report.add("clients", "Client configuration", "warn",
                   "No client configuration in this project. Run didi setup --client <name>, or configure a "
                   "client whose file lives outside the project by hand.");
    }
    reportAgentGuides(report, root, named);
    return finish(report, as_json);
}

}  // namespace didi::setup
