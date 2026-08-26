#include "didi/mcp/mcp_server.hpp"
#include "didi/common/logger.hpp"
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

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--version" || arg == "-v") {
            std::cout << "didi (godot-mcp-native) v1.0.0" << std::endl;
            return 0;
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Didi - Native Model Context Protocol (MCP) Server for Godot 4.x\n\n"
                      << "Usage:\n"
                      << "  didi [options]\n\n"
                      << "Options:\n"
                      << "  -v, --version         Show version information\n"
                      << "  -h, --help            Show this help dialog\n"
                      << "  --log-level <level>   Set log level (DEBUG, INFO, WARN, ERROR, NONE)\n"
                      << "\n"
                      << "MCP Protocol:\n"
                      << "  Communicates over standard I/O (JSON-RPC 2.0) with AI coding assistants.\n"
                      << "  Connects to Godot 4.x editor via native named pipes.\n";
            return 0;
        } else if (arg == "--log-level" && i + 1 < argc) {
            std::string lvl = argv[++i];
            if (lvl == "DEBUG") didi::Logger::instance().setLevel(didi::LogLevel::Debug);
            else if (lvl == "INFO") didi::Logger::instance().setLevel(didi::LogLevel::Info);
            else if (lvl == "WARN") didi::Logger::instance().setLevel(didi::LogLevel::Warn);
            else if (lvl == "ERROR") didi::Logger::instance().setLevel(didi::LogLevel::Error);
            else if (lvl == "NONE") didi::Logger::instance().setLevel(didi::LogLevel::None);
        }
    }

    DIDI_LOG_INFO("MAIN", "Starting Didi MCP Native Server v1.0.0 for Godot 4.x...");

    didi::mcp::McpServer server;
    g_server = &server;

    server.runStdio();

    DIDI_LOG_INFO("MAIN", "Didi MCP server exited cleanly.");
    return 0;
}
