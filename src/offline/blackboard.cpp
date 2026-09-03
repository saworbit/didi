#include "didi/offline/blackboard.hpp"

#include "didi/common/logger.hpp"
#include "didi/common/project_path.hpp"
#include "didi/runtime/session_lock.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <sstream>
#include <thread>

namespace didi::offline {
namespace {

constexpr int kBoardFormatVersion = 1;
constexpr int kLockAttempts = 50;
constexpr int kLockRetryMs = 20;

int64_t systemClockMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}

bool isLegalBoardName(const std::string& board) {
    if (board.empty() || board.size() > kBlackboardMaxBoardNameBytes) return false;
    return std::all_of(board.begin(), board.end(), [](unsigned char character) {
        return std::isalnum(character) != 0 || character == '_' || character == '-';
    });
}

// A path segment may not be empty, may not be a traversal, and may not carry a
// control character. Everything else is the caller's business: the board is
// their namespace, not ours.
bool isLegalSegment(const std::string& segment) {
    if (segment.empty() || segment == "." || segment == "..") return false;
    return std::none_of(segment.begin(), segment.end(), [](unsigned char character) {
        return character < 0x20 || character == 0x7f;
    });
}

json::json_pointer pointerFor(const std::vector<std::string>& segments) {
    std::string pointer;
    for (const auto& segment : segments) {
        std::string escaped;
        for (const char character : segment) {
            if (character == '~') escaped += "~0";
            else if (character == '/') escaped += "~1";
            else escaped += character;
        }
        pointer += "/" + escaped;
    }
    return json::json_pointer(pointer);
}

std::string joinPath(const std::vector<std::string>& segments) {
    std::string joined;
    for (const auto& segment : segments) {
        if (!joined.empty()) joined += ".";
        joined += segment;
    }
    return joined;
}

size_t documentDepth(const json& value, size_t depth = 1) {
    if (!value.is_object() && !value.is_array()) return depth;
    size_t deepest = depth;
    for (const auto& child : value) {
        deepest = std::max(deepest, documentDepth(child, depth + 1));
    }
    return deepest;
}

size_t countKeys(const json& value) {
    if (!value.is_object()) return 0;
    size_t total = 0;
    for (const auto& entry : value.items()) {
        total += 1 + countKeys(entry.value());
    }
    return total;
}

// Collects every path in the board, containers included, because a namespace is
// something an agent asks about as often as a leaf.
void collectPaths(const json& value, const std::string& prefix,
                  std::vector<std::pair<std::string, std::string>>& out) {
    if (!value.is_object()) return;
    for (const auto& entry : value.items()) {
        const std::string path = prefix.empty() ? entry.key() : prefix + "." + entry.key();
        const char* kind = entry.value().is_object() ? "namespace"
                         : entry.value().is_array()  ? "array"
                                                     : "value";
        out.emplace_back(path, kind);
        if (entry.value().is_object()) collectPaths(entry.value(), path, out);
    }
}

// One level only. A nested container becomes a marked placeholder rather than
// being silently omitted, so a caller can see there is more and ask for it.
json shallowView(const json& value) {
    if (!value.is_object()) return value;
    json view = json::object();
    for (const auto& entry : value.items()) {
        if (entry.value().is_object()) {
            view[entry.key()] = {{"_truncated", "object"}, {"_keys", entry.value().size()}};
        } else if (entry.value().is_array()) {
            view[entry.key()] = {{"_truncated", "array"}, {"_items", entry.value().size()}};
        } else {
            view[entry.key()] = entry.value();
        }
    }
    return view;
}

struct Board {
    json state{json::object()};
    json meta{json::object()};
};

Result<std::filesystem::path> boardDirectory() {
    std::error_code error;
    const auto root = std::filesystem::weakly_canonical(std::filesystem::current_path(), error);
    if (error) return Error::internal("project root cannot be resolved");
    return root / ".didi" / "blackboard";
}

// Drops entries whose ttl has lapsed. Returns true when the board changed, so a
// read can persist the sweep instead of rediscovering the same dead entries.
bool sweepExpired(Board& board, int64_t now_ms) {
    std::vector<std::string> expired;
    for (const auto& entry : board.meta.items()) {
        if (!entry.value().is_object()) continue;
        const auto expires = entry.value().find("expires_at_ms");
        if (expires == entry.value().end() || !expires->is_number_integer()) continue;
        if (expires->get<int64_t>() > now_ms) continue;
        expired.push_back(entry.key());
    }
    if (expired.empty()) return false;
    for (const auto& path : expired) {
        const auto segments = blackboardSplitPath(path);
        if (segments.isOk() && !segments.value().empty()) {
            const auto pointer = pointerFor(segments.value());
            if (board.state.contains(pointer)) {
                const auto parent = pointer.parent_pointer();
                if (board.state.contains(parent) && board.state.at(parent).is_object()) {
                    board.state.at(parent).erase(segments.value().back());
                }
            }
        }
        board.meta.erase(path);
    }
    return true;
}

Result<Board> loadBoard(const std::filesystem::path& file) {
    Board board;
    std::error_code error;
    if (!std::filesystem::exists(file, error) || error) return board;

    const auto size = std::filesystem::file_size(file, error);
    if (error) return Error::internal("blackboard file cannot be measured");
    if (size > kBlackboardMaxBoardBytes) {
        return Error::invalidArgument(
            "blackboard file is larger than the " + std::to_string(kBlackboardMaxBoardBytes) +
            " byte limit and was not read");
    }

    std::ifstream input(file, std::ios::binary);
    if (!input) return Error::internal("blackboard file cannot be opened");
    std::ostringstream buffer;
    buffer << input.rdbuf();

    json document;
    try {
        document = json::parse(buffer.str());
    } catch (const json::exception&) {
        // Refuse rather than reset. The file is another agent's work, and an
        // empty board would look exactly like a successful read.
        return Error::internal("blackboard file is not valid JSON; refusing to overwrite it");
    }
    if (!document.is_object()) {
        return Error::internal("blackboard file is not a JSON object; refusing to overwrite it");
    }
    if (document.contains("state") && document["state"].is_object()) board.state = document["state"];
    if (document.contains("meta") && document["meta"].is_object()) board.meta = document["meta"];
    return board;
}

Result<bool> saveBoard(const std::filesystem::path& file, const Board& board) {
    json document = {
        {"version", kBoardFormatVersion},
        {"state", board.state},
        {"meta", board.meta}
    };
    const std::string serialized = document.dump();
    if (serialized.size() > kBlackboardMaxBoardBytes) {
        return Error::invalidArgument(
            "the write would take the board past the " + std::to_string(kBlackboardMaxBoardBytes) +
            " byte limit; nothing was written");
    }

    std::error_code error;
    std::filesystem::create_directories(file.parent_path(), error);
    if (error) return Error::internal("blackboard directory cannot be created");

    auto temporary = file;
    temporary += ".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) return Error::internal("blackboard file cannot be written");
        output << serialized;
        if (!output) return Error::internal("blackboard file could not be written in full");
    }
    std::filesystem::rename(temporary, file, error);
    if (error) {
        // Some Windows volumes refuse a rename onto an existing file.
        error.clear();
        std::filesystem::remove(file, error);
        error.clear();
        std::filesystem::rename(temporary, file, error);
        if (error) {
            std::error_code cleanup;
            std::filesystem::remove(temporary, cleanup);
            return Error::internal("blackboard file could not be replaced");
        }
    }
    return true;
}

