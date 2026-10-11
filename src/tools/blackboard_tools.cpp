#include "didi/common/project_path.hpp"
#include "didi/mcp/mcp_protocol.hpp"
#include "didi/common/ipc_channel.hpp"
#include "didi/offline/blackboard.hpp"

#include <limits>

#include <memory>
#include <string>
#include "didi/mcp/tool_registration.hpp"

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

    // Characters, matching the maxLength the schema publishes for these
    // parameters. JSON Schema defines the length of a string as its number of
    // characters, so a byte bound here was a third as generous as the published
    // one for anything outside ASCII, and it said "bytes" about a number the
    // schema calls characters (#663).
    std::string string(const char* key, const std::string& fallback = {},
                       size_t max_characters = 512) {
        if (!args.contains(key) || args[key].is_null()) return fallback;
        if (!args[key].is_string()) {
            failure = std::string(key) + " must be a string";
            return fallback;
        }
        auto value = args[key].get<std::string>();
        if (paths::codePointCount(value) > max_characters) {
            failure = std::string(key) + " must be at most " + std::to_string(max_characters) +
                      " characters";
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
    if (outcome.isErr()) return CallToolResult::fromError(outcome.error());
    return CallToolResult::successJson(outcome.value());
}

} // namespace

CallToolResult handleBlackboardWrite(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    (void)ipc;
    if (!args.is_object()) return CallToolResult::errorJson(400, "arguments must be an object");
    ArgumentReader reader{args, {}};

    offline::BlackboardWriteRequest request;
    request.board = reader.string("board", "default", offline::kBlackboardMaxBoardNameBytes);
    request.path = reader.string("path", {}, offline::kBlackboardMaxPathCharacters);
    if (!reader.ok()) return CallToolResult::errorJson(400, reader.failure);
    if (request.path.empty()) return CallToolResult::errorJson(400, "path is required");
    if (!args.contains("value")) return CallToolResult::errorJson(400, "value is required");
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
    if (args.contains("expected_updated_at_ms") && !args["expected_updated_at_ms"].is_null()) {
        request.expected_updated_at_ms =
            reader.integer("expected_updated_at_ms", 0, 0, std::numeric_limits<int64_t>::max());
    }
    if (!reader.ok()) return CallToolResult::errorJson(400, reader.failure);

    return finish(offline::blackboardWrite(request));
}

CallToolResult handleBlackboardRead(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    (void)ipc;
    if (!args.is_object()) return CallToolResult::errorJson(400, "arguments must be an object");
    ArgumentReader reader{args, {}};

    offline::BlackboardReadRequest request;
    request.board = reader.string("board", "default", offline::kBlackboardMaxBoardNameBytes);
    request.path = reader.string("path", {}, offline::kBlackboardMaxPathCharacters);
    request.deep = reader.boolean("deep", true);
    request.include_metadata = reader.boolean("include_metadata", false);
    if (!reader.ok()) return CallToolResult::errorJson(400, reader.failure);

    return finish(offline::blackboardRead(request));
}

CallToolResult handleBlackboardPatch(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    (void)ipc;
    if (!args.is_object()) return CallToolResult::errorJson(400, "arguments must be an object");
    ArgumentReader reader{args, {}};

    offline::BlackboardPatchRequest request;
    request.board = reader.string("board", "default", offline::kBlackboardMaxBoardNameBytes);
    if (!reader.ok()) return CallToolResult::errorJson(400, reader.failure);
    if (!args.contains("operations")) return CallToolResult::errorJson(400, "operations is required");
    if (!args["operations"].is_array()) {
        return CallToolResult::errorJson(400, "operations must be an RFC 6902 array");
    }
    request.operations = args["operations"];
    if (args.contains("author") && !args["author"].is_null()) {
        request.author = reader.string("author", {}, 128);
    }
    if (args.contains("reason") && !args["reason"].is_null()) {
        request.reason = reader.string("reason", {}, 512);
    }
    if (args.contains("expected_revision") && !args["expected_revision"].is_null()) {
        request.expected_revision =
            reader.integer("expected_revision", 0, 0, std::numeric_limits<int64_t>::max());
    }
    if (!reader.ok()) return CallToolResult::errorJson(400, reader.failure);

    return finish(offline::blackboardPatch(request));
}

