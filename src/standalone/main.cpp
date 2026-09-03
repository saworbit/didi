#include "didi/mcp/mcp_server.hpp"
#include "didi/mcp/tool_registry.hpp"
#include "didi/common/logger.hpp"
#include "didi/common/project_path.hpp"
#include <iostream>
#include <csignal>

#include <atomic>

static didi::mcp::McpServer* g_server = nullptr;
static std::atomic<bool> g_stopRequested{false};

void signalHandler(int sig) {
    (void)sig;
    g_stopRequested.store(true);
    if (g_server) {
        g_server->stop();
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
static const char* kHelpHint = "Run didi --help for the supported options.";

static void refuse(const std::string& message, const char* help_line) {
    std::cerr << "Didi startup refused: " << message << std::endl;
    if (help_line) std::cerr << help_line << std::endl;
}

// A value-taking option must actually be given a value, and that value must not
// be another option. Without this check `--log-level --yolo` swallows the flag,
// so a launch that asked for YOLO mode starts without it and says nothing.
static bool takeValue(int argc, char* argv[], int& index, const std::string& option,
                      const char* help_line, std::string& out) {
    if (index + 1 >= argc) {
        refuse(option + " expects a value", help_line);
        return false;
    }
    const std::string value = argv[index + 1];
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

int main(int argc, char* argv[]) {
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    std::string project_root;
    const char* env_root = std::getenv("DIDI_PROJECT_ROOT");
    if (env_root && std::strlen(env_root) > 0) {
        project_root = env_root;
    }

    // Confirmations can only be turned off from here -- the launch arguments,
    // chosen by the person starting the process. Nothing reachable from a tool
    // call may set this.
    bool skip_confirmations = false;
    if (const char* env_yolo = std::getenv("DIDI_YOLO")) {
        const std::string value = env_yolo;
        skip_confirmations = value == "1" || value == "true" || value == "TRUE";
    }

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--version" || arg == "-v") {
            std::cout << "didi (godot-mcp-native) v1.4.0" << std::endl;
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
                      << "  --dump-tool-manifest  Print the registered tool surface as JSON and exit\n"
                      << "  --yolo                Skip confirmation on destructive tools (or DIDI_YOLO=1)\n"
                      << "                        For unattended runs. Mutations execute without review,\n"
                      << "                        and each affected result records confirmation: skipped.\n"
                      << "\n"
                      << "MCP Protocol:\n"
                      << "  Communicates over standard I/O (JSON-RPC 2.0) with AI coding assistants.\n"
                      << "  Connects to Godot 4.5+ editor via native named pipes.\n";
            return 0;
        } else if (arg == "--project" || arg == "-p") {
            if (!takeValue(argc, argv, i, arg, kProjectHelpLine, project_root)) return 2;
        } else if (arg == "--pipe-name") {
            std::string pipe_arg;
            if (!takeValue(argc, argv, i, arg, kPipeNameHelpLine, pipe_arg)) return 2;
#if defined(_WIN32)
            _putenv_s("DIDI_PIPE_NAME", pipe_arg.c_str());
#else
            setenv("DIDI_PIPE_NAME", pipe_arg.c_str(), 1);
#endif
        } else if (arg == "--yolo") {
            skip_confirmations = true;
        } else if (arg == "--log-level") {
            std::string lvl;
            if (!takeValue(argc, argv, i, arg, kLogLevelHelpLine, lvl)) return 2;
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

    const auto resolved_project = didi::paths::resolveExplicitProjectRoot(project_root);
    if (resolved_project.isErr()) {
        std::cerr << "Didi startup refused: " << resolved_project.error().message << std::endl;
        return 2;
    }
    try {
        std::filesystem::current_path(resolved_project.value());
        DIDI_LOG_INFO("MAIN", "Set working directory to explicit Godot project root: ",
                      didi::paths::projectPathToUtf8(resolved_project.value()));
    } catch (const std::exception& e) {
        DIDI_LOG_ERROR("MAIN", "Failed to change working directory to project root: ", e.what());
        return 2;
    }

    DIDI_LOG_INFO("MAIN", "Starting Didi MCP Native Server v1.4.0 for Godot 4.5+...");

    didi::mcp::McpServer server;
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

    DIDI_LOG_INFO("MAIN", "Didi MCP server exited cleanly.");
    return 0;
}