json boundsPayload() {
    return {
        {"max_value_bytes", kBlackboardMaxValueBytes},
        {"max_board_bytes", kBlackboardMaxBoardBytes},
        {"max_keys", kBlackboardMaxKeys},
        {"max_depth", kBlackboardMaxDepth},
        {"max_path_segments", kBlackboardMaxPathSegments}
    };
}

Result<bool> checkBoardBounds(const json& state) {
    if (countKeys(state) > kBlackboardMaxKeys) {
        return Error::invalidArgument(
            "the write would take the board past " + std::to_string(kBlackboardMaxKeys) +
            " keys; nothing was written");
    }
    if (documentDepth(state) > kBlackboardMaxDepth) {
        return Error::invalidArgument(
            "the write would nest deeper than " + std::to_string(kBlackboardMaxDepth) +
            " levels; nothing was written");
    }
    return true;
}

// Every operation runs under an exclusive OS lock, because two agents are two
// processes. Without it a read-modify-write pair loses whichever write landed
// first, which is the failure a shared board exists to prevent.
template <typename Operation>
Result<json> withBoardLock(const std::string& board, Operation operation) {
    if (!isLegalBoardName(board)) {
        return Error::invalidArgument(
            "board name must be 1 to " + std::to_string(kBlackboardMaxBoardNameBytes) +
            " characters of letters, digits, underscore or hyphen");
    }
    auto directory = boardDirectory();
    if (directory.isErr()) return directory.error();

    std::error_code error;
    std::filesystem::create_directories(directory.value(), error);
    if (error) return Error::internal("blackboard directory cannot be created");

    const auto file = directory.value() / (board + ".json");
    const auto lock_file = directory.value() / (board + ".lock");

    std::shared_ptr<runtime::RuntimeSessionLock> lock;
    for (int attempt = 0; attempt < kLockAttempts; ++attempt) {
        auto acquired = runtime::RuntimeSessionLock::acquire(lock_file, json::object());
        if (acquired.isOk()) {
            lock = acquired.value();
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(kLockRetryMs));
    }
    if (!lock) {
        return Error::internal("another process is holding the blackboard lock for board '" +
                               board + "'");
    }
    return operation(file);
}

} // namespace

