#pragma once

#include <string>
#include <vector>
#include <functional>
#include "didi/common/types.hpp"
#include "didi/common/json.hpp"

namespace didi {
namespace mcp {

inline const char* kProtocolVersion = "2024-11-05";
inline const char* kServerName = "didi";
inline const char* kServerVersion = "1.1.0";

struct ContentItem {
    std::string type; // "text" or "image" or "resource"
    std::string text;
    std::string data;     // base64 encoded for image
    std::string mimeType; // e.g. "image/png"

    json toJson() const {
        if (type == "image") {
            return {
                {"type", "image"},
                {"data", data},
                {"mimeType", mimeType.empty() ? "image/png" : mimeType}
            };
        } else if (type == "resource") {
            return {
                {"type", "resource"},
                {"resource", {{"uri", text}, {"mimeType", mimeType}}}
            };
        }
        return {
            {"type", "text"},
            {"text", text}
        };
    }

    static ContentItem makeText(std::string text) {
        ContentItem item;
        item.type = "text";
        item.text = std::move(text);
        return item;
    }

    static ContentItem makeImagePng(std::string base64_png) {
        ContentItem item;
        item.type = "image";
        item.data = std::move(base64_png);
        item.mimeType = "image/png";
        return item;
    }
};

struct CallToolResult {
    std::vector<ContentItem> content;
    bool isError{false};

    json toJson() const {
        json j;
        json arr = json::array();
        for (const auto& item : content) {
            arr.push_back(item.toJson());
        }
        j["content"] = arr;
        j["isError"] = isError;
        return j;
    }

    static CallToolResult success(std::string text) {
        CallToolResult res;
        res.content.push_back(ContentItem::makeText(std::move(text)));
        res.isError = false;
        return res;
    }

    static CallToolResult successJson(const json& data) {
        return success(data.dump(2));
    }

    static CallToolResult successImage(std::string base64_png, std::string text_desc = "") {
        CallToolResult res;
        if (!text_desc.empty()) {
            res.content.push_back(ContentItem::makeText(std::move(text_desc)));
        }
        res.content.push_back(ContentItem::makeImagePng(std::move(base64_png)));
        res.isError = false;
        return res;
    }

    static CallToolResult error(std::string err_msg) {
        CallToolResult res;
        res.content.push_back(ContentItem::makeText(std::move(err_msg)));
        res.isError = true;
        return res;
    }
};

using ToolHandler = std::function<CallToolResult(const json& arguments)>;

struct ExecutionCapability {
    std::vector<std::string> modes{"unimplemented"};
    bool implemented{false};
    std::string reason;

    json toJson() const {
        json data = {
            {"executionModes", modes},
            {"implemented", implemented}
        };
        if (!reason.empty()) {
            data["reason"] = reason;
        }
        return data;
    }
};

struct ToolDefinition {
    std::string name;
    std::string description;
    json inputSchema;
    ToolHandler handler;
    ExecutionCapability capability;

    json toJson() const {
        return {
            {"name", name},
            {"description", description},
            {"inputSchema", inputSchema},
            {"_meta", {{"didi", capability.toJson()}}}
        };
    }
};

struct ResourceDefinition {
    std::string uri;
    std::string name;
    std::string description;
    std::string mimeType;
    std::function<Result<std::string>()> readHandler;
    ExecutionCapability capability;

    json toJson() const {
        return {
            {"uri", uri},
            {"name", name},
            {"description", description},
            {"mimeType", mimeType},
            {"_meta", {{"didi", capability.toJson()}}}
        };
    }
};

struct PromptArgument {
    std::string name;
    std::string description;
    bool required{false};

    json toJson() const {
        return {
            {"name", name},
            {"description", description},
            {"required", required}
        };
    }
};

struct PromptDefinition {
    std::string name;
    std::string description;
    std::vector<PromptArgument> arguments;
    std::function<Result<json>(const json& args)> getHandler;

    json toJson() const {
        json j;
        j["name"] = name;
        j["description"] = description;
        json args_arr = json::array();
        for (const auto& a : arguments) {
            args_arr.push_back(a.toJson());
        }
        j["arguments"] = args_arr;
        return j;
    }
};

} // namespace mcp
} // namespace didi
