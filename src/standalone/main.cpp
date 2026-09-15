#include "didi/mcp/mcp_server.hpp"
#include "didi/mcp/control_room.hpp"
#include "didi/mcp/tool_registry.hpp"
#include "didi/common/logger.hpp"
#include "didi/common/project_path.hpp"
#include "didi/common/version.hpp"
#include <iostream>
#include <csignal>

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

static didi::mcp::McpServer* g_server = nullptr;
static std::atomic<bool> g_stopRequested{false};

// The flag has to be lock free, or setting it is not safe from a handler
// either. Every platform we build for gives us that; fail the build rather
// than the shutdown on one that does not.
static_assert(std::atomic<bool>::is_always_lock_free,
              "The stop flag is written from a signal handler and must be lock free");

// A handler runs either between two instructions on this thread or, on
// Windows, on a thread the operating system made for the interrupt. Either
// way the only safe thing it can do is set a flag. The CRT is explicit that a
// handler must not touch the heap, stdio, or anything that makes a system
// call, and this one used to call stop(), which joins the board watcher
// thread, sends IPC frames to detach the runtime session, and logs through a
// mutex. All of that now runs on the normal path when runStdio returns.
void signalHandler(int sig) {
    (void)sig;
    g_stopRequested.store(true);
    if (g_server) {
        g_server->requestStop();
    }
}

// One line per value-taking option, shared by --help and by the parse errors
// below, so a refusal can show the exact line a reader needs and the two can
// never drift apart.
static const char* kProjectHelpLine =
    "  -p, --project <dir>   Set Godot project root directory (or use DIDI_PROJECT_ROOT)";
static const char* kPipeNameHelpLine =
    "  --pipe-name <name>    Override Named Pipe / Unix domain socket name (or DIDI_PIPE_NAME)";
static const char* kLogLevelHelpLine =
    "  --log-level <level>   Set log level (DEBUG, INFO, WARN, ERROR, NONE)";
static const char* kUiAppHelpLine =
    "  --ui-app <mode>       MCP Apps dashboard: auto (default), always, or off";
static const char* kHelpHint = "Run didi --help for the supported options.";

#if defined(_WIN32)
// Windows hands a process its command line and its environment as UTF-16. The
// narrow main(argc, argv) the CRT synthesises converts both through the system
// ANSI codepage, which is lossy for every character outside it: a project root
// under C:/Users/Jose/... arrived with the accented byte mangled, the path no
// longer parsed as UTF-8, and the process fast-failed with no output (#611).
// Reading the wide forms and encoding them ourselves is the only way to get
// the bytes the caller actually typed.
static std::optional<std::string> wideToUtf8(const std::wstring& value) {
    if (value.empty()) return std::string();
    if (value.size() > static_cast<size_t>(std::numeric_limits<int>::max())) return std::nullopt;
    const int required = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
        nullptr, 0, nullptr, nullptr);
    if (required <= 0) return std::nullopt;
    std::string utf8(static_cast<size_t>(required), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), utf8.data(), required,
                            nullptr, nullptr) != required) {
        return std::nullopt;
    }
    return utf8;
}
#endif

// The environment reaches the process the same way the command line does, so
// DIDI_PROJECT_ROOT needs the same treatment as --project.
static std::optional<std::string> environmentValue(const char* name) {
#if defined(_WIN32)
    const std::wstring wide_name(name, name + std::strlen(name));
    const DWORD required = GetEnvironmentVariableW(wide_name.c_str(), nullptr, 0);
    if (required == 0) return std::nullopt;
    std::wstring value(required, L'\0');
    const DWORD length = GetEnvironmentVariableW(wide_name.c_str(), value.data(), required);
    if (length == 0 || length >= required) return std::nullopt;
    value.resize(length);
    return wideToUtf8(value);
#else
    const char* value = std::getenv(name);
    if (!value) return std::nullopt;
    return std::string(value);
#endif
}