Result<std::vector<std::string>> blackboardSplitPath(const std::string& path) {
    if (path.size() > kBlackboardMaxPathBytes) {
        return Error::invalidArgument("path is longer than " +
                                      std::to_string(kBlackboardMaxPathBytes) + " bytes");
    }
    std::vector<std::string> segments;
    std::string current;
    for (const char character : path) {
        if (character == '.' || character == '/') {
            if (!isLegalSegment(current)) {
                return Error::invalidArgument(
                    "path segments cannot be empty, '.', '..', or hold control characters");
            }
            segments.push_back(current);
            current.clear();
            continue;
        }
        current += character;
    }
    if (!isLegalSegment(current)) {
        return Error::invalidArgument(
            "path segments cannot be empty, '.', '..', or hold control characters");
    }
    segments.push_back(current);
    if (segments.size() > kBlackboardMaxPathSegments) {
        return Error::invalidArgument("path has more than " +
                                      std::to_string(kBlackboardMaxPathSegments) + " segments");
    }
    return segments;
}

Result<std::filesystem::path> blackboardBoardPath(const std::string& board) {
    if (!isLegalBoardName(board)) {
        return Error::invalidArgument("board name is not legal");
    }
    auto directory = boardDirectory();
    if (directory.isErr()) return directory.error();
    return directory.value() / (board + ".json");
}

Result<json> blackboardWrite(const BlackboardWriteRequest& request, BlackboardClock clock) {
    const int64_t now_ms = clock ? clock() : systemClockMs();

    auto segments = blackboardSplitPath(request.path);
    if (segments.isErr()) return segments.error();

    if (request.value.dump().size() > kBlackboardMaxValueBytes) {
        return Error::invalidArgument(
            "value is larger than the " + std::to_string(kBlackboardMaxValueBytes) +
            " byte limit for a single write");
    }
    if (request.ttl_seconds.has_value() &&
        (*request.ttl_seconds <= 0 || *request.ttl_seconds > kBlackboardMaxTtlSeconds)) {
        return Error::invalidArgument("ttl_seconds must be between 1 and " +
                                      std::to_string(kBlackboardMaxTtlSeconds));
    }

    return withBoardLock(request.board, [&](const std::filesystem::path& file) -> Result<json> {
        auto loaded = loadBoard(file);
        if (loaded.isErr()) return loaded.error();
        Board board = loaded.value();
        sweepExpired(board, now_ms);

        const auto& parts = segments.value();
        const std::string path = joinPath(parts);

        // Build the write on a copy, refusing to turn an existing value into a
        // container on the way down. Silently replacing another agent's scalar
        // with an object is the kind of quiet default that makes a shared board
        // untrustworthy.
        json candidate_state = board.state;
        json* cursor = &candidate_state;
        for (size_t index = 0; index + 1 < parts.size(); ++index) {
            auto found = cursor->find(parts[index]);
            if (found == cursor->end()) {
                (*cursor)[parts[index]] = json::object();
                cursor = &(*cursor)[parts[index]];
                continue;
            }
            if (!found->is_object()) {
                const std::vector<std::string> walked(parts.begin(), parts.begin() + index + 1);
                return Error::invalidArgument("path '" + path + "' runs through the value at '" +
                                              joinPath(walked) + "'; clear it first");
            }
            cursor = &(*found);
        }

        const bool replaced = cursor->contains(parts.back());
        const json previous = replaced ? (*cursor)[parts.back()] : json();
        (*cursor)[parts.back()] = request.value;

        auto bounds = checkBoardBounds(candidate_state);
        if (bounds.isErr()) return bounds.error();

        json entry = json::object();
        entry["updated_at_ms"] = now_ms;
        if (request.author.has_value()) entry["author"] = *request.author;
        if (request.reason.has_value()) entry["reason"] = *request.reason;
        if (request.ttl_seconds.has_value()) {
            entry["expires_at_ms"] = now_ms + (*request.ttl_seconds * 1000);
        }

        json result = {
            {"board", request.board},
            {"path", path},
            {"replaced", replaced},
            {"dry_run", request.dry_run},
            {"metadata", entry},
            {"bounds", boundsPayload()}
        };
        if (replaced) result["previous_value"] = previous;

        if (request.dry_run) return result;

        board.state = candidate_state;
        board.meta[path] = entry;
        auto saved = saveBoard(file, board);
        if (saved.isErr()) return saved.error();
        return result;
    });
}

