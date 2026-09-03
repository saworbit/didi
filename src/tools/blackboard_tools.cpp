#include "didi/mcp/mcp_protocol.hpp"
#include "didi/common/ipc_channel.hpp"
#include "didi/offline/blackboard.hpp"

#include <memory>
#include <string>

namespace didi {
namespace mcp {

namespace {

// Board content is written by whatever called the tool. It is data, never an
// instruction, and nothing here interprets it: values go in and come back out
// verbatim. The only judgments made are about shape and size.
struct ArgumentReader {
    const json& args;
    std::string failure;

    bool ok() const { return failure.empty(); }

    std::string string(const char* key, const std::string& fallback = {},
                       size_t max_bytes = 512) {
        if (!args.contains(key) || args[key].is_null()) return fallback;
        if (!args[key].is_string()) {
            failure = std::string(key) + " must be a string";
            return fallback;
        }
        auto value = args[key].get<std::string>();
        if (value.size() > max_bytes) {
            failure = std::string(key) + " must be at most " + std::to_string(max_bytes) + " bytes";
            return fallback;
        }
        return value;
    }

    bool boolean(const char* key, bool fallback) {
        if (!args.contains(key) || args[key].is_null()) return fallback;
        if (!args[key].is_boolean()) {
            failure = std::string(key) + " must be a boolean";
            return fallback;
        }
        return args[key].get<bool>();
    }

    int64_t integer(const char* key, int64_t fallback, int64_t low, int64_t high) {
        if (!args.contains(key) || args[key].is_null()) return fallback;
        if (!args[key].is_number_integer()) {
            failure = std::string(key) + " must be an integer";
            return fallback;
        }
        const auto value = args[key].get<int64_t>();
        if (value < low || value > high) {
            failure = std::string(key) + " must be between " + std::to_string(low) + " and " +
                      std::to_string(high);
            return fallback;
        }
        return value;
    }
};

CallToolResult finish(const Result<json>& outcome) {
    if (outcome.isErr()) return CallToolResult::error(outcome.error().message);
    return CallToolResult::successJson(outcome.value());
}

} // namespace

CallToolResult handleBlackboardWrite(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    (void)ipc;
    if (!args.is_object()) return CallToolResult::error("arguments must be an object");
    ArgumentReader reader{args, {}};

    offline::BlackboardWriteRequest request;
    request.board = reader.string("board", "default", offline::kBlackboardMaxBoardNameBytes);
    request.path = reader.string("path", {}, offline::kBlackboardMaxPathBytes);
    if (!reader.ok()) return CallToolResult::error(reader.failure);
    if (request.path.empty()) return CallToolResult::error("path is required");
    if (!args.contains("value")) return CallToolResult::error("value is required");
    request.value = args["value"];

    if (args.contains("author") && !args["author"].is_null()) {
        request.author = reader.string("author", {}, 128);
    }
    if (args.contains("reason") && !args["reason"].is_null()) {
        request.reason = reader.string("reason", {}, 512);
    }
    if (args.contains("ttl_seconds") && !args["ttl_seconds"].is_null()) {
        request.ttl_seconds = reader.integer("ttl_seconds", 0, 1, offline::kBlackboardMaxTtlSeconds);
    }
    if (!reader.ok()) return CallToolResult::error(reader.failure);

    return finish(offline::blackboardWrite(request));
}

CallToolResult handleBlackboardRead(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    (void)ipc;
    if (!args.is_object()) return CallToolResult::error("arguments must be an object");
    ArgumentReader reader{args, {}};

    offline::BlackboardReadRequest request;
    request.board = reader.string("board", "default", offline::kBlackboardMaxBoardNameBytes);
    request.path = reader.string("path", {}, offline::kBlackboardMaxPathBytes);
    request.deep = reader.boolean("deep", true);
    request.include_metadata = reader.boolean("include_metadata", false);
    if (!reader.ok()) return CallToolResult::error(reader.failure);

    return finish(offline::blackboardRead(request));
}

CallToolResult handleBlackboardPatch(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    (void)ipc;
    if (!args.is_object()) return CallToolResult::error("arguments must be an object");
    ArgumentReader reader{args, {}};

    offline::BlackboardPatchRequest request;
    request.board = reader.string("board", "default", offline::kBlackboardMaxBoardNameBytes);
    if (!reader.ok()) return CallToolResult::error(reader.failure);
    if (!args.contains("operations")) return CallToolResult::error("operations is required");
    if (!args["operations"].is_array()) {
        return CallToolResult::error("operations must be an RFC 6902 array");
    }
    request.operations = args["operations"];
    if (args.contains("author") && !args["author"].is_null()) {
        request.author = reader.string("author", {}, 128);
    }
    if (args.contains("reason") && !args["reason"].is_null()) {
        request.reason = reader.string("reason", {}, 512);
    }
    if (!reader.ok()) return CallToolResult::error(reader.failure);

    return finish(offline::blackboardPatch(request));
}

CallToolResult handleBlackboardListKeys(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    (void)ipc;
    if (!args.is_object()) return CallToolResult::error("arguments must be an object");
    ArgumentReader reader{args, {}};

    offline::BlackboardListKeysRequest request;
    request.board = reader.string("board", "default", offline::kBlackboardMaxBoardNameBytes);
    request.prefix = reader.string("prefix", {}, offline::kBlackboardMaxPathBytes);
    request.max_keys = static_cast<size_t>(
        reader.integer("max_keys", 500, 1, static_cast<int64_t>(offline::kBlackboardMaxKeys)));
    request.include_metadata = reader.boolean("include_metadata", false);
    if (!reader.ok()) return CallToolResult::error(reader.failure);

    return finish(offline::blackboardListKeys(request));
}

CallToolResult handleBlackboardClear(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    (void)ipc;
    if (!args.is_object()) return CallToolResult::error("arguments must be an object");
    ArgumentReader reader{args, {}};

    offline::BlackboardClearRequest request;
    request.board = reader.string("board", "default", offline::kBlackboardMaxBoardNameBytes);
    request.path = reader.string("path", {}, offline::kBlackboardMaxPathBytes);
    if (!reader.ok()) return CallToolResult::error(reader.failure);

    return finish(offline::blackboardClear(request));
}

} // namespace mcp
} // namespace didi