static void refuse(const std::string& message, const char* help_line) {
    std::cerr << "Didi startup refused: " << message << std::endl;
    if (help_line) std::cerr << help_line << std::endl;
}

// A value-taking option must actually be given a value, and that value must not
// be another option. Without this check `--log-level --yolo` swallows the flag,
// so a launch that asked for YOLO mode starts without it and says nothing.
static bool takeValue(const std::vector<std::string>& arguments, size_t& index,
                      const std::string& option, const char* help_line, std::string& out) {
    if (index + 1 >= arguments.size()) {
        refuse(option + " expects a value", help_line);
        return false;
    }
    const std::string& value = arguments[index + 1];
    if (value.empty()) {
        refuse(option + " expects a value and was given an empty one", help_line);
        return false;
    }
    if (value[0] == '-') {
        refuse(option + " expects a value, but the next argument is the option " + value,
               help_line);
        return false;
    }
    out = value;
    ++index;
    return true;
}

// argv is already UTF-8 by the time it gets here, whichever entry point ran.
static int runDidi(const std::vector<std::string>& arguments) {
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    std::string project_root;
    if (const auto env_root = environmentValue("DIDI_PROJECT_ROOT")) {
        project_root = *env_root;
    }

    // Confirmations can only be turned off from here -- the launch arguments,
    // chosen by the person starting the process. Nothing reachable from a tool
    // call may set this.
    bool skip_confirmations = false;
    auto ui_app_mode = didi::mcp::McpServer::UiAppMode::Auto;
    std::string managed_editor, recovery_workspace;
    if (const auto env_yolo = environmentValue("DIDI_YOLO")) {
        const std::string& value = *env_yolo;
        skip_confirmations = value == "1" || value == "true" || value == "TRUE";
    }

    for (size_t i = 1; i < arguments.size(); ++i) {
        const std::string& arg = arguments[i];
        if (arg == "--version" || arg == "-v") {
            // The build id as well as the version, because the version cannot
            // tell two builds apart and the GDExtension is a separate file a
            // user copies around on its own. A session that has attached
            // reports both halves and says whether they match; before one is
            // attached, this is the only way to record which build a run was
            // handed. Field trial 03 lost an hour to a bridge six days older
            // than the server, and could not have caught it from a version.
            std::cout << "didi (godot-mcp-native) v" << didi::mcp::kServerVersion << "\n"
                      << "build " << didi::kBuildId << std::endl;
            return 0;
        } else if (arg == "--dump-tool-manifest") {
            // Emits the registered tool surface as JSON so documentation can be
            // validated against the software rather than against other
            // documentation. Introspection only: it registers no handlers'
            // side effects, needs no Godot project, and never opens IPC.
            didi::mcp::ToolRegistry::instance().registerAllDefaultTools();
            std::cout << didi::mcp::ToolRegistry::instance()
                             .buildManifest()
                             .toJson()
                             .dump(2)
                      << std::endl;
            return 0;
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Didi - Native Model Context Protocol (MCP) Server for Godot 4.5+\n\n"
                      << "Usage:\n"
                      << "  didi [options]\n\n"
                      << "Options:\n"
                      << "  -v, --version         Show version information\n"
                      << "  -h, --help            Show this help dialog\n"
                      << kProjectHelpLine << "\n"
                      << kPipeNameHelpLine << "\n"
                      << kLogLevelHelpLine << "\n"
                      << kUiAppHelpLine << "\n"
                      << "  --dump-tool-manifest  Print the registered tool surface as JSON and exit\n"
                      << "  --managed-editor <exe>  Own a headless Godot editor with saved-file recovery\n"
                      << "  --recovery-workspace <new-dir>  Copy the project here; required with --managed-editor\n"
                      << "  --yolo                Skip confirmation on destructive tools (or DIDI_YOLO=1)\n"
                      << "                        For unattended runs. Mutations execute without review,\n"
                      << "                        and each affected result records confirmation: skipped.\n"
                      << "\n"
                      << "MCP Protocol:\n"
                      << "  Communicates over standard I/O (JSON-RPC 2.0) with AI coding assistants.\n"
                      << "  Connects to Godot 4.5+ editor via native named pipes.\n";
            return 0;
        } else if (arg == "--managed-editor") {
            if (!takeValue(arguments, i, arg, kHelpHint, managed_editor)) return 2;
        } else if (arg == "--recovery-workspace") {
            if (!takeValue(arguments, i, arg, kHelpHint, recovery_workspace)) return 2;
        } else if (arg == "--project" || arg == "-p") {
            if (!takeValue(arguments, i, arg, kProjectHelpLine, project_root)) return 2;
        } else if (arg == "--pipe-name") {
            std::string pipe_arg;
            if (!takeValue(arguments, i, arg, kPipeNameHelpLine, pipe_arg)) return 2;
#if defined(_WIN32)
            _putenv_s("DIDI_PIPE_NAME", pipe_arg.c_str());
#else
            setenv("DIDI_PIPE_NAME", pipe_arg.c_str(), 1);
#endif
        } else if (arg == "--ui-app") {
            std::string mode;
            if (!takeValue(arguments, i, arg, kUiAppHelpLine, mode)) return 2;
            const auto parsed = didi::mcp::McpServer::parseUiAppMode(mode);
            if (!parsed.has_value()) {
                refuse("--ui-app expects auto, always, or off, not " + mode, kUiAppHelpLine);
                return 2;
            }
            ui_app_mode = *parsed;
        } else if (arg == "--yolo") {
            skip_confirmations = true;
        } else if (arg == "--log-level") {
            std::string lvl;
            if (!takeValue(arguments, i, arg, kLogLevelHelpLine, lvl)) return 2;
            if (lvl == "DEBUG") didi::Logger::instance().setLevel(didi::LogLevel::Debug);
            else if (lvl == "INFO") didi::Logger::instance().setLevel(didi::LogLevel::Info);
            else if (lvl == "WARN") didi::Logger::instance().setLevel(didi::LogLevel::Warn);
            else if (lvl == "ERROR") didi::Logger::instance().setLevel(didi::LogLevel::Error);
            else if (lvl == "NONE") didi::Logger::instance().setLevel(didi::LogLevel::None);
            else {
                refuse("--log-level expects DEBUG, INFO, WARN, ERROR, or NONE, not " + lvl,
                       kLogLevelHelpLine);
                return 2;
            }
        } else if (arg.empty()) {
            refuse("an empty argument is neither an option nor a value", kHelpHint);
            return 2;
        } else if (arg[0] == '-') {
            // A misspelled option used to be ignored, so a typo looked exactly
            // like a clean start. Refuse before anything else runs.
            refuse("unknown option " + arg, kHelpHint);
            return 2;
        } else {
            refuse("unexpected argument " + arg, kHelpHint);
            return 2;
        }
    }

    if (managed_editor.empty() != recovery_workspace.empty()) {
        refuse("--managed-editor and --recovery-workspace must be supplied together", kHelpHint);
        return 2;
    }
    auto resolved_project = didi::paths::resolveExplicitProjectRoot(project_root);
    if (resolved_project.isErr()) {
        std::cerr << "Didi startup refused: " << resolved_project.error().message << std::endl;
        return 2;
    }
    std::filesystem::path recovery_container;
    if (!managed_editor.empty()) {
        std::error_code ec;
        std::filesystem::path executable;
        // Same throw as the project root: building a path from bytes the
        // platform cannot encode escapes main and kills the process without a
        // line of output. Refuse instead.
        try {
            executable = didi::paths::projectPathFromUtf8(managed_editor);
            recovery_container = std::filesystem::absolute(
                didi::paths::projectPathFromUtf8(recovery_workspace));
        } catch (const std::exception&) {
            refuse("--managed-editor and --recovery-workspace must be valid UTF-8", kHelpHint);
            return 2;
        }
        if (!executable.is_absolute() || !std::filesystem::is_regular_file(executable, ec) || ec) {
            refuse("--managed-editor requires an absolute executable file", kHelpHint);
            return 2;
        }
        const auto initialized = didi::runtime::CheckpointStore::initialize(resolved_project.value(), recovery_container);
        if (initialized.isErr()) { refuse(initialized.error().message, kHelpHint); return 2; }
        resolved_project = recovery_container / "project";
    }
    try {
        std::filesystem::current_path(resolved_project.value());
        DIDI_LOG_INFO("MAIN", "Set working directory to explicit Godot project root: ",
                      didi::paths::projectPathToUtf8(resolved_project.value()));
    } catch (const std::exception& e) {
        DIDI_LOG_ERROR("MAIN", "Failed to change working directory to project root: ", e.what());
        return 2;
    }

    DIDI_LOG_INFO("MAIN", "Starting Didi MCP Native Server v", didi::mcp::kServerVersion,
                  " for Godot 4.5+...");

    didi::mcp::McpServer server;
    std::shared_ptr<didi::runtime::ManagedRecovery> recovery;
    if (!managed_editor.empty()) {
        recovery = std::make_shared<didi::runtime::ManagedRecovery>(recovery_container, managed_editor,
            didi::mcp::ToolRegistry::instance().getRuntimeSessionClient());
        const auto started = recovery->start();
        if (started.isErr()) { refuse(started.error().message, nullptr); return 2; }
        didi::mcp::ToolRegistry::instance().setManagedRecovery(recovery);
        DIDI_LOG_INFO("MAIN", "Managed recovery workspace: ", didi::paths::projectPathToUtf8(resolved_project.value()));
    }
    server.setUiAppMode(ui_app_mode);
    // The dashboard's log page is the only place this process's own
    // diagnostics are readable: they otherwise go to standard error, which a
    // client that launched this server over stdio usually discards.
    didi::mcp::installControlRoomLogRing();
    server.setConfirmationsSkipped(skip_confirmations);
    if (skip_confirmations) {
        // Loud, once, at startup. Someone reading a log after a bad afternoon
        // should be able to see immediately that nothing was asked.
        DIDI_LOG_WARN("MAIN",
                      "YOLO mode: confirmation is disabled. Destructive tools will execute "
                      "without review, and every affected result records confirmation: skipped.");
    }
    g_server = &server;

    server.runStdio();
    didi::mcp::ToolRegistry::instance().setManagedRecovery(nullptr);
    recovery.reset();

    DIDI_LOG_INFO("MAIN", "Didi MCP server exited cleanly.");

    // The session ended while a read of stdin was still outstanding, so the
    // reader thread is parked inside std::cin. Returning from main would run
    // static destruction and take std::cin away underneath it. The runtime
    // session is already handed back, the session lock is released, and every
    // write flushed as it was made, so there is nothing left for exit to do.
    if (server.stdinReaderStillParked()) {
        std::_Exit(0);
    }
    return 0;
}

#if defined(_WIN32) && defined(_MSC_VER)
// MSVC links wmainCRTStartup when wmain is the entry point, so the command line
// arrives as UTF-16 and nothing has been through the ANSI codepage yet. The
// guard is on the compiler rather than the platform because MinGW needs
// -municode to link a wmain, and this project builds Windows with MSVC.
int wmain(int argc, wchar_t* argv[]) {
    std::vector<std::string> arguments;
    arguments.reserve(static_cast<size_t>(argc));
    for (int i = 0; i < argc; ++i) {
        auto encoded = wideToUtf8(argv[i]);
        if (!encoded.has_value()) {
            refuse("argument " + std::to_string(i) + " cannot be encoded as UTF-8", kHelpHint);
            return 2;
        }
        arguments.push_back(std::move(*encoded));
    }
    return runDidi(arguments);
}
#else
int main(int argc, char* argv[]) {
    return runDidi(std::vector<std::string>(argv, argv + argc));
}
#endif
