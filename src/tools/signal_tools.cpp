#include "didi/mcp/mcp_protocol.hpp"
#include "didi/tools/phase7_live_forward.hpp"
#include "didi/common/ipc_channel.hpp"
#include "didi/common/logger.hpp"

#include <cmath>
#include <limits>
#include <optional>
#include <string>
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

// The rules a signal argument has to satisfy, and the sentence for the first
// one it breaks.
//
// This used to answer `false` and the caller got the identifier
// `unsupported_signal_emit_argument`, which names neither which argument, nor
// which rule, nor what the limit is (#616). `where` is the argument's position,
// carried down so a nested value says where inside the entry it sits.
std::optional<std::string> describeUnusableSignalValue(const json& value, int depth,
                                                       const std::string& where) {
    if (depth > 8) {
        return "Argument 'arguments' " + where +
               " is nested more than 8 levels deep; signal_emit carries at most 8.";
    }
    if (value.is_null() || value.is_boolean()) return std::nullopt;
    if (value.is_number_unsigned()) {
        if (value.get<uint64_t>() >
            static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
            return "Argument 'arguments' " + where +
                   " is larger than a Godot integer holds; send a whole number up to " +
                   std::to_string(std::numeric_limits<int64_t>::max()) + ".";
        }
        return std::nullopt;
    }
    if (value.is_number_integer()) return std::nullopt;
    if (value.is_number_float()) {
        if (!std::isfinite(value.get<double>())) {
            return "Argument 'arguments' " + where +
                   " is not a finite number; a signal cannot carry an infinity or a NaN.";
        }
        return std::nullopt;
    }
    if (value.is_string()) {
        if (!isBoundedUtf8String(value, 0, 4096)) {
            return "Argument 'arguments' " + where +
                   " is a string of " + std::to_string(value.get_ref<const std::string&>().size()) +
                   " bytes; the limit is 4096 per string.";
        }
        return std::nullopt;
    }
    if (value.is_array()) {
        if (value.size() > 64) {
            return "Argument 'arguments' " + where + " holds " + std::to_string(value.size()) +
                   " entries; the limit is 64 per array.";
        }
        for (size_t index = 0; index < value.size(); ++index) {
            if (auto refused = describeUnusableSignalValue(
                    value[index], depth + 1, where + "[" + std::to_string(index) + "]")) {
                return refused;
            }
        }
        return std::nullopt;
    }
    if (value.is_object()) {
        if (value.size() > 64) {
            return "Argument 'arguments' " + where + " holds " + std::to_string(value.size()) +
                   " keys; the limit is 64 per object.";
        }
        for (auto it = value.begin(); it != value.end(); ++it) {
            if (!isBoundedUtf8String(json(it.key()), 0, 4096)) {
                return "Argument 'arguments' " + where +
                       " has a key longer than 4096 bytes; the limit is 4096 per string.";
            }
            if (auto refused =
                    describeUnusableSignalValue(it.value(), depth + 1, where + "." + it.key())) {
                return refused;
            }
        }
        return std::nullopt;
    }
    return "Argument 'arguments' " + where + " is a value a signal cannot carry.";
}

} // namespace

// The argument-value rules, asked before a preview is composed as well as by
// the handler.
//
// #399 was "the dry run issued a token for arguments the real call refuses",
// and it was closed by checking argument names on the preview path. The values
// were never moved there, so a dry run signed nesting, array and object sizes
// the confirmed call then refused: two round trips and a spent token to learn
// something the first call had everything it needed to say (#616).
std::optional<Error> refuseUnusableSignalArguments(const ResolvedToolBinding& binding,
                                                   const json& arguments) {
    if (binding.policy_source != "signal_emit" || !arguments.is_object()) return std::nullopt;
    if (!arguments.contains("arguments")) return std::nullopt;
    const auto& values = arguments["arguments"];
    if (!values.is_array()) return std::nullopt;
    if (values.size() > 16) {
        return Error(400, "Argument 'arguments' carries " + std::to_string(values.size()) +
                              " values; signal_emit accepts at most 16.");
    }
    for (size_t index = 0; index < values.size(); ++index) {
        if (auto refused = describeUnusableSignalValue(values[index], 0,
                                                       "entry " + std::to_string(index))) {
            return Error(400, *refused);
        }
    }
    try {
        const auto serialised = values.dump().size();
        if (serialised > 32u * 1024u) {
            return Error(413, "Argument 'arguments' serialises to " +
                                  std::to_string(serialised) + " bytes; the limit is 32768.");
        }
    } catch (const json::exception&) {
        return Error(400, "Argument 'arguments' holds text that is not valid UTF-8.");
    }
    return std::nullopt;
}

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
    // The same rules the preview path runs, so the two cannot disagree about
    // what this call accepts.
    if (auto refused = refuseUnusableSignalArguments(binding, normalized)) {
        return signalRequestError(binding, refused->code, refused->message);
    }
    return sendPhase7LiveRequest(binding, normalized, ipc);
}

} // namespace mcp
} // namespace didi
