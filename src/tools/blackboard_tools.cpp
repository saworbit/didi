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


namespace {

// Reads a bounded array of short strings, used for dependencies and tags.
bool readStringList(const json& args, const char* key, size_t max_items, size_t max_bytes,
                    std::vector<std::string>& out, std::string& failure) {
    if (!args.contains(key) || args[key].is_null()) return true;
    if (!args[key].is_array()) {
        failure = std::string(key) + " must be an array of strings";
        return false;
    }
    if (args[key].size() > max_items) {
        failure = std::string(key) + " must hold at most " + std::to_string(max_items) + " entries";
        return false;
    }
    for (const auto& item : args[key]) {
        if (!item.is_string() || item.get<std::string>().empty() ||
            item.get<std::string>().size() > max_bytes) {
            failure = std::string(key) + " entries must be non-empty strings of at most " +
                      std::to_string(max_bytes) + " bytes";
            return false;
        }
        out.push_back(item.get<std::string>());
    }
    return true;
}

} // namespace

CallToolResult handleBlackboardTaskCreate(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    (void)ipc;
    if (!args.is_object()) return CallToolResult::error("arguments must be an object");
    ArgumentReader reader{args, {}};

    offline::BlackboardTaskCreateRequest request;
    request.board = reader.string("board", "default", offline::kBlackboardMaxBoardNameBytes);
    request.task_id = reader.string("task_id", {}, offline::kBlackboardMaxTaskIdBytes);
    request.title = reader.string("title", {}, offline::kBlackboardMaxTaskTitleBytes);
    request.priority = reader.integer("priority", 0, -1000, 1000);
    if (!reader.ok()) return CallToolResult::error(reader.failure);
    if (request.title.empty()) return CallToolResult::error("title is required");

    if (args.contains("description") && !args["description"].is_null()) {
        request.description = reader.string("description", {}, offline::kBlackboardMaxTaskTextBytes);
    }
    if (args.contains("assigned_to") && !args["assigned_to"].is_null()) {
        request.assigned_to = reader.string("assigned_to", {}, offline::kBlackboardMaxTaskIdBytes);
    }
    if (!reader.ok()) return CallToolResult::error(reader.failure);

    std::string failure;
    if (!readStringList(args, "dependencies", offline::kBlackboardMaxTaskDependencies,
                        offline::kBlackboardMaxTaskIdBytes, request.dependencies, failure) ||
        !readStringList(args, "tags", offline::kBlackboardMaxTaskTags, 64, request.tags, failure)) {
        return CallToolResult::error(failure);
    }

    return finish(offline::blackboardTaskCreate(request));
}

CallToolResult handleBlackboardTaskClaim(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    (void)ipc;
    if (!args.is_object()) return CallToolResult::error("arguments must be an object");
    ArgumentReader reader{args, {}};

    offline::BlackboardTaskClaimRequest request;
    request.board = reader.string("board", "default", offline::kBlackboardMaxBoardNameBytes);
    request.agent_id = reader.string("agent_id", {}, offline::kBlackboardMaxTaskIdBytes);
    request.lease_seconds = reader.integer("lease_seconds", offline::kBlackboardDefaultLeaseSeconds,
                                           1, offline::kBlackboardMaxLeaseSeconds);
    if (!reader.ok()) return CallToolResult::error(reader.failure);
    if (request.agent_id.empty()) return CallToolResult::error("agent_id is required");

    if (args.contains("task_id") && !args["task_id"].is_null()) {
        request.task_id = reader.string("task_id", {}, offline::kBlackboardMaxTaskIdBytes);
    }
    if (args.contains("tag") && !args["tag"].is_null()) {
        request.tag = reader.string("tag", {}, 64);
    }
    if (!reader.ok()) return CallToolResult::error(reader.failure);

    return finish(offline::blackboardTaskClaim(request));
}

CallToolResult handleBlackboardTaskUpdate(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    (void)ipc;
    if (!args.is_object()) return CallToolResult::error("arguments must be an object");
    ArgumentReader reader{args, {}};

    offline::BlackboardTaskUpdateRequest request;
    request.board = reader.string("board", "default", offline::kBlackboardMaxBoardNameBytes);
    request.task_id = reader.string("task_id", {}, offline::kBlackboardMaxTaskIdBytes);
    request.agent_id = reader.string("agent_id", {}, offline::kBlackboardMaxTaskIdBytes);
    if (!reader.ok()) return CallToolResult::error(reader.failure);
    if (request.task_id.empty()) return CallToolResult::error("task_id is required");
    if (request.agent_id.empty()) return CallToolResult::error("agent_id is required");

    if (args.contains("progress") && !args["progress"].is_null()) {
        request.progress = reader.integer("progress", 0, 0, 100);
    }
    if (args.contains("note") && !args["note"].is_null()) {
        request.note = reader.string("note", {}, offline::kBlackboardMaxTaskTextBytes);
    }
    if (args.contains("status") && !args["status"].is_null()) {
        request.status = reader.string("status", {}, 32);
    }
    if (args.contains("renew_lease_seconds") && !args["renew_lease_seconds"].is_null()) {
        request.renew_lease_seconds = reader.integer("renew_lease_seconds", 0, 1,
                                                     offline::kBlackboardMaxLeaseSeconds);
    }
    if (!reader.ok()) return CallToolResult::error(reader.failure);

    return finish(offline::blackboardTaskUpdate(request));
}

CallToolResult handleBlackboardTaskComplete(const json& args,
                                            std::shared_ptr<ipc::IIpcClient> ipc) {
    (void)ipc;
    if (!args.is_object()) return CallToolResult::error("arguments must be an object");
    ArgumentReader reader{args, {}};

    offline::BlackboardTaskCompleteRequest request;
    request.board = reader.string("board", "default", offline::kBlackboardMaxBoardNameBytes);
    request.task_id = reader.string("task_id", {}, offline::kBlackboardMaxTaskIdBytes);
    request.agent_id = reader.string("agent_id", {}, offline::kBlackboardMaxTaskIdBytes);
    if (!reader.ok()) return CallToolResult::error(reader.failure);
    if (request.task_id.empty()) return CallToolResult::error("task_id is required");
    if (request.agent_id.empty()) return CallToolResult::error("agent_id is required");
    if (args.contains("artifacts") && !args["artifacts"].is_null()) {
        request.artifacts = args["artifacts"];
    }

    return finish(offline::blackboardTaskComplete(request));
}

CallToolResult handleBlackboardTaskList(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    (void)ipc;
    if (!args.is_object()) return CallToolResult::error("arguments must be an object");
    ArgumentReader reader{args, {}};

    offline::BlackboardTaskListRequest request;
    request.board = reader.string("board", "default", offline::kBlackboardMaxBoardNameBytes);
    request.max_tasks = static_cast<size_t>(
        reader.integer("max_tasks", 200, 1, static_cast<int64_t>(offline::kBlackboardMaxTasks)));
    if (args.contains("status") && !args["status"].is_null()) {
        request.status = reader.string("status", {}, 32);
    }
    if (args.contains("assigned_to") && !args["assigned_to"].is_null()) {
        request.assigned_to = reader.string("assigned_to", {}, offline::kBlackboardMaxTaskIdBytes);
    }
    if (args.contains("tag") && !args["tag"].is_null()) {
        request.tag = reader.string("tag", {}, 64);
    }
    if (!reader.ok()) return CallToolResult::error(reader.failure);

    return finish(offline::blackboardTaskList(request));
}

} // namespace mcp
} // namespace didi
