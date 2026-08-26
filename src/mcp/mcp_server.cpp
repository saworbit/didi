#include "didi/mcp/mcp_server.hpp"
#include "didi/common/logger.hpp"

#if defined(_WIN32)
#include <io.h>
#include <fcntl.h>
#endif

namespace didi {
namespace mcp {

McpServer::McpServer() {
    m_ipcClient = ipc::createIpcClient();
    initializeRegistries();
}

McpServer::~McpServer() {
    stop();
}

void McpServer::initializeRegistries() {
    ToolRegistry::instance().setIpcClient(m_ipcClient);
    ToolRegistry::instance().registerAllDefaultTools();

    ResourceRegistry::instance().setIpcClient(m_ipcClient);
    ResourceRegistry::instance().registerAllDefaultResources();

    PromptRegistry::instance().registerAllDefaultPrompts();
}

void McpServer::setIpcClient(std::shared_ptr<ipc::IIpcClient> ipc_client) {
    m_ipcClient = ipc_client;
    ToolRegistry::instance().setIpcClient(m_ipcClient);
    ResourceRegistry::instance().setIpcClient(m_ipcClient);
}

std::shared_ptr<ipc::IIpcClient> McpServer::getIpcClient() const {
    return m_ipcClient;
}

void McpServer::stop() {
    m_running.store(false);
}

void McpServer::sendResponse(const JsonRpcResponse& resp) {
    std::string out = resp.serialize();
    DIDI_LOG_DEBUG("MCP_OUT", out);
    std::cout << out << "\n";
    std::cout.flush();
}

void McpServer::sendNotification(const std::string& method, const json& params) {
    json notif = {
        {"jsonrpc", "2.0"},
        {"method", method},
        {"params", params}
    };
    std::string out = notif.dump();
    DIDI_LOG_DEBUG("MCP_NOTIF", out);
    std::cout << out << "\n";
    std::cout.flush();
}

JsonRpcResponse McpServer::handleRequest(const JsonRpcRequest& req) {
    DIDI_LOG_DEBUG("MCP_REQ", "Method: ", req.method);

    if (req.method == "initialize") {
        m_initialized = true;
        json result = {
            {"protocolVersion", kProtocolVersion},
            {"capabilities", {
                {"tools", {{"listChanged", false}}},
                {"resources", {{"subscribe", false}, {"listChanged", false}}},
                {"prompts", {{"listChanged", false}}},
                {"logging", json::object()}
            }},
            {"serverInfo", {
                {"name", kServerName},
                {"version", kServerVersion}
            }}
        };
        return JsonRpcResponse::makeSuccess(req.id, result);
    }

    if (req.method == "notifications/initialized") {
        DIDI_LOG_INFO("MCP_SERVER", "Client initialized session");
        return JsonRpcResponse::makeSuccess(req.id, json::object());
    }

    if (req.method == "ping") {
        return JsonRpcResponse::makeSuccess(req.id, json::object());
    }

    // Tools
    if (req.method == "tools/list") {
        auto tools = ToolRegistry::instance().listTools();
        json tool_list = json::array();
        for (const auto& t : tools) {
            tool_list.push_back(t.toJson());
        }
        return JsonRpcResponse::makeSuccess(req.id, {{"tools", tool_list}});
    }

    if (req.method == "tools/call") {
        if (!req.params.is_object()) {
            return JsonRpcResponse::makeError(req.id, JsonRpcErrorCode::InvalidParams, "Params must be a JSON object");
        }
        std::string name = req.params.value("name", "");
        json arguments = req.params.contains("arguments") && req.params["arguments"].is_object()
                             ? req.params["arguments"]
                             : json::object();
        if (name.empty()) {
            return JsonRpcResponse::makeError(req.id, JsonRpcErrorCode::InvalidParams, "Tool name is required");
        }
        auto result = ToolRegistry::instance().callTool(name, arguments);
        return JsonRpcResponse::makeSuccess(req.id, result.toJson());
    }

    // Resources
    if (req.method == "resources/list") {
        auto resources = ResourceRegistry::instance().listResources();
        json res_list = json::array();
        for (const auto& r : resources) {
            res_list.push_back(r.toJson());
        }
        return JsonRpcResponse::makeSuccess(req.id, {{"resources", res_list}});
    }

    if (req.method == "resources/read") {
        if (!req.params.is_object()) {
            return JsonRpcResponse::makeError(req.id, JsonRpcErrorCode::InvalidParams, "Params must be a JSON object");
        }
        std::string uri = req.params.value("uri", "");
        if (uri.empty()) {
            return JsonRpcResponse::makeError(req.id, JsonRpcErrorCode::InvalidParams, "Resource URI is required");
        }
        auto read_res = ResourceRegistry::instance().readResource(uri);
        if (read_res.isErr()) {
            return JsonRpcResponse::makeError(req.id, read_res.error().code, read_res.error().message);
        }
        auto r_def = ResourceRegistry::instance().getResource(uri);
        std::string mime = r_def ? r_def->mimeType : "text/plain";
        json contents = json::array({
            {
                {"uri", uri},
                {"mimeType", mime},
                {"text", read_res.value()}
            }
        });
        return JsonRpcResponse::makeSuccess(req.id, {{"contents", contents}});
    }

    // Prompts
    if (req.method == "prompts/list") {
        auto prompts = PromptRegistry::instance().listPrompts();
        json prompt_list = json::array();
        for (const auto& p : prompts) {
            prompt_list.push_back(p.toJson());
        }
        return JsonRpcResponse::makeSuccess(req.id, {{"prompts", prompt_list}});
    }

    if (req.method == "prompts/get") {
        if (!req.params.is_object()) {
            return JsonRpcResponse::makeError(req.id, JsonRpcErrorCode::InvalidParams, "Params must be a JSON object");
        }
        std::string name = req.params.value("name", "");
        json args = req.params.contains("arguments") && req.params["arguments"].is_object()
                        ? req.params["arguments"]
                        : json::object();
        if (name.empty()) {
            return JsonRpcResponse::makeError(req.id, JsonRpcErrorCode::InvalidParams, "Prompt name is required");
        }
        auto p_res = PromptRegistry::instance().getPromptResult(name, args);
        if (p_res.isErr()) {
            return JsonRpcResponse::makeError(req.id, p_res.error().code, p_res.error().message);
        }
        return JsonRpcResponse::makeSuccess(req.id, p_res.value());
    }

    return JsonRpcResponse::makeError(req.id, JsonRpcErrorCode::MethodNotFound, "Method not found: " + req.method);
}

void McpServer::runStdio() {
    m_running.store(true);
    DIDI_LOG_INFO("MCP_SERVER", "Starting Didi MCP server over stdio...");

    // Try background initial connection to Godot GDExtension Named Pipe
    if (m_ipcClient) {
        if (m_ipcClient->connect(ipc::kDefaultPipeName, 500)) {
            DIDI_LOG_INFO("MCP_SERVER", "Connected to active Godot Editor GDExtension IPC pipe");
        } else {
            DIDI_LOG_INFO("MCP_SERVER", "Godot Editor IPC pipe not detected; running with offline fallback engine");
        }
    }

    std::string line;
    while (m_running.load() && std::getline(std::cin, line)) {
        std::string trimmed = strings::trim(line);
        if (trimmed.empty()) continue;

        // Check for HTTP-style Content-Length header
        if (strings::startsWith(trimmed, "Content-Length:") || strings::startsWith(trimmed, "content-length:")) {
            auto pos = trimmed.find(':');
            if (pos != std::string::npos) {
                try {
                    int content_len = std::stoi(strings::trim(trimmed.substr(pos + 1)));
                    if (content_len > 0 && content_len <= 128 * 1024 * 1024) {
                        // Read following empty lines until header separator
                        while (std::getline(std::cin, line)) {
                            if (strings::trim(line).empty()) break;
                        }
                        std::vector<char> buffer(content_len);
                        std::cin.read(buffer.data(), content_len);
                        trimmed = std::string(buffer.data(), buffer.size());
                    }
                } catch (const std::exception& e) {
                    DIDI_LOG_WARN("MCP_SERVER", "Invalid Content-Length header: ", e.what());
                    continue;
                }
            }
        }

        auto req_opt = JsonRpcRequest::parse(trimmed);
        if (!req_opt.has_value()) {
            DIDI_LOG_WARN("MCP_SERVER", "Malformed JSON-RPC payload received");
            sendResponse(JsonRpcResponse::makeError(nullptr, JsonRpcErrorCode::ParseError, "Parse error"));
            continue;
        }

        const auto& req = req_opt.value();
        if (req.is_notification) {
            handleRequest(req);
            continue;
        }

        auto resp = handleRequest(req);
        sendResponse(resp);
    }

    DIDI_LOG_INFO("MCP_SERVER", "Didi MCP stdio loop terminated");
}

} // namespace mcp
} // namespace didi
