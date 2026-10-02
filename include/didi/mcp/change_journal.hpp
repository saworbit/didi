#pragma once

// The change journal (Q15 in docs/BUILD_QUEUE.md, principles P1 and P3).
//
// Every mutating call the server runs is recorded where every call already
// passes, ToolRegistry::dispatchTool, never per tool: the tool, its target, the
// values before where the tool reported them and after as it read them back,
// the files the answer says it wrote, and the undo reference the bridge read
// off the editor's history. The person reviewing what an agent did reads it in
// godot://project/journal and the Control Room, and editor_undo's
// journal_entry undoes one entry on its own.
//
// It lives in the project's .didi/journal.json, because every client runs its
// own server and the harness starts one per batch: a journal in memory would
// see one conversation's calls at most. It is bounded, written atomically under
// a lock two servers share, and redacts by key name before anything reaches
// the disk. A journal that cannot be written never fails the call it was
// recording; the answer says so instead.

#include "didi/common/json.hpp"
#include "didi/common/types.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace didi::runtime {
struct RuntimeRouteLease;
}

namespace didi::mcp::journal {

inline constexpr int kFormat = 1;
// Entries kept; the oldest go first and `dropped` counts them.
inline constexpr size_t kMaxEntries = 200;
// One value's serialised size before it is cut to a preview.
inline constexpr size_t kMaxValueBytes = 512;
// One entry's serialised size, after which its values are cut harder.
inline constexpr size_t kMaxEntryBytes = 4096;
// Entries godot://project/journal answers with, newest first.
inline constexpr size_t kResourceEntries = 50;
// The blackboard's wait, for the same kind of lock (two processes, one file).
inline constexpr std::chrono::milliseconds kLockWait{5000};

// <project>/.didi/journal.json
std::filesystem::path journalPath(const std::filesystem::path& project_root);

// Whether a key names a secret: a password, passphrase, secret, token, API or
// private key, credential or authorization.
bool isSecretKey(std::string_view key);

// The value with every secret replaced by "[redacted]": the value of a key
// that names one, and the value beside a setting or property whose name names
// one, as in {"setting": "services/api_token", "value": ...}.
json redactSecrets(const json& value);

// A value whose serialised form is longer than max_bytes becomes
// {"truncated": true, "bytes": n, "preview": "<the first max_bytes>"}.
json boundedValue(const json& value, size_t max_bytes = kMaxValueBytes);

// What dispatch knows about a finished mutating call.
struct Call {
    std::string tool;
    json arguments = json::object();
    // The structured answer of a call that succeeded, or its error object.
    json answer = json::object();
    bool succeeded = true;
    json undo_steps = json::array();
    std::string execution_mode;
    std::string session_id;
};

// The entry a call becomes, before it has an id or a time. Redacted and
// bounded, so nothing larger or more secret than this reaches the file.
json entryFor(const Call& call);

// Adds an entry under the journal's lock and writes the file atomically.
// Returns the entry as stored, with its `id` and `at`. An entry carrying
// `undoes` marks that entry `undone_by` in the same write. A file that does
// not parse is moved aside and a new journal begun, saying where it went; a
// file in a newer format than this server writes is left alone and refused.
Result<json> append(const std::filesystem::path& project_root, json entry,
                    std::chrono::system_clock::time_point now = std::chrono::system_clock::now());

// The whole document: {format, next_id, dropped, entries}, oldest entry first.
// No journal yet reads as an empty one.
Result<json> load(const std::filesystem::path& project_root);

// One entry by id, or a 404 that says whether it was dropped or never existed.
Result<json> findEntry(const std::filesystem::path& project_root, int64_t id);

// The newest `limit` entries, newest first, each with `undo_state`: whether it
// can be undone on its own now, and if not, why. The editor behind `lease`
// judges every reference in one request; with no editor, an entry that has an
// undo reference reads "unknown". `compact` keeps only what a dashboard row
// shows. Always answers: an unreadable journal is reported in the view.
json view(const std::filesystem::path& project_root, const runtime::RuntimeRouteLease* lease,
          size_t limit = kResourceEntries, bool compact = false);

}  // namespace didi::mcp::journal
