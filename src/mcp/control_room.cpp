#include "didi/mcp/control_room.hpp"

#include "didi/mcp/tool_availability.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <utility>

namespace didi {
namespace mcp {

namespace {

// Truncate to a byte budget without splitting a character.
//
// The values clipped here are project paths, node names and Godot log lines,
// all of which can hold any UTF-8. Cutting one mid-sequence produces a string
// nlohmann::json refuses to serialise -- it throws type_error.316 -- so a byte
// -wise cut turns a project whose path puts a multi-byte character on the
// boundary into a dashboard that cannot be built at all.
//
// The input is valid UTF-8, so the byte at the cut is either a boundary already
// or a continuation byte (10xxxxxx) inside the sequence the cut would split.
// Walking back off the continuation bytes lands on that sequence's lead byte,
// which is the largest valid boundary at or below the limit.
std::string clip(std::string value, std::size_t limit) {
    if (value.size() <= limit) return value;
    std::size_t cut = limit;
    while (cut > 0 && (static_cast<unsigned char>(value[cut]) & 0xC0) == 0x80) --cut;
    value.resize(cut);
    value += "...";
    return value;
}

const char* levelName(LogLevel level) {
    switch (level) {
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info:  return "INFO";
        case LogLevel::Warn:  return "WARN";
        case LogLevel::Error: return "ERROR";
        case LogLevel::None:  break;
    }
    return "INFO";
}

json light(const char* label, const char* state, std::string value, std::string fact,
           std::string reason) {
    json entry = {{"label", label}, {"state", state}, {"value", std::move(value)}};
    if (!fact.empty()) entry["fact"] = clip(std::move(fact), kControlRoomMaxFactChars);
    if (!reason.empty()) entry["reason"] = clip(std::move(reason), kControlRoomMaxFactChars);
    return entry;
}

json fact(const char* label, std::string value) {
    return {{"label", label}, {"value", clip(std::move(value), kControlRoomMaxFactChars)}};
}

// Whether the attached bridge came out of this server's build.
//
// Unknown when nothing is attached, or when this server has no build identity
// of its own to compare against. An empty bridge identity is not unknown: it is
// a bridge older than the field, which is a mismatch.
std::optional<bool> bridgeBuildMatches(const ControlRoomInputs& in) {
    if (!in.connected || !in.bridge_build_id.has_value() || in.server_build_id.empty()) {
        return std::nullopt;
    }
    return *in.bridge_build_id == in.server_build_id;
}

std::string bridgeBuildReason(const ControlRoomInputs& in) {
    const std::string bridge = in.bridge_build_id.value_or("");
    return "The attached GDExtension is from a different build than this server (bridge " +
           (bridge.empty() ? std::string("older than this field") : bridge) + ", server " +
           in.server_build_id +
           "). Tool schemas come from the server and the answers come from the bridge, so a "
           "call can be refused for a reason the schema says is supported. Copy build/addons/didi "
           "into the project again and restart the editor.";
}

// The bridge light. Amber is the honest answer far more often than either of
// the others, and it always carries the reason it is amber.
json bridgeLight(const ControlRoomInputs& in) {
    if (in.connected) {
        const std::string kind = in.session_kind.value_or("unknown");
        if (kind == "unknown") {
            return light("Bridge", "warn", "Connected",
                         in.selected_session_id.value_or(""),
                         "The route is connected but did not report its kind.");
        }
        const auto matches = bridgeBuildMatches(in);
        if (matches.has_value() && !*matches) {
            return light("Bridge", "warn", "Build mismatch",
                         in.selected_session_id.value_or(""), bridgeBuildReason(in));
        }
        return light("Bridge", "ok", kind == "editor" ? "Editor attached" : "Game attached",
                     in.selected_session_id.value_or(""), "");
    }
    if (in.managed_unavailable) {
        return light("Bridge", "bad", "Route unreachable",
                     in.selected_session_id.value_or(""),
                     "A route is selected but did not produce an authenticated lease.");
    }
    if (in.descriptors_present) {
        return light("Bridge", "warn", "Detached", "",
                     "Godot published a session and no route is selected. Attach one.");
    }
    return light("Bridge", "bad", "No session", "",
                 "No published descriptor. Start Godot with the Didi addon enabled.");
}

json safetyLight(const ControlRoomInputs& in) {
    if (in.skip_confirmations) {
        return light("Safety", "bad", "Confirmations skipped", "",
                     "Every mutation executes without a second call.");
    }
    if (in.managed_recovery_armed) {
        return light("Safety", "warn", "Managed recovery armed", "",
                     "An authorized read may spend the one automatic editor restart.");
    }
    return light("Safety", "ok", "Confirmations enforced", "", "");
}

json projectLight(const ControlRoomInputs& in) {
    if (!in.project_readable) {
        return light("Project", "warn", "Root unreadable", in.project_root,
                     "The root resolved at startup is no longer readable.");
    }
    return light("Project", "ok", "Selected", in.project_root, "");
}

json workLight(const ControlRoomInputs& in) {
    if (!in.board_present) {
        return light("Work", "unknown", "No board", "",
                     "No blackboard exists in this project yet.");
    }
    return light("Work", "ok", "Board present", "",
                 "Call blackboard_task_list for its tasks; reading a board reclaims "
                 "lapsed leases, which this tool will not do.");
}

}  // namespace

// ---------------------------------------------------------------------------
// LogRing
// ---------------------------------------------------------------------------

LogRing::LogRing(std::size_t capacity)
    : m_capacity(capacity == 0 ? 1 : capacity) {}

bool LogRing::admits(LogLevel level) const {
    const LogLevel floor = m_minimumLevel.value_or(Logger::instance().getLevel());
    return level >= floor && floor != LogLevel::None;
}

void LogRing::setMinimumLevel(std::optional<LogLevel> level) {
    std::lock_guard<std::mutex> guard(m_mutex);
    m_minimumLevel = level;
}

void LogRing::record(LogLevel level, std::string_view tag, std::string_view message) {
    {
        std::lock_guard<std::mutex> guard(m_mutex);
        if (!admits(level)) return;
    }
    ControlRoomLogRecord entry;
    entry.time = controlRoomTimestamp();
    entry.level = levelName(level);
    entry.tag = clip(std::string(tag), 64);
    entry.message = clip(std::string(message), kControlRoomMaxMessageChars);

    std::lock_guard<std::mutex> guard(m_mutex);
    m_records.push_back(std::move(entry));
    while (m_records.size() > m_capacity) {
        m_records.pop_front();
        ++m_dropped;
    }
}

std::vector<ControlRoomLogRecord> LogRing::snapshot() const {
    std::lock_guard<std::mutex> guard(m_mutex);
    return {m_records.begin(), m_records.end()};
}

std::size_t LogRing::dropped() const {
    std::lock_guard<std::mutex> guard(m_mutex);
    return m_dropped;
}

void LogRing::install() {
    Logger::instance().setSink(
        [this](LogLevel level, std::string_view tag, std::string_view message) {
            record(level, tag, message);
        });
}

LogRing& controlRoomLogRing() {
    static LogRing ring;
    return ring;
}

void installControlRoomLogRing() {
    controlRoomLogRing().install();
}

// ---------------------------------------------------------------------------
// Model
// ---------------------------------------------------------------------------

std::string controlRoomTimestamp() {
    const auto now = std::chrono::system_clock::now();
    const auto seconds = std::chrono::system_clock::to_time_t(now);
    std::tm parts{};
#if defined(_WIN32)
    gmtime_s(&parts, &seconds);
#else
    gmtime_r(&seconds, &parts);
#endif
    std::ostringstream out;
    out << std::put_time(&parts, "%Y-%m-%dT%H:%M:%SZ");
    return out.str();
}

json buildControlRoomModel(const ControlRoomInputs& in,
                           const std::vector<ToolDefinition>& tools) {
    json model;
    model["captured_at"] = in.captured_at.empty() ? controlRoomTimestamp() : in.captured_at;
    model["server"] = {{"name", in.server_name}, {"version", in.server_version}};
    model["project"] = {{"root", clip(in.project_root, kControlRoomMaxFactChars)},
                        {"readable", in.project_readable}};

    model["lights"] = json::array({bridgeLight(in), projectLight(in), safetyLight(in),
                                   workLight(in)});

    // Tool rows. currentModeFor is the same function tools/list uses, so the
    // dashboard cannot report a mode discovery would not.
    int canonical = 0, implemented = 0, unimplemented = 0, legacy = 0, live_now = 0;
    json rows = json::array();
    for (const auto& tool : tools) {
        if (tool.legacy) {
            ++legacy;
        } else {
            ++canonical;
            if (tool.capability.implemented) ++implemented;
            else ++unimplemented;
        }
        const std::string mode = currentModeFor(tool.capability, tool.name, false, in.connected,
                                                in.session_kind, in.managed_unavailable);
        if (mode == "live") ++live_now;
        rows.push_back({{"name", tool.name},
                        {"mode", mode},
                        {"legacy", tool.legacy},
                        {"mutates", !tool.annotations.read_only}});
    }

    // The registry is a hash map, so its iteration order is arbitrary and can
    // differ between runs. A dashboard listing a hundred tools in no order is
    // unreadable, and an arbitrary order also means the row that gets cut when
    // the cap bites is arbitrary too. Sort first, then cut, so both the order
    // and the truncation are deterministic.
    std::sort(rows.begin(), rows.end(), [](const json& left, const json& right) {
        return left.value("name", "") < right.value("name", "");
    });
    if (rows.size() > kControlRoomMaxToolRows) {
        rows.erase(rows.begin() + static_cast<long>(kControlRoomMaxToolRows), rows.end());
    }
    model["tools"] = std::move(rows);
    model["surface"] = {{"canonical", canonical},
                        {"implemented", implemented},
                        {"unimplemented", unimplemented},
                        {"legacy", legacy},
                        {"live_now", live_now},
                        {"listed", model["tools"].size()},
                        {"truncated", static_cast<std::size_t>(canonical + legacy) >
                                          kControlRoomMaxToolRows}};

    // Sessions. An explicit allowlist -- never descriptor.toJson() -- so that a
    // field added to SessionDescriptor later cannot reach a client by accident.
    // The token is not in this list and must never be.
    json sessions = json::array();
    for (const auto& session : in.sessions) {
        if (sessions.size() >= kControlRoomMaxSessions) break;
        const auto& descriptor = session.descriptor;
        const bool selected = in.selected_session_id.has_value() &&
                              *in.selected_session_id == descriptor.session_id;
        json row = {{"session_id", descriptor.session_id},
                    {"kind", descriptor.kind},
                    {"pid", descriptor.pid},
                    {"project_path", clip(descriptor.project_path, kControlRoomMaxFactChars)},
                    {"protocol_version", descriptor.protocol_version},
                    {"started_at_ms", descriptor.started_at_ms},
                    {"stale", session.stale},
                    {"selected", selected}};
        // Absent when the extension that published this session is older than
        // the field, which the bridge light reports as a mismatch rather than
        // as a match nobody checked.
        if (!descriptor.build_id.empty()) row["build_id"] = descriptor.build_id;
        // Absent rather than false when it was not established. A dashboard that
        // renders unknown as dead is worse than one that says it does not know.
        if (session.alive.has_value()) row["alive"] = *session.alive;
        sessions.push_back(std::move(row));
    }
    model["sessions"] = std::move(sessions);
    if (in.selected_session_id.has_value()) {
        model["selected_session"] = *in.selected_session_id;
    }
    model["session_note"] =
        "A listed session is a descriptor Godot published. `alive` is what the "
        "scanner found for the process behind it and is omitted when that could "
        "not be established; a stale descriptor is one the scanner no longer "
        "trusts. Neither is a promise that the next call will reach the engine.";

    // Facts, for the overview table.
    json facts = json::array();
    facts.push_back(fact("Server", in.server_name + " " + in.server_version));
    if (!in.server_build_id.empty()) facts.push_back(fact("Server build", in.server_build_id));
    if (in.bridge_build_id.has_value()) {
        const auto matches = bridgeBuildMatches(in);
        const std::string bridge = in.bridge_build_id->empty()
                                       ? std::string("older than this field")
                                       : *in.bridge_build_id;
        facts.push_back(fact("Bridge build",
                             matches.has_value() && !*matches ? bridge + " (does not match)"
                                                              : bridge));
    }
    if (!in.protocol_versions.empty()) {
        std::ostringstream versions;
        for (std::size_t i = 0; i < in.protocol_versions.size(); ++i) {
            if (i) versions << ", ";
            versions << in.protocol_versions[i];
        }
        facts.push_back(fact("MCP revisions", versions.str()));
    }
    facts.push_back(fact("Project root", in.project_root));
    facts.push_back(fact("Route", in.connected
                                      ? ("connected " + in.session_kind.value_or("unknown"))
                                      : (in.managed_unavailable ? "selected, unreachable"
                                                                : "detached")));
    facts.push_back(fact("Confirmations", in.skip_confirmations ? "skipped" : "enforced"));
    facts.push_back(fact("Managed recovery", in.managed_recovery_armed ? "armed" : "off"));
    {
        std::ostringstream surface;
        surface << canonical << " canonical, " << implemented << " implemented, "
                << unimplemented << " reserved, " << legacy << " legacy, " << live_now
                << " live now";
        facts.push_back(fact("Surface", surface.str()));
    }
    model["facts"] = std::move(facts);

    // Log tail: the newest `log_limit` records, never more than the ring holds.
    json log = json::array();
    const std::size_t limit = std::min(in.log_limit, kControlRoomMaxLogRecords);
    const std::size_t begin = in.log.size() > limit ? in.log.size() - limit : 0;
    for (std::size_t i = begin; i < in.log.size(); ++i) {
        const auto& record = in.log[i];
        log.push_back({{"time", record.time},
                       {"level", record.level},
                       {"tag", record.tag},
                       {"message", clip(record.message, kControlRoomMaxMessageChars)}});
    }
    model["log"] = std::move(log);
    model["log_returned"] = model["log"].size();
    model["log_available"] = in.log.size();
    model["log_truncated"] = begin > 0;

    std::ostringstream note;
    note << "Didi's own diagnostics, which otherwise go only to this process's standard error. "
         << "Godot's logs are separate: use runtime_read_logs and runtime_read_output.";
    if (in.log_dropped > 0) {
        note << " " << in.log_dropped << " older record(s) dropped.";
    }
    model["log_note"] = note.str();

    return model;
}

}  // namespace mcp
}  // namespace didi
