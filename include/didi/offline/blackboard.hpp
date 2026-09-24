#pragma once

#include "didi/common/types.hpp"

#include <cstdint>
#include <functional>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace didi::offline {

// A blackboard is a coordination surface between agents, not a database. Every
// bound below exists so that one agent cannot make the board unusable for the
// next one, and each is reported in the response that hits it.
inline constexpr size_t kBlackboardMaxBoardNameBytes = 64;
// Characters, as the published maxLength counts them. The tool readers were
// moved to characters by #663 and this one was not, so a path of 200 CJK
// characters passed the reader and was refused here as over 512 bytes.
inline constexpr size_t kBlackboardMaxPathCharacters = 512;
inline constexpr size_t kBlackboardMaxPathSegments = 32;
inline constexpr size_t kBlackboardMaxValueBytes = 256 * 1024;
inline constexpr size_t kBlackboardMaxBoardBytes = 4 * 1024 * 1024;
inline constexpr size_t kBlackboardMaxKeys = 10'000;
inline constexpr size_t kBlackboardMaxDepth = 32;
inline constexpr size_t kBlackboardMaxPatchOperations = 100;
inline constexpr int64_t kBlackboardMaxTtlSeconds = 30 * 24 * 60 * 60;

// Milliseconds since the Unix epoch. Injected so tests can prove expiry without
// sleeping, and so every entry in one operation shares a single reading.
using BlackboardClock = std::function<int64_t()>;

struct BlackboardWriteRequest {
    std::string board{"default"};
    std::string path;
    json value;
    std::optional<std::string> author;
    std::optional<std::string> reason;
    std::optional<int64_t> ttl_seconds;
    // "Only if this key has not changed since I read it."
    //
    // The lease was the only concurrency guard on this surface and it covers
    // tasks. Keys had no version and nothing a second writer could pin a write
    // to, so two agents that both read 0, both incremented and both wrote 1
    // left the board holding 1, with neither call an error and nothing in
    // either response saying a concurrent change had happened (#682). The value
    // is the `updated_at_ms` a read reported for this path, or 0 for "this path
    // must not exist yet". A caller that does not pass it keeps the old
    // behaviour.
    std::optional<int64_t> expected_updated_at_ms;
    bool dry_run{false};
};

struct BlackboardReadRequest {
    std::string board{"default"};
    std::string path;              // Empty reads the whole board.
    bool deep{true};               // False returns one level, with child names only.
    bool include_metadata{false};
};

struct BlackboardPatchRequest {
    std::string board{"default"};
    // "Only if the board has not changed since I read it." A patch spans paths,
    // so its unit is the board: the `revision` a read reported. See
    // BlackboardWriteRequest::expected_updated_at_ms (#682).
    std::optional<int64_t> expected_revision;
    // Copy-initialized, not braced: `json x{json::array()}` picks the
    // initializer-list constructor on GCC and yields an array holding one
    // array, which is not what any of this means.
    json operations = json::array();  // RFC 6902, applied all or nothing.
    std::optional<std::string> author;
    std::optional<std::string> reason;
    bool dry_run{false};
};

struct BlackboardListKeysRequest {
    std::string board{"default"};
    std::string prefix;            // Dot or slash path prefix; empty lists everything.
    size_t max_keys{500};
    bool include_metadata{false};
};

struct BlackboardClearRequest {
    std::string board{"default"};
    std::string path;              // Empty clears the whole board.
    // Who removed it and why.
    //
    // This is the one destructive call on the board and the only one that had
    // no identity argument at all: an agent that came back to find its keys
    // gone could read `author` on every value still there and nothing about the
    // call that removed the rest (#681). Recorded where a later reader can find
    // it, since the keys themselves are gone.
    std::optional<std::string> author;
    std::optional<std::string> reason;
    bool dry_run{false};
};

// Tasks share the board file and its lock. A task is claimed when it holds an
// unexpired lease and by nothing else: a separate "locked" status would be a
// second record of the same fact, and two records drift.
inline constexpr size_t kBlackboardMaxTasks = 2'000;
inline constexpr size_t kBlackboardMaxTaskIdBytes = 128;
// Characters, not bytes, because these are the numbers the schema publishes
// as maxLength for title and description, and JSON Schema defines that as a
// character count. A byte cap here enforced a bound a third as generous as
// the published one for anything outside ASCII (#663).
inline constexpr size_t kBlackboardMaxTaskTitleCharacters = 512;
inline constexpr size_t kBlackboardMaxTaskTextCharacters = 4'096;
inline constexpr size_t kBlackboardMaxTaskDependencies = 64;
inline constexpr size_t kBlackboardMaxTaskTags = 16;
inline constexpr int64_t kBlackboardMaxLeaseSeconds = 24 * 60 * 60;
inline constexpr int64_t kBlackboardDefaultLeaseSeconds = 300;