Result<json> blackboardRead(const BlackboardReadRequest& request, BlackboardClock clock) {
    const int64_t now_ms = clock ? clock() : systemClockMs();
    std::vector<std::string> parts;
    if (!request.path.empty()) {
        auto segments = blackboardSplitPath(request.path);
        if (segments.isErr()) return segments.error();
        parts = segments.value();
    }

    return withBoardLock(request.board, [&](const std::filesystem::path& file) -> Result<json> {
        auto loaded = loadBoard(file);
        if (loaded.isErr()) return loaded.error();
        Board board = loaded.value();
        if (sweepExpired(board, now_ms)) {
            auto saved = saveBoard(file, board);
            if (saved.isErr()) return saved.error();
        }

        const std::string path = joinPath(parts);
        json result = {
            {"board", request.board},
            {"path", path},
            {"deep", request.deep}
        };

        const json* value = &board.state;
        if (!parts.empty()) {
            const auto pointer = pointerFor(parts);
            if (!board.state.contains(pointer)) {
                result["found"] = false;
                return result;
            }
            value = &board.state.at(pointer);
        }
        result["found"] = true;
        result["value"] = request.deep ? *value : shallowView(*value);
        if (request.include_metadata) {
            json metadata = json::object();
            for (const auto& entry : board.meta.items()) {
                const std::string& entry_path = entry.key();
                if (path.empty() || entry_path == path ||
                    entry_path.rfind(path + ".", 0) == 0) {
                    metadata[entry_path] = entry.value();
                }
            }
            result["metadata"] = metadata;
        }
        return result;
    });
}

Result<json> blackboardPatch(const BlackboardPatchRequest& request, BlackboardClock clock) {
    const int64_t now_ms = clock ? clock() : systemClockMs();
    if (!request.operations.is_array()) {
        return Error::invalidArgument("operations must be an RFC 6902 array");
    }
    if (request.operations.empty()) {
        return Error::invalidArgument("operations must hold at least one operation");
    }
    if (request.operations.size() > kBlackboardMaxPatchOperations) {
        return Error::invalidArgument("operations must hold at most " +
                                      std::to_string(kBlackboardMaxPatchOperations) +
                                      " operations");
    }

    return withBoardLock(request.board, [&](const std::filesystem::path& file) -> Result<json> {
        auto loaded = loadBoard(file);
        if (loaded.isErr()) return loaded.error();
        Board board = loaded.value();
        sweepExpired(board, now_ms);

        // All or nothing. The patch is applied to a copy and the board is only
        // replaced once every operation has succeeded.
        json patched;
        try {
            patched = board.state.patch(request.operations);
        } catch (const json::exception& failure) {
            return Error::invalidArgument(
                std::string("patch failed and the board is unchanged: ") + failure.what());
        }
        if (!patched.is_object()) {
            return Error::invalidArgument("patch would replace the board root with a non-object");
        }
        auto bounds = checkBoardBounds(patched);
        if (bounds.isErr()) return bounds.error();

        // Metadata describes paths. An entry whose path the patch removed is now
        // describing nothing, so it goes with it.
        json meta = json::object();
        for (const auto& entry : board.meta.items()) {
            auto segments = blackboardSplitPath(entry.key());
            if (segments.isOk() && patched.contains(pointerFor(segments.value()))) {
                meta[entry.key()] = entry.value();
            }
        }

        json result = {
            {"board", request.board},
            {"operations_applied", request.operations.size()},
            {"dry_run", request.dry_run},
            {"bounds", boundsPayload()}
        };
        if (request.author.has_value()) result["author"] = *request.author;
        if (request.reason.has_value()) result["reason"] = *request.reason;

        if (request.dry_run) {
            result["resulting_value"] = patched;
            return result;
        }

        board.state = patched;
        board.meta = meta;
        auto saved = saveBoard(file, board);
        if (saved.isErr()) return saved.error();
        return result;
    });
}

