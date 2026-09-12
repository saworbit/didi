#pragma once

#include <algorithm>
#include <array>
#include <optional>
#include <string>
#include <vector>
#include <functional>
#include "didi/common/version.hpp"
#include "didi/common/types.hpp"
#include "didi/common/json.hpp"

namespace didi {
namespace mcp {

struct ResolvedToolBinding;

inline const char* kProtocolVersion = "2024-11-05";
inline const char* kServerName = "didi";
inline const char* kServerVersion = kProjectVersion;

// MCP Apps (the io.modelcontextprotocol/ui extension, revision 2026-01-26).
// Extensions are bilateral: this server declares support unconditionally, and
// advertises the UI surface itself only to a client that declared it too.
// See docs/CONTROL_ROOM_DESIGN.md.
inline const char* kUiExtensionName = "io.modelcontextprotocol/ui";
inline const char* kUiAppMimeType = "text/html;profile=mcp-app";
inline const char* kControlRoomResourceUri = "ui://didi/control-room";
inline const char* kControlRoomToolName = "didi_control_room";

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

// Where a modern client names the runtime session it means.
//
// The modern revision says a stdio process is not a conversation, and that
// state spanning requests must be referenced by an explicit identifier the
// client passes on each request. A Godot session is exactly that kind of
// state: attaching one set a process-wide route that every later request
// inherited, so a task could read from or mutate an editor it never selected.
// Results already carry a `didi` block, so this is the request side of a key
// that exists.
inline const char* kDidiMetaKey = "didi";
inline const char* kRuntimeSessionMetaKey = "runtime_session_id";

// Which protocol revision a single request belongs to. Didi is dual-era: a
// legacy client negotiates once in `initialize` and the process remembers what
// it declared, while a modern client carries version and capabilities in
// `_meta` on every request and is entitled to have nothing inferred from
// earlier traffic on the same process. Requests of both kinds can be
// interleaved on one stdio process, so this is a property of the request and
// never of the server.
enum class ProtocolEra { Legacy, Modern };

// What one request selected, carried from the protocol boundary down to
// dispatch so that no layer has to guess.
struct RequestScope {
    ProtocolEra era{ProtocolEra::Legacy};
    // The session this request named, if it named one.
    std::optional<std::string> runtime_session_id;

    bool selectsRuntimeSession() const { return runtime_session_id.has_value(); }

    // A legacy request inherits whatever route the handshake era attached, the
    // way it always has. A modern request may only ever be served on the
    // session it named for itself.
    bool mayInheritActiveRoute() const { return era == ProtocolEra::Legacy; }

