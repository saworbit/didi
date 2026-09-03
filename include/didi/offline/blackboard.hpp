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
inline constexpr size_t kBlackboardMaxPathBytes = 512;
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
    bool dry_run{false};
};

Result<json> blackboardWrite(const BlackboardWriteRequest& request, BlackboardClock clock = {});
Result<json> blackboardRead(const BlackboardReadRequest& request, BlackboardClock clock = {});
Result<json> blackboardPatch(const BlackboardPatchRequest& request, BlackboardClock clock = {});
Result<json> blackboardListKeys(const BlackboardListKeysRequest& request, BlackboardClock clock = {});
Result<json> blackboardClear(const BlackboardClearRequest& request, BlackboardClock clock = {});

// Exposed for tests and for the tool layer's error messages. Splits a dot or
// slash path into segments, rejecting anything that could escape the board or
// make it unreadable.
Result<std::vector<std::string>> blackboardSplitPath(const std::string& path);

// The directory a board lives in, resolved under the current project root.
Result<std::filesystem::path> blackboardBoardPath(const std::string& board);

} // namespace didi::offline
