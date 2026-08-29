#include "didi/mcp/mcp_protocol.hpp"
#include "didi/tools/phase7_live_forward.hpp"
#include "didi/common/ipc_channel.hpp"
#include "didi/common/logger.hpp"

#include <cmath>
#include <limits>
#include <string_view>

namespace didi {
namespace mcp {
namespace {

CallToolResult signalRequestError(const ResolvedToolBinding& binding, int code,
                                  std::string_view message) {
    return CallToolResult::error(json{{"error", {
        {"code", code}, {"message", message},
        {"data", {{"tool", binding.invoked_name},
                  {"canonical_tool", binding.canonical_name},
                  {"retryable", false}}}}}}.dump());
}

CallToolResult invalidSignalRequest(const ResolvedToolBinding& binding,
                                    std::string_view message) {
    return signalRequestError(binding, 400, message);
}

bool hasOnlySignalKeys(const json& value,
                       std::initializer_list<std::string_view> allowed) {
    if (!value.is_object()) return false;
    for (auto it = value.begin(); it != value.end(); ++it) {
        bool found = false;
        for (const auto key : allowed) {
            if (it.key() == key) {
                found = true;
                break;
            }
        }
        if (!found) return false;
    }
    return true;
}

bool isBoundedUtf8String(const json& value, size_t minimum, size_t maximum) {
    if (!value.is_string()) return false;
    const auto& text = value.get_ref<const std::string&>();
    if (text.size() < minimum || text.size() > maximum) return false;
    try {
        (void)json(text).dump();
        return true;
    } catch (const json::exception&) {
        return false;
    }
}

bool validateRelationshipRequest(const json& args, bool allow_flags) {
    if (!hasOnlySignalKeys(args,
            allow_flags
                ? std::initializer_list<std::string_view>{
                      "emitter_node", "signal_name", "target_node", "target_method", "flags"}
                : std::initializer_list<std::string_view>{
                      "emitter_node", "signal_name", "target_node", "target_method"})) {
        return false;
    }
    if (!args.contains("emitter_node") ||
        !isBoundedUtf8String(args["emitter_node"], 1, 1024) ||
        !args.contains("signal_name") ||
        !isBoundedUtf8String(args["signal_name"], 1, 128) ||
        !args.contains("target_node") ||
        !isBoundedUtf8String(args["target_node"], 1, 1024) ||
        !args.contains("target_method") ||
        !isBoundedUtf8String(args["target_method"], 1, 128)) {
        return false;
    }
    if (allow_flags && args.contains("flags")) {
        if (!(args["flags"].is_number_integer() || args["flags"].is_number_unsigned()) ||
            args["flags"] != 2) {
            return false;
        }
    }
    return true;
}

bool validateSignalJsonValue(const json& value, int depth) {
    if (depth > 8) return false;
    if (value.is_null() || value.is_boolean()) return true;
    if (value.is_number_unsigned()) {
        return value.get<uint64_t>() <=
               static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
    }
    if (value.is_number_integer()) return true;
    if (value.is_number_float()) return std::isfinite(value.get<double>());
    if (value.is_string()) return isBoundedUtf8String(value, 0, 4096);
    if (value.is_array()) {
        if (value.size() > 64) return false;
        for (const auto& element : value) {
            if (!validateSignalJsonValue(element, depth + 1)) return false;
        }
        return true;
    }
    if (value.is_object()) {
        if (value.size() > 64) return false;
        for (auto it = value.begin(); it != value.end(); ++it) {
            if (!isBoundedUtf8String(json(it.key()), 0, 4096) ||
                !validateSignalJsonValue(it.value(), depth + 1)) {
                return false;
            }
        }
        return true;
    }
    return false;
}

} // namespace

CallToolResult handleSignalListConnections(const ResolvedToolBinding& binding, const json& args,
                         std::shared_ptr<ipc::IIpcClient> ipc) {
    if (!hasOnlySignalKeys(args, {"target_node"}) ||
        !args.contains("target_node") ||
        !isBoundedUtf8String(args["target_node"], 1, 1024)) {
        return invalidSignalRequest(binding, "invalid_signal_list_connections_request");
    }
    return sendPhase7LiveRequest(binding, args, ipc);
}

CallToolResult handleSignalConnect(const ResolvedToolBinding& binding, const json& args,
                         std::shared_ptr<ipc::IIpcClient> ipc) {
    if (!validateRelationshipRequest(args, true)) {
        return invalidSignalRequest(binding, "invalid_signal_connect_request");
    }
    auto normalized = args;
    normalized["flags"] = 2;
    return sendPhase7LiveRequest(binding, normalized, ipc);
}

CallToolResult handleSignalDisconnect(const ResolvedToolBinding& binding, const json& args,
                         std::shared_ptr<ipc::IIpcClient> ipc) {
    if (!validateRelationshipRequest(args, false)) {
        return invalidSignalRequest(binding, "invalid_signal_disconnect_request");
    }
    return sendPhase7LiveRequest(binding, args, ipc);
}

CallToolResult handleSignalEmit(const ResolvedToolBinding& binding, const json& args,
                         std::shared_ptr<ipc::IIpcClient> ipc) {
    if (!hasOnlySignalKeys(args, {"target_node", "signal_name", "arguments"}) ||
        !args.contains("target_node") ||
        !isBoundedUtf8String(args["target_node"], 1, 1024) ||
        !args.contains("signal_name") ||
        !isBoundedUtf8String(args["signal_name"], 1, 128) ||
        (args.contains("arguments") && !args["arguments"].is_array())) {
        return invalidSignalRequest(binding, "invalid_signal_emit_request");
    }
    auto normalized = args;
    if (!normalized.contains("arguments")) normalized["arguments"] = json::array();
    if (normalized["arguments"].size() > 16) {
        return invalidSignalRequest(binding, "signal_emit_argument_count_exceeded");
    }
    for (const auto& argument : normalized["arguments"]) {
        if (!validateSignalJsonValue(argument, 0)) {
            return invalidSignalRequest(binding, "unsupported_signal_emit_argument");
        }
    }
    try {
        if (normalized["arguments"].dump().size() > 32u * 1024u) {
            return signalRequestError(binding, 413, "signal_emit_arguments_too_large");
        }
    } catch (const json::exception&) {
        return invalidSignalRequest(binding, "invalid_signal_emit_argument_encoding");
    }
    return sendPhase7LiveRequest(binding, normalized, ipc);
}

} // namespace mcp
} // namespace didi