CallToolResult handleBlackboardListKeys(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    (void)ipc;
    if (!args.is_object()) return CallToolResult::errorJson(400, "arguments must be an object");
    ArgumentReader reader{args, {}};

    offline::BlackboardListKeysRequest request;
    request.board = reader.string("board", "default", offline::kBlackboardMaxBoardNameBytes);
    request.prefix = reader.string("prefix", {}, offline::kBlackboardMaxPathCharacters);
    request.max_keys = static_cast<size_t>(
        reader.integer("max_keys", 500, 1, static_cast<int64_t>(offline::kBlackboardMaxKeys)));
    request.include_metadata = reader.boolean("include_metadata", false);
    if (!reader.ok()) return CallToolResult::errorJson(400, reader.failure);

    return finish(offline::blackboardListKeys(request));
}

CallToolResult handleBlackboardClear(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    (void)ipc;
    if (!args.is_object()) return CallToolResult::errorJson(400, "arguments must be an object");
    ArgumentReader reader{args, {}};

    offline::BlackboardClearRequest request;
    request.board = reader.string("board", "default", offline::kBlackboardMaxBoardNameBytes);
    request.path = reader.string("path", {}, offline::kBlackboardMaxPathCharacters);
    if (args.contains("author") && !args["author"].is_null()) {
        request.author = reader.string("author", {}, 128);
    }
    if (args.contains("reason") && !args["reason"].is_null()) {
        request.reason = reader.string("reason", {}, 512);
    }
    if (!reader.ok()) return CallToolResult::errorJson(400, reader.failure);

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
    if (!args.is_object()) return CallToolResult::errorJson(400, "arguments must be an object");
    ArgumentReader reader{args, {}};

    offline::BlackboardTaskCreateRequest request;
    request.board = reader.string("board", "default", offline::kBlackboardMaxBoardNameBytes);
    request.task_id = reader.string("task_id", {}, offline::kBlackboardMaxTaskIdBytes);
    request.title = reader.string("title", {}, offline::kBlackboardMaxTaskTitleCharacters);
    request.priority = reader.integer("priority", 0, -1000, 1000);
    if (!reader.ok()) return CallToolResult::errorJson(400, reader.failure);
    if (request.title.empty()) return CallToolResult::errorJson(400, "title is required");

    if (args.contains("description") && !args["description"].is_null()) {
        request.description = reader.string("description", {}, offline::kBlackboardMaxTaskTextCharacters);
    }
    if (args.contains("author") && !args["author"].is_null()) {
        request.author = reader.string("author", {}, 128);
    }
    if (args.contains("assigned_to") && !args["assigned_to"].is_null()) {
        request.assigned_to = reader.string("assigned_to", {}, offline::kBlackboardMaxTaskIdBytes);
    }
    if (!reader.ok()) return CallToolResult::errorJson(400, reader.failure);

    std::string failure;
    if (!readStringList(args, "dependencies", offline::kBlackboardMaxTaskDependencies,
                        offline::kBlackboardMaxTaskIdBytes, request.dependencies, failure) ||
        !readStringList(args, "tags", offline::kBlackboardMaxTaskTags, 64, request.tags, failure)) {
        return CallToolResult::errorJson(400, failure);
    }

    return finish(offline::blackboardTaskCreate(request));
}

CallToolResult handleBlackboardTaskClaim(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    (void)ipc;
    if (!args.is_object()) return CallToolResult::errorJson(400, "arguments must be an object");
    ArgumentReader reader{args, {}};

    offline::BlackboardTaskClaimRequest request;
    request.board = reader.string("board", "default", offline::kBlackboardMaxBoardNameBytes);
    request.agent_id = reader.string("agent_id", {}, offline::kBlackboardMaxTaskIdBytes);
    request.lease_seconds = reader.integer("lease_seconds", offline::kBlackboardDefaultLeaseSeconds,
                                           1, offline::kBlackboardMaxLeaseSeconds);
    if (!reader.ok()) return CallToolResult::errorJson(400, reader.failure);
    if (request.agent_id.empty()) return CallToolResult::errorJson(400, "agent_id is required");

    if (args.contains("task_id") && !args["task_id"].is_null()) {
        request.task_id = reader.string("task_id", {}, offline::kBlackboardMaxTaskIdBytes);
    }
    if (args.contains("tag") && !args["tag"].is_null()) {
        request.tag = reader.string("tag", {}, 64);
    }
    if (!reader.ok()) return CallToolResult::errorJson(400, reader.failure);

    return finish(offline::blackboardTaskClaim(request));
}