    // The default is a legacy request, so every existing caller and every test
    // that calls a tool directly keeps the behaviour it had.
    static RequestScope legacy() { return RequestScope{}; }
};

inline const char* kModernProtocolVersion = "2026-07-28";

// A revision belongs here only once Didi serves its result shapes. The modern
// revision joined when every result gained `resultType` and the cacheable
// operations gained freshness hints -- the two move together, or the
// advertisement is a claim nobody checked.
inline json supportedProtocolVersions() {
    return json::array({kModernProtocolVersion, kProtocolVersion});
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

    // Compact, not pretty. This text is read by a model, and indentation is
    // billed as tokens on every tool call for no benefit. structuredContent
    // carries the same data for clients that parse rather than read.
    static CallToolResult successJson(const json& data) {
        auto result = success(data.dump());
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

    // The envelope the rest of the surface answers a failure with. Most tools
    // built one of these by hand; eighteen returned the message as a bare JSON
    // string with no code, so a client that switches on error.code -- the
    // documented way to tell retryable from not -- got undefined and had to
    // fall back to substring-matching English prose (#420).
    //
    // The prose is the good part and is kept as it was. Only the wrapper is
    // new. `code` is the HTTP-shaped status this server already uses elsewhere
    // for the same situation: 400 for a bad argument, 404 for something that is
    // not there, 409 for a conflict with what is, 501 for a mode that is off.
    // An Error already carries the code and the message. Answering with only
    // its message threw the code away at 68 call sites, which is how a 404 and
    // a 409 both reached the client as prose with nothing to branch on.
    static CallToolResult fromError(const Error& error) {
        json data = error.data.is_object() ? error.data : json::object();
        return errorJson(error.code, error.message, std::move(data));
    }

    // The same envelope, keeping the sentence a tool puts in front of a helper's
    // message. A path validator says "file does not exist beneath the project
    // root"; the tool says which argument it was reading. Prefixing the message
    // by hand and passing the result to error() is what threw the code away at
    // the eight path-validation sites in #460.
    static CallToolResult fromError(const Error& error, const std::string& prefix) {
        json data = error.data.is_object() ? error.data : json::object();
        return errorJson(error.code, prefix + error.message, std::move(data));
    }

    static CallToolResult errorJson(int code, std::string message, json data = json::object()) {
        if (!data.is_object()) data = json::object();
        if (!data.contains("retryable")) data["retryable"] = false;
        auto res = error(json{{"error", {{"code", code},
                                         {"message", std::move(message)},
                                         {"data", std::move(data)}}}}.dump());
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
    // What this registration calls its own non-engine work on the wire.
    // "offline_fallback" is the routing vocabulary and stays in `modes`, because
    // the registry uses it to decide whether a call can run with no session
    // attached. It is the wrong word to publish for something with no live path:
    // there is nothing to fall back from, and a caller reading it as a quality
    // signal is told to reattach an editor that would change nothing (#419,
    // #503). Tools set this in capabilityForTool -- most say "local", while the
    // session tools and the control room already say something more specific in
    // their own answers, and this is where those names live so the answer and
    // the advertisement come from one place.
    //
    // The default is the old word rather than "local" because resources still
    // report "offline_fallback" from their own read handlers. Their
    // advertisement and their answers agree today, and defaulting to "local"
    // here would have moved one without the other. Sweeping the resource half
    // is its own change.
    std::string local_mode{"offline_fallback"};

    // The mode a tool reports when it is not running live.
    const std::string& localMode() const { return local_mode; }

    // True when an engine can contribute to this tool at all.
    bool supportsLive() const {
        return std::find(modes.begin(), modes.end(), "live") != modes.end();
    }

    // executionModes as published, rather than as routed. A tool with no live
    // path advertises the name it will actually answer with.
    std::vector<std::string> publishedModes() const {
        if (supportsLive()) return modes;
        std::vector<std::string> published;
        published.reserve(modes.size());
        for (const auto& mode : modes) {
            published.push_back(mode == "offline_fallback" ? local_mode : mode);
        }
        return published;
    }

    json toJson() const {
        json data = {
            {"executionModes", publishedModes()},
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
    // True for tools that start a subprocess against the project. A local
    // working directory does not make the code being run closed: Godot runs the
    // project's scripts, extensions and export plugins, and dotnet build can
    // restore packages and run custom targets. Derived from the resolved
    // binding, never hand-set. See toolRunsProjectControlledCode.
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
    // The name this one resolves to. The same as `name` for the 116 canonical
    // registrations. Set by ToolRegistry::registerTool. Never set by hand.
    std::string canonical_name;
    // Set by ToolRegistry::registerTool from MutationSafety. Never set by hand.
    ToolAnnotations annotations;
    // Optional. Declared only for tools whose real result shape is known; see
    // outputSchemaForTool. Absent means no promise is made about the payload.
    json outputSchema;

    json toJson() const {
        // Ten of the 126 registrations are legacy names for a tool that is also
        // listed under its own. They published identical schemas, identical
        // descriptions and identical metadata, so nothing an MCP client reads
        // said they were duplicates: which of the two an agent picked was a coin
        // flip, error data named a `canonical_tool` the caller had never heard
        // of, and any inventory of the surface double-counted seven
        // capabilities. didi_control_room knew all along (#493).
        //
        // `legacy` is stated on every tool rather than only on the ten, because
        // "this is not an alias" is a fact a client should be able to read
        // rather than infer from a missing key.
        json meta = capability.toJson();
        meta["legacy"] = legacy;
        if (!canonical_name.empty() && canonical_name != name) {
            meta["canonical"] = canonical_name;
        }
        json definition = {
            {"name", name},
            {"description", description},
            {"inputSchema", inputSchema},
            {"annotations", annotations.toJson()},
            {"_meta", {{"didi", std::move(meta)}}}
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
    // The io.modelcontextprotocol/ui block for a UI resource: content security
    // policy domains, requested permissions, framing preference. Empty for
    // every ordinary resource, and then no `ui` key is emitted at all.
    json uiMeta;

    json toJson() const {
        json meta = {{"didi", capability.toJson()}};
        if (uiMeta.is_object() && !uiMeta.empty()) meta["ui"] = uiMeta;
        return {
            {"uri", uri},
            {"name", name},
            {"description", description},
            {"mimeType", mimeType},
            {"_meta", meta}
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
