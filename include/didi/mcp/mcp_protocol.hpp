#pragma once

#include <array>
#include <optional>
#include <string>
#include <vector>
#include <functional>
#include "didi/common/types.hpp"
#include "didi/common/json.hpp"

namespace didi {
namespace mcp {

struct ResolvedToolBinding;

inline const char* kProtocolVersion = "2024-11-05";
inline const char* kServerName = "didi";
inline const char* kServerVersion = "1.4.0";

// Revision 2026-07-28 removed the initialize handshake: a modern client
// declares its protocol version in `_meta` on every request, and servers must
// implement `server/discover`. Didi still serves legacy result shapes, so it
// advertises only the legacy revision -- but answering discover lets a modern
// client fail deterministically with an actionable list rather than meeting
// silence, which is what the specification tells stdio clients to probe for.
//
// A revision joins kSupportedProtocolVersions when Didi actually serves it,
// not when it can name it.
inline const char* kProtocolVersionMetaKey = "io.modelcontextprotocol/protocolVersion";
inline const char* kServerInfoMetaKey = "io.modelcontextprotocol/serverInfo";
inline constexpr int kUnsupportedProtocolVersionCode = -32022;

inline json supportedProtocolVersions() {
    return json::array({kProtocolVersion});
}

inline bool isSupportedProtocolVersion(const std::string& version) {
    for (const auto& supported : supportedProtocolVersions()) {
        if (supported.get<std::string>() == version) return true;
    }
    return false;
}

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
    // Server-produced result data. Emitted alongside the text block, never
    // instead of it, so clients that do not read structuredContent are
    // unaffected.
    std::optional<json> structuredContent;

    json toJson() const {
        json j;
        json arr = json::array();
        for (const auto& item : content) {
            arr.push_back(item.toJson());
        }
        j["content"] = arr;
        j["isError"] = isError;
        if (structuredContent.has_value() && !isError) {
            j["structuredContent"] = structuredContent.value();
        }
        return j;
    }

    static CallToolResult success(std::string text) {
        CallToolResult res;
        res.content.push_back(ContentItem::makeText(std::move(text)));
        res.isError = false;
        return res;
    }

    static CallToolResult successJson(const json& data) {
        auto result = success(data.dump(2));
        result.structuredContent = data;
        return result;
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
using BoundToolHandler =
    std::function<CallToolResult(const ResolvedToolBinding&, const json& arguments)>;

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

// The v1.0 tool names retained only for protocol compatibility. This array is
// the single source of truth for which registrations are legacy: the registry
// marks tools from it, tests assert the registry agrees with it, and the
// generated tool manifest derives the legacy count from it. Prefer the
// canonical names in new integrations.
inline constexpr std::array<const char*, 10> kLegacyToolNames{
    "analyze_script_diagnostics",
    "capture_viewport",
    "create_visual_test_lab",
    "execute_test_session",
    "get_scene_hierarchy",
    "inject_input_event",
    "instantiate_asset",
    "mutate_scene_tree",
    "patch_script_symbols",
    "query_project_resources"
};

inline bool isLegacyToolName(const std::string& name) {
    for (const char* legacy : kLegacyToolNames) {
        if (name == legacy) return true;
    }
    return false;
}

// Specification tool annotations. Clients use these to decide what may be
// auto-approved, so they are derived in registerTool from the same
// MutationSafety classification that drives dry-run and confirmation. They
// cannot be set by hand and cannot drift from that contract.
//
// The defaults are the conservative direction: not read-only, and destructive.
// Under-claiming safety costs a client prompt; over-claiming it would let a
// mutation be auto-approved.
struct ToolAnnotations {
    bool read_only{false};
    bool destructive{true};
    bool idempotent{false};
    // Didi's world is one local Godot project. No tool reaches the network.
    bool open_world{false};

    json toJson() const {
        return {
            {"readOnlyHint", read_only},
            {"destructiveHint", destructive},
            {"idempotentHint", idempotent},
            {"openWorldHint", open_world}
        };
    }
};

struct ToolDefinition {
    std::string name;
    std::string description;
    json inputSchema;
    ToolHandler handler;
    BoundToolHandler boundHandler;
    ExecutionCapability capability;
    // Set by ToolRegistry::registerTool from kLegacyToolNames. Never set by hand.
    bool legacy{false};
    // Set by ToolRegistry::registerTool from MutationSafety. Never set by hand.
    ToolAnnotations annotations;
    // Optional. Declared only for tools whose real result shape is known; see
    // outputSchemaForTool. Absent means no promise is made about the payload.
    json outputSchema;

    json toJson() const {
        json definition = {
            {"name", name},
            {"description", description},
            {"inputSchema", inputSchema},
            {"annotations", annotations.toJson()},
            {"_meta", {{"didi", capability.toJson()}}}
        };
        if (outputSchema.is_object() && !outputSchema.empty()) {
            definition["outputSchema"] = outputSchema;
        }
        return definition;
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