CallToolResult handleBlackboardTaskUpdate(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    (void)ipc;
    if (!args.is_object()) return CallToolResult::errorJson(400, "arguments must be an object");
    ArgumentReader reader{args, {}};

    offline::BlackboardTaskUpdateRequest request;
    request.board = reader.string("board", "default", offline::kBlackboardMaxBoardNameBytes);
    request.task_id = reader.string("task_id", {}, offline::kBlackboardMaxTaskIdBytes);
    request.agent_id = reader.string("agent_id", {}, offline::kBlackboardMaxTaskIdBytes);
    if (!reader.ok()) return CallToolResult::errorJson(400, reader.failure);
    if (request.task_id.empty()) return CallToolResult::errorJson(400, "task_id is required");
    if (request.agent_id.empty()) return CallToolResult::errorJson(400, "agent_id is required");

    if (args.contains("progress") && !args["progress"].is_null()) {
        request.progress = reader.integer("progress", 0, 0, 100);
    }
    if (args.contains("note") && !args["note"].is_null()) {
        request.note = reader.string("note", {}, offline::kBlackboardMaxTaskTextCharacters);
    }
    if (args.contains("status") && !args["status"].is_null()) {
        request.status = reader.string("status", {}, 32);
    }
    if (args.contains("renew_lease_seconds") && !args["renew_lease_seconds"].is_null()) {
        request.renew_lease_seconds = reader.integer("renew_lease_seconds", 0, 1,
                                                     offline::kBlackboardMaxLeaseSeconds);
    }
    if (!reader.ok()) return CallToolResult::errorJson(400, reader.failure);

    return finish(offline::blackboardTaskUpdate(request));
}

CallToolResult handleBlackboardTaskComplete(const json& args,
                                            std::shared_ptr<ipc::IIpcClient> ipc) {
    (void)ipc;
    if (!args.is_object()) return CallToolResult::errorJson(400, "arguments must be an object");
    ArgumentReader reader{args, {}};

    offline::BlackboardTaskCompleteRequest request;
    request.board = reader.string("board", "default", offline::kBlackboardMaxBoardNameBytes);
    request.task_id = reader.string("task_id", {}, offline::kBlackboardMaxTaskIdBytes);
    request.agent_id = reader.string("agent_id", {}, offline::kBlackboardMaxTaskIdBytes);
    if (!reader.ok()) return CallToolResult::errorJson(400, reader.failure);
    if (request.task_id.empty()) return CallToolResult::errorJson(400, "task_id is required");
    if (request.agent_id.empty()) return CallToolResult::errorJson(400, "agent_id is required");
    if (args.contains("artifacts") && !args["artifacts"].is_null()) {
        request.artifacts = args["artifacts"];
    }

    return finish(offline::blackboardTaskComplete(request));
}

CallToolResult handleBlackboardTaskList(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    (void)ipc;
    if (!args.is_object()) return CallToolResult::errorJson(400, "arguments must be an object");
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
    if (!reader.ok()) return CallToolResult::errorJson(400, reader.failure);

    return finish(offline::blackboardTaskList(request));
}

