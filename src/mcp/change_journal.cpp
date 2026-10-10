#include "didi/mcp/change_journal.hpp"

#include "didi/common/atomic_write.hpp"
#include "didi/runtime/session_client.hpp"
#include "didi/common/file_lock.hpp"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <sstream>
#include <system_error>
#include <utility>

namespace didi::mcp::journal {

namespace {

std::string lowered(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

// Dumped with invalid UTF-8 replaced: a project can hold a file name that is
// not valid UTF-8 (#650), and the journal must never throw over one.
std::string dumped(const json& value) {
    return value.dump(-1, ' ', false, json::error_handler_t::replace);
}

// The first max bytes of text, cut where a UTF-8 character begins.
std::string utf8Prefix(const std::string& text, size_t max) {
    if (text.size() <= max) return text;
    size_t end = max;
    while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) --end;
    return text.substr(0, end);
}

// Envelope fields every answer can carry. They describe the call, not the
// change, and the journal already records the call.
constexpr const char* kEnvelopeKeys[] = {
    "execution_mode", "is_live_engine", "session",          "follow_up",
    "transport",      "offline_reason", "status",           "_meta",
    "limitation",     "warnings",       "confirmation",     "recovery"};

// Arguments that steer the call rather than name the change.
constexpr const char* kSteeringArguments[] = {
    "confirmation_token", "dry_run", "_meta", "request_id", "fields"};

// The arguments that say what a call was aimed at, in the order a person
// reading the journal looks for them.
constexpr const char* kTargetKeys[] = {
    "target_node", "node_path",     "parent_path", "scene_path",   "script_path",
    "path",        "file_path",     "save_path",   "resource_path", "asset_path",
    "setting",     "name",          "action",      "bus_name",     "bus",
    "preset",      "library_name",  "group",       "signal",       "method_name",
    "property_name", "journal_entry"};

// Answer fields that name a file the call wrote.
constexpr const char* kWrittenFileKeys[] = {
    "resource_file", "layout_path", "project_layout_path", "library_path", "written_to",
    "save_path",     "output_path", "asset_path",          "script_path",  "file_path"};

// The values beside a setting or property name that the name governs.
constexpr const char* kGovernedValueKeys[] = {
    "value", "old_value", "previous", "previous_value", "before", "after",
    "new",   "old",       "requested_value", "current", "default"};

constexpr const char* kNameKeys[] = {"setting", "name", "property", "property_name",
                                                  "key"};

bool contains(const auto& keys, std::string_view key) {
    return std::any_of(std::begin(keys), std::end(keys),
                       [key](const char* candidate) { return key == candidate; });
}

json withoutKeys(const json& object, const auto& keys) {
    if (!object.is_object()) return object;
    json out = json::object();
    for (const auto& [key, value] : object.items()) {
        if (!contains(keys, key)) out[key] = value;
    }
    return out;
}

json boundedMembers(const json& object, size_t max) {
    if (!object.is_object()) return boundedValue(object, max);
    json out = json::object();
    for (const auto& [key, value] : object.items()) out[key] = boundedValue(value, max);
    return out;
}

// The value before the change, where the answer reports one. A batch reports
// it per write.
json beforeOf(const json& answer) {
    if (!answer.is_object()) return nullptr;
    if (const auto writes = answer.find("writes"); writes != answer.end() && writes->is_array()) {
        json before = json::array();
        for (const auto& write : *writes) {
            if (!write.is_object() || !write.contains("old_value")) continue;
            json item = {{"old_value", write["old_value"]}};
            for (const char* key : {"target_node", "property"}) {
                if (write.contains(key)) item[key] = write[key];
            }
            before.push_back(std::move(item));
        }
        if (!before.empty()) return before;
    }
    for (const char* key : {"old_value", "before", "previous_value", "previous", "old"}) {
        if (const auto found = answer.find(key); found != answer.end()) return *found;
    }
    return nullptr;
}

// Whether the answer says the scene it names was written to disk.
bool sceneWritten(const json& answer) {
    return answer.value("scene_saved", false) == true || answer.contains("file_bytes") ||
           answer.value("saved", false) == true || answer.value("created", false) == true;
}

json filesOf(const json& answer) {
    json files = json::array();
    const auto add = [&files](const json& value) {
        if (!value.is_string() || value.get_ref<const std::string&>().empty()) return;
        if (std::find(files.begin(), files.end(), value) == files.end()) files.push_back(value);
    };
    if (!answer.is_object()) return files;
    for (const char* key : kWrittenFileKeys) {
        if (const auto found = answer.find(key); found != answer.end()) add(*found);
    }
    if (sceneWritten(answer)) {
        if (const auto found = answer.find("scene_path"); found != answer.end()) add(*found);
    }
    for (const char* key : {"updated_files", "written"}) {
        const auto list = answer.find(key);
        if (list == answer.end() || !list->is_array()) continue;
        for (const auto& item : *list) {
            add(item.is_object() ? item.value("path", json()) : item);
        }
    }
    return files;
}

std::string isoTime(std::chrono::system_clock::time_point now) {
    const auto seconds = std::chrono::system_clock::to_time_t(now);
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(
                            now.time_since_epoch()).count() % 1000;
    std::tm utc{};
#if defined(_WIN32)
    gmtime_s(&utc, &seconds);
#else
    gmtime_r(&seconds, &utc);
#endif
    std::ostringstream out;
    out << std::put_time(&utc, "%Y-%m-%dT%H:%M:%S") << '.' << std::setfill('0') << std::setw(3)
        << millis << 'Z';
    return out.str();
}

json emptyDocument() {
    return {{"format", kFormat}, {"next_id", 1}, {"dropped", 0}, {"entries", json::array()}};
}

Result<std::string> readText(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return Error(500, "The journal cannot be opened for reading.");
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (in.bad()) return Error(500, "The journal could not be read to the end.");
    return text;
}

// A document as written by this format, or the reason it is not one.
Result<json> parseDocument(const std::string& text) {
    auto doc = json::parse(text, nullptr, false);
    if (doc.is_discarded() || !doc.is_object()) {
        return Error(500, "The journal is not a JSON object.", {{"code", "journal_unreadable"}});
    }
    const auto format = doc.value("format", json());
    if (!format.is_number_integer()) {
        return Error(500, "The journal names no format.", {{"code", "journal_unreadable"}});
    }
    if (format.get<int64_t>() > kFormat) {
        return Error(409,
                     "The journal is in format " + std::to_string(format.get<int64_t>()) +
                         ", newer than the format " + std::to_string(kFormat) +
                         " this server writes. It was left as it is.",
                     {{"code", "journal_format_newer"}});
    }
    if (!doc.value("entries", json()).is_array() || !doc.value("next_id", json()).is_number_integer()) {
        return Error(500, "The journal has no entries list or next id.", {{"code", "journal_unreadable"}});
    }
    if (!doc.value("dropped", json()).is_number_integer()) doc["dropped"] = 0;
    return doc;
}

}  // namespace

std::filesystem::path journalPath(const std::filesystem::path& project_root) {
    return project_root / ".didi" / "journal.json";
}

bool isSecretKey(std::string_view key) {
    static constexpr const char* kSecretWords[] = {
        "password", "passwd",     "passphrase",  "secret",      "token",
        "apikey",   "api_key",    "api-key",     "privatekey",  "private_key",
        "private-key", "credential", "authorization", "access_key"};
    const auto text = lowered(key);
    return std::any_of(std::begin(kSecretWords), std::end(kSecretWords),
                       [&text](const char* word) { return text.find(word) != std::string::npos; });
}

json redactSecrets(const json& value) {
    if (value.is_array()) {
        json out = json::array();
        for (const auto& item : value) out.push_back(redactSecrets(item));
        return out;
    }
    if (!value.is_object()) return value;
    bool governed_secret = false;
    for (const char* name_key : kNameKeys) {
        const auto name = value.find(name_key);
        if (name != value.end() && name->is_string() && isSecretKey(name->get_ref<const std::string&>())) {
            governed_secret = true;
        }
    }
    json out = json::object();
    for (const auto& [key, item] : value.items()) {
        if (isSecretKey(key) || (governed_secret && contains(kGovernedValueKeys, key))) {
            out[key] = "[redacted]";
        } else {
            out[key] = redactSecrets(item);
        }
    }
    return out;
}

json boundedValue(const json& value, size_t max_bytes) {
    const auto text = dumped(value);
    if (text.size() <= max_bytes) return value;
    return {{"truncated", true}, {"bytes", text.size()}, {"preview", utf8Prefix(text, max_bytes)}};
}

json entryFor(const Call& call) {
    json entry = {{"tool", call.tool}, {"outcome", call.succeeded ? "applied" : "failed"}};
    if (!call.execution_mode.empty()) entry["execution_mode"] = call.execution_mode;
    if (!call.session_id.empty()) entry["session"] = call.session_id;

    const json arguments = redactSecrets(withoutKeys(call.arguments, kSteeringArguments));
    json target = json::object();
    if (arguments.is_object()) {
        for (const char* key : kTargetKeys) {
            const auto found = arguments.find(key);
            if (found == arguments.end() || found->is_object() || found->is_array()) continue;
            target[key] = boundedValue(*found, 200);
            if (target.size() == 3) break;
        }
        if (const auto writes = arguments.find("writes"); writes != arguments.end() && writes->is_array()) {
            target["writes"] = writes->size();
        }
    }
    entry["target"] = std::move(target);
    entry["arguments"] = boundedMembers(arguments, kMaxValueBytes);

    const json answer = redactSecrets(call.answer);
    if (call.succeeded) {
        if (auto before = beforeOf(answer); !before.is_null()) {
            entry["before"] = boundedValue(before, kMaxValueBytes);
        }
        entry["after"] = boundedMembers(withoutKeys(answer, kEnvelopeKeys), kMaxValueBytes);
        entry["files"] = filesOf(answer);
    } else {
        const json error = answer.is_object() ? answer : json::object();
        json failure = {{"code", error.value("code", json())},
                        {"message", boundedValue(error.value("message", json()), 300)}};
        const auto data = error.value("data", json::object());
        if (data.is_object() && data.contains("code")) failure["reason"] = data["code"];
        entry["error"] = std::move(failure);
        entry["files"] = json::array();
    }

    if (call.undo_steps.is_array() && !call.undo_steps.empty()) {
        entry["undo"] = call.undo_steps;
        for (const auto& step : call.undo_steps) {
            if (step.is_object() && step.value("scene_path", json()).is_string()) {
                entry["scene"] = step["scene_path"];
                break;
            }
        }
    } else {
        entry["undo"] = nullptr;
        entry["undo_note"] =
            call.tool == "editor_undo" || call.tool == "editor_redo"
                ? "An undo or redo moves the editor's history; the other one reverses it."
            : call.execution_mode == "live"
                ? "The call registered no step in the editor's undo history."
                : "Written without an editor, so no undo history holds it.";
    }
    if (!entry.contains("scene") && answer.is_object() && !sceneWritten(answer) &&
        answer.value("scene_path", json()).is_string()) {
        entry["scene"] = answer["scene_path"];
    }
    if (call.succeeded && call.tool == "editor_undo" && call.arguments.is_object()) {
        if (const auto undone = call.arguments.find("journal_entry");
            undone != call.arguments.end() && undone->is_number_integer()) {
            entry["undoes"] = *undone;
        }
    }

    // A last bound on the whole entry, because a call with many large values
    // can pass every per-value bound and still be large.
    if (dumped(entry).size() > kMaxEntryBytes) {
        for (const char* key : {"after", "arguments", "before"}) {
            if (entry.contains(key)) entry[key] = boundedValue(entry[key], 600);
            if (dumped(entry).size() <= kMaxEntryBytes) break;
        }
    }
    return entry;
}

Result<json> load(const std::filesystem::path& project_root) {
    const auto file = journalPath(project_root);
    std::error_code error;
    if (!std::filesystem::exists(file, error)) return emptyDocument();
    auto text = readText(file);
    if (text.isErr()) return text.error();
    return parseDocument(text.value());
}

Result<json> findEntry(const std::filesystem::path& project_root, int64_t id) {
    auto doc = load(project_root);
    if (doc.isErr()) return doc.error();
    for (const auto& entry : doc.value()["entries"]) {
        if (entry.value("id", int64_t{0}) == id) return entry;
    }
    const auto next = doc.value().value("next_id", int64_t{1});
    const auto& entries = doc.value()["entries"];
    const auto oldest = entries.empty() ? next : entries.front().value("id", next);
    if (id >= 1 && id < oldest) {
        return Error(404,
                     "Journal entry " + std::to_string(id) + " was dropped: the journal keeps the "
                     "last " + std::to_string(kMaxEntries) + " entries, and the oldest now is " +
                         std::to_string(oldest) + ".",
                     {{"code", "journal_entry_not_found"}, {"oldest_entry", oldest}});
    }
    return Error(404, "No journal entry " + std::to_string(id) + " exists in this project.",
                 {{"code", "journal_entry_not_found"}, {"next_entry", next}});
}

Result<json> append(const std::filesystem::path& project_root, json entry,
                    std::chrono::system_clock::time_point now) {
    const auto file = journalPath(project_root);
    std::error_code error;
    std::filesystem::create_directories(file.parent_path(), error);
    if (error) return Error(500, "The project's .didi directory cannot be created: " + error.message());

    auto lock = files::FileLock::acquireWithin(file.parent_path() / "journal.lock",
                                                           json::object(), kLockWait);
    if (lock.isErr()) {
        return Error(500, "The journal lock stayed held for the whole " +
                              std::to_string(kLockWait.count()) + " ms wait.");
    }

    json doc = emptyDocument();
    if (std::filesystem::exists(file, error)) {
        auto text = readText(file);
        if (text.isErr()) return text.error();
        auto parsed = parseDocument(text.value());
        if (parsed.isOk()) {
            doc = std::move(parsed.value());
        } else if (parsed.error().code == 409) {
            return parsed.error();
        } else {
            // Kept, not overwritten: a journal is a record, and a damaged one
            // may still be read by a person.
            const auto stamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   now.time_since_epoch()).count();
            const auto aside = file.parent_path() / ("journal.unreadable-" + std::to_string(stamp) + ".json");
            if (auto moved = files::renameWithRetry(file, aside, files::kReplaceRetryBudget); moved) {
                return Error(500, "The journal does not parse and could not be moved aside: " +
                                      moved.message());
            }
            doc["recovered"] = {{"moved_to", aside.filename().string()},
                                {"reason", parsed.error().message},
                                {"at", isoTime(now)}};
        }
    }