struct BlackboardTaskCreateRequest {
    std::string board{"default"};
    std::string task_id;                    // Generated when empty.
    std::string title;
    std::optional<std::string> description;
    // Who asked for this task. `assigned_to` is who should do it, which is a
    // different question, and this call could answer neither (#681).
    std::optional<std::string> author;
    std::optional<std::string> assigned_to;
    std::vector<std::string> dependencies;
    std::vector<std::string> tags;
    int64_t priority{0};                    // Higher is claimed first.
    bool dry_run{false};
};

struct BlackboardTaskClaimRequest {
    std::string board{"default"};
    std::string agent_id;
    std::optional<std::string> task_id;     // Claim this one, or the best ready one.
    std::optional<std::string> tag;         // Only consider tasks carrying this tag.
    int64_t lease_seconds{kBlackboardDefaultLeaseSeconds};
    bool dry_run{false};
};

struct BlackboardTaskUpdateRequest {
    std::string board{"default"};
    std::string task_id;
    std::string agent_id;                   // Must hold the lease.
    std::optional<int64_t> progress;        // 0 to 100.
    std::optional<std::string> note;
    std::optional<std::string> status;      // needs_review or failed.
    std::optional<int64_t> renew_lease_seconds;
    bool dry_run{false};
};

struct BlackboardTaskCompleteRequest {
    std::string board{"default"};
    std::string task_id;
    std::string agent_id;                   // Must hold the lease.
    json artifacts;                         // Free-form pointers to what changed.
    bool dry_run{false};
};

struct BlackboardTaskListRequest {
    std::string board{"default"};
    std::optional<std::string> status;
    std::optional<std::string> assigned_to;
    std::optional<std::string> tag;
    size_t max_tasks{200};
};

Result<json> blackboardWrite(const BlackboardWriteRequest& request, BlackboardClock clock = {});
Result<json> blackboardRead(const BlackboardReadRequest& request, BlackboardClock clock = {});
Result<json> blackboardPatch(const BlackboardPatchRequest& request, BlackboardClock clock = {});
Result<json> blackboardListKeys(const BlackboardListKeysRequest& request, BlackboardClock clock = {});
Result<json> blackboardClear(const BlackboardClearRequest& request, BlackboardClock clock = {});

Result<json> blackboardTaskCreate(const BlackboardTaskCreateRequest& request, BlackboardClock clock = {});
Result<json> blackboardTaskClaim(const BlackboardTaskClaimRequest& request, BlackboardClock clock = {});
Result<json> blackboardTaskUpdate(const BlackboardTaskUpdateRequest& request, BlackboardClock clock = {});
Result<json> blackboardTaskComplete(const BlackboardTaskCompleteRequest& request, BlackboardClock clock = {});
Result<json> blackboardTaskList(const BlackboardTaskListRequest& request, BlackboardClock clock = {});

// Exposed for tests and for the tool layer's error messages. Splits a dot or
// slash path into segments, rejecting anything that could escape the board or
// make it unreadable.
Result<std::vector<std::string>> blackboardSplitPath(const std::string& path);

// The directory a board lives in, resolved under the current project root.
// Whether a string may name a board: letters, digits, underscore and hyphen,
// within the length cap. Published so the resource URI parser can tell a bad
// board name from a bad kind rather than reporting one as the other (#515).
bool isLegalBlackboardBoardName(const std::string& board);

Result<std::filesystem::path> blackboardBoardPath(const std::string& board);

// Reads a whole board as one document, for the `blackboard://` resources. Kind is
// "state" or "tasks". Expiry is applied first, so a resource never reports an
// entry a read would not return.
Result<json> blackboardReadResource(const std::string& board, const std::string& kind,
                                    BlackboardClock clock = {});

// Size and modified time of a board file, or nullopt when it does not exist yet.
// The watcher compares these rather than parsing, because it runs on a timer and
// most ticks find nothing.
struct BlackboardFileStamp {
    uint64_t size{0};
    int64_t modified_ns{0};

    bool operator==(const BlackboardFileStamp& other) const {
        return size == other.size && modified_ns == other.modified_ns;
    }
    bool operator!=(const BlackboardFileStamp& other) const { return !(*this == other); }
};

std::optional<BlackboardFileStamp> blackboardFileStamp(const std::string& board);

} // namespace didi::offline