// The tools whose handlers this file holds. registerAllDefaultTools calls
// each domain's in turn (#1256).
void ToolRegistry::registerBlackboardTools() {
    {
        ToolDefinition t;
        t.name = "blackboard_write";
        t.description = "Writes a value at a dot or slash path on a shared board, so a later agent in another process can read the decision instead of re-deriving it.";
        t.inputSchema = {
            {"type", "object"},
            {"properties", {
                {"board", {{"type", "string"}, {"default", "default"}, {"minLength", 1}, {"maxLength", 64},
                           {"description", "Board name. Letters, digits, underscore and hyphen. Separate boards do not see each other."}}},
                {"path", {{"type", "string"}, {"minLength", 1}, {"maxLength", 512},
                          {"description", "Dot or slash path such as architecture.inventory.slots. Segments cannot be empty, '.' or '..'."}}},
                {"value", {{"type", anyJsonType()},
                           {"description", "Any JSON value. Stored and returned verbatim; Didi never interprets or executes it."}}},
                {"author", {{"type", "string"}, {"maxLength", 128},
                            {"description", "Who wrote it. Recorded as metadata, never verified. The task tools call the same idea agent_id, because there it is an identity a lease is checked against rather than provenance."}}},
                {"reason", {{"type", "string"}, {"maxLength", 512},
                            {"description", "Why it was written. Recorded as metadata."}}},
                {"ttl_seconds", {{"type", "integer"}, {"minimum", 1}, {"maximum", 2592000},
                                 {"description", "Drop the entry once this many seconds have passed. Expiry is applied on the next read, listing or write."}}},
                {"expected_updated_at_ms", {{"type", "integer"}, {"minimum", 0},
                                          {"description", "Only write if the path is still at this updated_at_ms, which blackboard_read returns. 0 means the path must not exist yet. A mismatch is refused 409 with reason_code stale_write and the time it actually holds, the shape blackboard_task_claim already uses for a claim somebody else holds. Omit it for a last-writer-wins write."}}}
            }},
            {"required", json::array({"path", "value"})},
            {"additionalProperties", false}
        };
        t.handler = [this](const json& args) { return handleBlackboardWrite(args, m_ipcClient); };
        registerTool(std::move(t));
    }
    {
        ToolDefinition t;
        t.name = "blackboard_read";
        t.description = "Reads a board or a subtree of one. Deep returns the whole subtree; shallow returns one level and marks nested containers rather than dropping them.";
        t.inputSchema = {
            {"type", "object"},
            {"properties", {
                {"board", {{"type", "string"}, {"default", "default"}, {"minLength", 1}, {"maxLength", 64},
                           {"description", "Board name. Letters, digits, underscore and hyphen. Separate boards do not see each other."}}},
                {"path", {{"type", "string"}, {"maxLength", 512},
                          {"description", "Dot or slash path. Omit to read the whole board."}}},
                {"deep", {{"type", "boolean"}, {"default", true},
                          {"description", "False returns one level, with nested containers replaced by a _truncated marker carrying their size."}}},
                {"include_metadata", {{"type", "boolean"}, {"default", false},
                                      {"description", "Include the author, reason, write time and expiry recorded for each path."}}}
            }},
            {"additionalProperties", false}
        };
        t.handler = [this](const json& args) { return handleBlackboardRead(args, m_ipcClient); };
        registerTool(std::move(t));
    }
    {
        ToolDefinition t;
        t.name = "blackboard_patch";
        t.description = "Applies RFC 6902 operations to a board, all or nothing, so a parallel change is not silently overwritten by a read-modify-write.";
        t.inputSchema = {
            {"type", "object"},
            {"properties", {
                {"board", {{"type", "string"}, {"default", "default"}, {"minLength", 1}, {"maxLength", 64},
                           {"description", "Board name. Letters, digits, underscore and hyphen. Separate boards do not see each other."}}},
                {"operations", {{"type", "array"}, {"minItems", 1}, {"maxItems", 100},
                                {"description", "RFC 6902 operations against the board root. If any one fails, none is applied and the board is unchanged."},
                                {"items", {{"type", "object"}}}}},
                {"author", {{"type", "string"}, {"maxLength", 128},
                            {"description", "Who applied the patch. Recorded as metadata, never verified. The task tools call the same idea agent_id, because there it is an identity a lease is checked against rather than provenance."}}},
                {"reason", {{"type", "string"}, {"maxLength", 512}}},
                {"expected_revision", {{"type", "integer"}, {"minimum", 0},
                                       {"description", "Only apply if the board is still at this revision, which every read and write returns. A patch spans paths, so its unit is the board rather than one key. A mismatch is refused 409 with reason_code stale_patch."}}}
            }},
            {"required", json::array({"operations"})},
            {"additionalProperties", false}
        };
        t.handler = [this](const json& args) { return handleBlackboardPatch(args, m_ipcClient); };
        registerTool(std::move(t));
    }
    {
        ToolDefinition t;
        t.name = "blackboard_list_keys";
        t.description = "Lists the paths on a board, namespaces and values alike, so an agent can discover what another one recorded without reading the whole board.";
        t.inputSchema = {
            {"type", "object"},
            {"properties", {
                {"board", {{"type", "string"}, {"default", "default"}, {"minLength", 1}, {"maxLength", 64},
                           {"description", "Board name. Letters, digits, underscore and hyphen. Separate boards do not see each other."}}},
                {"prefix", {{"type", "string"}, {"maxLength", 512},
                            {"description", "Only paths at or beneath this one. Omit to list everything."}}},
                {"max_keys", {{"type", "integer"}, {"minimum", 1}, {"maximum", 10000}, {"default", 500}}},
                {"include_metadata", {{"type", "boolean"}, {"default", false}}}
            }},
            {"additionalProperties", false}
        };
        t.handler = [this](const json& args) { return handleBlackboardListKeys(args, m_ipcClient); };
        registerTool(std::move(t));
    }
    {
        ToolDefinition t;
        t.name = "blackboard_clear";
        t.description = "Removes a subtree, or the whole board when no path is given. This is the one that destroys work another agent is relying on, so it is confirmed.";
        t.inputSchema = {
            {"type", "object"},
            {"properties", {
                {"board", {{"type", "string"}, {"default", "default"}, {"minLength", 1}, {"maxLength", 64},
                           {"description", "Board name. Letters, digits, underscore and hyphen. Separate boards do not see each other."}}},
                {"path", {{"type", "string"}, {"maxLength", 512},
                          {"description", "Dot or slash path to remove. Omit to clear the entire board."}}},
                {"author", {{"type", "string"}, {"maxLength", 128},
                            {"description", "Who removed it. Recorded where a later reader can find it, since the keys themselves are gone: a read of a cleared path answers reason: cleared and names you."}}},
                {"reason", {{"type", "string"}, {"maxLength", 512},
                            {"description", "Why it was removed. Recorded beside the author."}}}
            }},
            {"additionalProperties", false}
        };
        t.handler = [this](const json& args) { return handleBlackboardClear(args, m_ipcClient); };
        registerTool(std::move(t));
    }
    {
        ToolDefinition t;
        t.name = "blackboard_task_create";
        t.description = "Registers a unit of work on the board, optionally waiting on other tasks. A task whose prerequisites are unmet is blocked until every one of them completes.";
        t.inputSchema = {
            {"type", "object"},
            {"properties", {
                {"board", {{"type", "string"}, {"default", "default"}, {"maxLength", 64}}},
                {"task_id", {{"type", "string"}, {"maxLength", 128},
                             {"description", "Letters, digits, underscore, hyphen and dot. Generated as TASK-n when omitted."}}},
                {"title", {{"type", "string"}, {"minLength", 1}, {"maxLength", 512}}},
                {"description", {{"type", "string"}, {"maxLength", 4096}}},
                {"author", {{"type", "string"}, {"maxLength", 128},
                            {"description", "Who asked for this task, which is not who should do it. Recorded as metadata, never verified. The claim and update tools take agent_id instead, because there it is an identity the lease is checked against rather than provenance."}}},
                {"assigned_to", {{"type", "string"}, {"maxLength", 128},
                                 {"description", "A suggestion only. Claiming is what actually assigns work."}}},
                {"dependencies", {{"type", "array"}, {"maxItems", 64}, {"items", {{"type", "string"}}},
                                  {"description", "Task ids that must reach completed first. Each must already exist: depending on something that does not exist would block forever with nothing to explain it."}}},
                {"tags", {{"type", "array"}, {"maxItems", 16}, {"items", {{"type", "string"}}}}},
                {"priority", {{"type", "integer"}, {"minimum", -1000}, {"maximum", 1000}, {"default", 0},
                              {"description", "Higher is claimed first. Ties go to the older task."}}}
            }},
            {"required", json::array({"title"})},
            {"additionalProperties", false}
        };
        t.handler = [this](const json& args) { return handleBlackboardTaskCreate(args, m_ipcClient); };
        registerTool(std::move(t));
    }
    {
        ToolDefinition t;
        t.name = "blackboard_task_claim";
        t.description = "Atomically leases the next ready task, or a named one. Reading that a task is free and writing that it is yours happen under one lock, so two agents cannot both win it.";
        t.inputSchema = {
            {"type", "object"},
            {"properties", {
                {"board", {{"type", "string"}, {"default", "default"}, {"maxLength", 64}}},
                {"agent_id", {{"type", "string"}, {"minLength", 1}, {"maxLength", 128},
                              {"description", "Who is taking the work. Recorded as the lease owner and required to update or complete it."}}},
                {"task_id", {{"type", "string"}, {"maxLength", 128},
                             {"description", "Claim this task specifically. Omit to take the highest priority ready one."}}},
                {"tag", {{"type", "string"}, {"maxLength", 64},
                         {"description", "Only consider tasks carrying this tag."}}},
                {"lease_seconds", {{"type", "integer"}, {"minimum", 1}, {"maximum", 86400}, {"default", 300},
                                   {"description", "How long the claim holds. When it lapses the task returns to the pool, so an agent that dies does not strand the work."}}}
            }},
            {"required", json::array({"agent_id"})},
            {"additionalProperties", false}
        };
        t.handler = [this](const json& args) { return handleBlackboardTaskClaim(args, m_ipcClient); };
        registerTool(std::move(t));
    }
    {
        ToolDefinition t;
        t.name = "blackboard_task_update";
        t.description = "Records progress, adds a note, renews the lease, or hands the task back for review. Requires the live lease, except for reopening a needs_review or failed task.";
        t.inputSchema = {
            {"type", "object"},
            {"properties", {
                {"board", {{"type", "string"}, {"default", "default"}, {"maxLength", 64}}},
                {"task_id", {{"type", "string"}, {"minLength", 1}, {"maxLength", 128}}},
                {"agent_id", {{"type", "string"}, {"minLength", 1}, {"maxLength", 128},
                              {"description", "Must hold the live lease, unless reopening a needs_review or failed task, which is by definition somebody else's call."}}},
                {"progress", {{"type", "integer"}, {"minimum", 0}, {"maximum", 100}}},
                {"note", {{"type", "string"}, {"maxLength", 4096},
                          {"description", "Appended to the task's notes. The oldest is dropped past 100."}}},
                {"status", {{"type", "string"}, {"enum", json::array({"needs_review", "failed", "pending"})},
                            {"description", "needs_review and failed both release the lease. pending reopens a reviewed or failed task."}}},
                {"renew_lease_seconds", {{"type", "integer"}, {"minimum", 1}, {"maximum", 86400},
                                         {"description", "Push the lease expiry out. Nothing renews a lease on an agent's behalf."}}}
            }},
            {"required", json::array({"task_id", "agent_id"})},
            {"additionalProperties", false}
        };
        t.handler = [this](const json& args) { return handleBlackboardTaskUpdate(args, m_ipcClient); };
        registerTool(std::move(t));
    }
    {
        ToolDefinition t;
        t.name = "blackboard_task_complete";
        t.description = "Marks a task done and releases whatever was waiting on it. Only the agent holding the live lease may complete it, because completing someone else's task releases dependents on work that is still half done.";
        t.inputSchema = {
            {"type", "object"},
            {"properties", {
                {"board", {{"type", "string"}, {"default", "default"}, {"maxLength", 64}}},
                {"task_id", {{"type", "string"}, {"minLength", 1}, {"maxLength", 128}}},
                {"agent_id", {{"type", "string"}, {"minLength", 1}, {"maxLength", 128},
                              {"description", "Must hold the live lease."}}},
                {"artifacts", {{"type", anyJsonType()},
                               {"description", "Free-form record of what changed: files, node paths, board keys. Stored and returned verbatim."}}}
            }},
            {"required", json::array({"task_id", "agent_id"})},
            {"additionalProperties", false}
        };
        t.handler = [this](const json& args) { return handleBlackboardTaskComplete(args, m_ipcClient); };
        registerTool(std::move(t));
    }
    {
        ToolDefinition t;
        t.name = "blackboard_task_list";
        t.description = "Lists tasks with their status, lease and dependencies, filtered by status, assignee or tag. Lapsed leases are reclaimed before the list is built, so nothing reads as held by an agent that is gone.";
        t.inputSchema = {
            {"type", "object"},
            {"properties", {
                {"board", {{"type", "string"}, {"default", "default"}, {"maxLength", 64}}},
                {"status", {{"type", "string"},
                            {"enum", json::array({"blocked", "pending", "in_progress", "needs_review", "completed", "failed"})}}},
                {"assigned_to", {{"type", "string"}, {"maxLength", 128}}},
                {"tag", {{"type", "string"}, {"maxLength", 64}}},
                {"max_tasks", {{"type", "integer"}, {"minimum", 1}, {"maximum", 2000}, {"default", 200}}}
            }},
            {"additionalProperties", false}
        };
        t.handler = [this](const json& args) { return handleBlackboardTaskList(args, m_ipcClient); };
        registerTool(std::move(t));
    }
}

} // namespace mcp
} // namespace didi