Result<json> blackboardListKeys(const BlackboardListKeysRequest& request, BlackboardClock clock) {
    const int64_t now_ms = clock ? clock() : systemClockMs();
    if (request.max_keys < 1 || request.max_keys > kBlackboardMaxKeys) {
        return Error::invalidArgument("max_keys must be between 1 and " +
                                      std::to_string(kBlackboardMaxKeys));
    }
    std::string prefix;
    if (!request.prefix.empty()) {
        auto segments = blackboardSplitPath(request.prefix);
        if (segments.isErr()) return segments.error();
        prefix = joinPath(segments.value());
    }

    return withBoardLock(request.board, [&](const std::filesystem::path& file) -> Result<json> {
        auto loaded = loadBoard(file);
        if (loaded.isErr()) return loaded.error();
        Board board = loaded.value();
        if (sweepExpired(board, now_ms)) {
            auto saved = saveBoard(file, board);
            if (saved.isErr()) return saved.error();
        }

        std::vector<std::pair<std::string, std::string>> all;
        collectPaths(board.state, "", all);

        json keys = json::array();
        size_t matched = 0;
        for (const auto& record : all) {
            const std::string& path = record.first;
            if (!prefix.empty() && path != prefix && path.rfind(prefix + ".", 0) != 0) continue;
            ++matched;
            if (keys.size() >= request.max_keys) continue;
            json item = {{"path", path}, {"kind", record.second}};
            if (request.include_metadata && board.meta.contains(path)) {
                item["metadata"] = board.meta[path];
            }
            keys.push_back(std::move(item));
        }

        return json{
            {"board", request.board},
            {"prefix", prefix},
            {"keys", keys},
            {"returned", keys.size()},
            {"total", matched},
            {"truncated", matched > keys.size()}
        };
    });
}

Result<json> blackboardClear(const BlackboardClearRequest& request, BlackboardClock clock) {
    const int64_t now_ms = clock ? clock() : systemClockMs();
    std::vector<std::string> parts;
    if (!request.path.empty()) {
        auto segments = blackboardSplitPath(request.path);
        if (segments.isErr()) return segments.error();
        parts = segments.value();
    }

    return withBoardLock(request.board, [&](const std::filesystem::path& file) -> Result<json> {
        auto loaded = loadBoard(file);
        if (loaded.isErr()) return loaded.error();
        Board board = loaded.value();
        sweepExpired(board, now_ms);

        const std::string path = joinPath(parts);
        size_t removed_keys = 0;

        if (parts.empty()) {
            removed_keys = countKeys(board.state);
            if (!request.dry_run) {
                board.state = json::object();
                board.meta = json::object();
            }
        } else {
            const auto pointer = pointerFor(parts);
            if (!board.state.contains(pointer)) {
                return json{
                    {"board", request.board}, {"path", path}, {"found", false},
                    {"removed_keys", 0}, {"dry_run", request.dry_run}
                };
            }
            removed_keys = 1 + countKeys(board.state.at(pointer));
            if (!request.dry_run) {
                board.state.at(pointer.parent_pointer()).erase(parts.back());
                std::vector<std::string> dead;
                for (const auto& entry : board.meta.items()) {
                    if (entry.key() == path || entry.key().rfind(path + ".", 0) == 0) {
                        dead.push_back(entry.key());
                    }
                }
                for (const auto& entry_path : dead) board.meta.erase(entry_path);
            }
        }

        json result = {
            {"board", request.board},
            {"path", path},
            {"found", true},
            {"removed_keys", removed_keys},
            {"dry_run", request.dry_run}
        };
        if (request.dry_run) return result;

        auto saved = saveBoard(file, board);
        if (saved.isErr()) return saved.error();
        return result;
    });
}

} // namespace didi::offline
