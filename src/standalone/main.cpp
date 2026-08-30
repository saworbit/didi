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
                      << "  -p, --project <dir>   Set Godot project root directory (or use DIDI_PROJECT_ROOT)\n"
                      << "  --pipe-name <name>    Override Named Pipe / Unix domain socket name (or DIDI_PIPE_NAME)\n"
                      << "  --log-level <level>   Set log level (DEBUG, INFO, WARN, ERROR, NONE)\n"
                      << "  --dump-tool-manifest  Print the registered tool surface as JSON and exit\n"
                      << "  --yolo                Skip confirmation on destructive tools (or DIDI_YOLO=1)\n"
                      << "                        For unattended runs. Mutations execute without review,\n"
                      << "                        and each affected result records confirmation: skipped.\n"
                      << "\n"
                      << "MCP Protocol:\n"
                      << "  Communicates over standard I/O (JSON-RPC 2.0) with AI coding assistants.\n"
                      << "  Connects to Godot 4.5+ editor via native named pipes.\n";
            return 0;
        } else if ((arg == "--project" || arg == "-p") && i + 1 < argc) {
            project_root = argv[++i];
        } else if (arg == "--pipe-name" && i + 1 < argc) {
            std::string pipe_arg = argv[++i];
#if defined(_WIN32)
            _putenv_s("DIDI_PIPE_NAME", pipe_arg.c_str());
#else
            setenv("DIDI_PIPE_NAME", pipe_arg.c_str(), 1);
#endif
        } else if (arg == "--yolo") {
            skip_confirmations = true;
        } else if (arg == "--log-level" && i + 1 < argc) {
            std::string lvl = argv[++i];
            if (lvl == "DEBUG") didi::Logger::instance().setLevel(didi::LogLevel::Debug);
            else if (lvl == "INFO") didi::Logger::instance().setLevel(didi::LogLevel::Info);
            else if (lvl == "WARN") didi::Logger::instance().setLevel(didi::LogLevel::Warn);
            else if (lvl == "ERROR") didi::Logger::instance().setLevel(didi::LogLevel::Error);
            else if (lvl == "NONE") didi::Logger::instance().setLevel(didi::LogLevel::None);
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