    const int64_t id = std::max<int64_t>(doc.value("next_id", int64_t{1}), 1);
    entry["id"] = id;
    entry["at"] = isoTime(now);
    auto& entries = doc["entries"];
    if (const auto undone = entry.find("undoes"); undone != entry.end() && undone->is_number_integer()) {
        for (auto& earlier : entries) {
            if (earlier.value("id", int64_t{0}) == undone->get<int64_t>()) earlier["undone_by"] = id;
        }
    }
    entries.push_back(entry);
    while (entries.size() > kMaxEntries) {
        entries.erase(entries.begin());
        doc["dropped"] = doc.value("dropped", int64_t{0}) + 1;
    }
    doc["next_id"] = id + 1;
    auto written = files::writeFileAtomically(file, dumped(doc));
    if (written.isErr()) return written.error();
    return entry;
}

json view(const std::filesystem::path& project_root, const runtime::RuntimeRouteLease* lease,
          size_t limit, bool compact) {
    json out = {{"entries", json::array()}, {"total", 0}, {"truncated", false}};
    auto doc = load(project_root);
    if (doc.isErr()) {
        out["status"] = "unreadable";
        out["error"] = {{"code", doc.error().code},
                        {"message", doc.error().message},
                        {"reason", doc.error().data.is_object()
                                       ? doc.error().data.value("code", json())
                                       : json()}};
        return out;
    }
    const auto& entries = doc.value()["entries"];
    out["total"] = entries.size();
    out["dropped"] = doc.value().value("dropped", 0);
    out["next_id"] = doc.value().value("next_id", 1);
    if (doc.value().contains("recovered")) out["recovered"] = doc.value()["recovered"];
    json newest = json::array();
    for (auto it = entries.rbegin(); it != entries.rend() && newest.size() < limit; ++it) {
        newest.push_back(*it);
    }
    out["truncated"] = entries.size() > newest.size();

    // Every step of every listed entry, judged in one request.
    json refs = json::array();
    for (const auto& entry : newest) {
        const auto undo = entry.value("undo", json());
        if (!undo.is_array()) continue;
        for (const auto& step : undo) refs.push_back(step);
    }
    json states;
    json judged = {{"judged", false}};
    if (refs.empty()) {
        judged["reason"] = "No listed entry has an undo reference.";
    } else if (!lease || !lease->descriptor.has_value()) {
        judged["reason"] = "No editor is attached to judge the entries against.";
    } else if (lease->descriptor->kind != "editor") {
        judged["reason"] = "The attached session is a game, which has no editor history.";
    } else {
        auto sent = runtime::sendLiveRouteRequest(*lease, "editor.undoStatus", {{"refs", refs}},
                                                  5000, true);
        if (sent.response.isOk() && sent.response.value().value("states", json()).is_array() &&
            sent.response.value()["states"].size() == refs.size()) {
            states = sent.response.value()["states"];
            judged = {{"judged", true}, {"run", sent.response.value().value("run", json())}};
        } else {
            judged["reason"] = sent.response.isErr()
                                   ? "The editor did not judge them: " + sent.response.error().message
                                   : std::string("The editor's answer did not judge every reference.");
        }
    }
    out["editor"] = judged;

    size_t next_ref = 0;
    for (auto& entry : newest) {
        const auto undo = entry.value("undo", json());
        json state;
        if (!undo.is_array() || undo.empty()) {
            state = {{"state", "none"}, {"reason", entry.value("undo_note", std::string())}};
        } else if (states.is_array()) {
            // The newest step is the one an undo starts with, and an entry
            // undoes only if every step does in turn.
            json steps = json::array();
            for (size_t i = 0; i < undo.size(); ++i) steps.push_back(states[next_ref + i]);
            state = steps.back();
            if (undo.size() > 1) state["steps"] = std::move(steps);
        } else {
            state = {{"state", "unknown"}, {"reason", judged.value("reason", std::string())}};
        }
        if (undo.is_array()) next_ref += undo.size();
        entry["undo_state"] = std::move(state);
        if (compact) {
            json row = json::object();
            for (const char* key : {"id", "at", "tool", "outcome", "target", "scene", "undone_by"}) {
                if (entry.contains(key)) row[key] = entry[key];
            }
            row["undo"] = entry["undo_state"].value("state", std::string("none"));
            entry = std::move(row);
        }
    }
    out["entries"] = std::move(newest);
    return out;
}

}  // namespace didi::mcp::journal
