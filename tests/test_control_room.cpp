// Control Room model and log ring.
//
// Every test here is one of the adversarial requirements in
// docs/CONTROL_ROOM_DESIGN.md section 6, and each fails if its guard is removed.

#include "didi/mcp/control_room.hpp"
#include "didi/mcp/tool_availability.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <functional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#define ASSERT_TRUE(cond) if (!(cond)) throw std::runtime_error("Assertion failed: " #cond);
#define ASSERT_EQ(a, b) ASSERT_TRUE((a) == (b))
#define ASSERT_FALSE(cond) ASSERT_TRUE(!(cond))

void registerTest(const std::string& name, std::function<void()> fn);

namespace {

using namespace didi;
using namespace didi::mcp;

ToolDefinition makeTool(const std::string& name, bool implemented, bool read_only,
                        std::vector<std::string> modes) {
    ToolDefinition tool;
    tool.name = name;
    tool.description = "test";
    tool.inputSchema = {{"type", "object"}};
    tool.capability = {std::move(modes), implemented, {}};
    tool.annotations.read_only = read_only;
    tool.annotations.destructive = !read_only;
    return tool;
}

// Deliberately carries a token. The model must never emit one even when the
// input it is built from has one, which is what makes the A1 test adversarial
// rather than merely descriptive.
ControlRoomSession makeSession(const std::string& id, const std::string& kind) {
    runtime::SessionDescriptor descriptor;
    descriptor.session_id = id;
    descriptor.kind = kind;
    descriptor.pid = 4242;
    descriptor.project_path = "C:/projects/game";
    descriptor.endpoint = "didi-endpoint-name";
    descriptor.protocol_version = "1";
    descriptor.started_at_ms = 1700000000000;
    // The thing that must never come out the other side.
    descriptor.token = "SUPERSECRETSESSIONTOKEN0123456789";
    ControlRoomSession session;
    session.descriptor = descriptor;
    session.alive = true;
    return session;
}

std::string lightState(const json& model, const std::string& label) {
    for (const auto& entry : model.at("lights")) {
        if (entry.value("label", "") == label) return entry.value("state", "");
    }
    return "<missing>";
}

std::string factValue(const json& model, const std::string& label) {
    for (const auto& entry : model.at("facts")) {
        if (entry.value("label", "") == label) return entry.value("value", "");
    }
    return "<missing>";
}

std::string lightReason(const json& model, const std::string& label) {
    for (const auto& entry : model.at("lights")) {
        if (entry.value("label", "") == label) return entry.value("reason", "");
    }
    return "";
}

// -----------------------------------------------------------------------
// A1. No secret reaches the page.
// -----------------------------------------------------------------------
void test_no_session_token_reaches_the_model() {
    ControlRoomInputs in;
    in.sessions = {makeSession("aaaabbbbccccddddeeeeffff00001111", "editor")};
    in.selected_session_id = "aaaabbbbccccddddeeeeffff00001111";
    in.connected = true;
    in.session_kind = "editor";

    const auto model = buildControlRoomModel(in, {});
    const std::string dumped = model.dump();

    // The value itself.
    ASSERT_TRUE(dumped.find("SUPERSECRETSESSIONTOKEN") == std::string::npos);
    // And the field name, so a future descriptor field called token cannot be
    // added to the allowlist without this failing first.
    std::string lowered = dumped;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    ASSERT_TRUE(lowered.find("token") == std::string::npos);

    // The endpoint is not a secret, but it is not useful to a client either and
    // naming pipes in a rendered page invites someone to connect to one.
    ASSERT_TRUE(dumped.find("didi-endpoint-name") == std::string::npos);

    // What a client does get.
    ASSERT_EQ(model["sessions"].size(), 1u);
    ASSERT_EQ(model["sessions"][0]["session_id"], "aaaabbbbccccddddeeeeffff00001111");
    ASSERT_EQ(model["sessions"][0]["kind"], "editor");
    ASSERT_EQ(model["sessions"][0]["pid"], 4242);
    ASSERT_TRUE(model["sessions"][0]["selected"].get<bool>());
}

// -----------------------------------------------------------------------
// Sessions are read from the payload a client actually gets.
//
// Found live rather than in a test: the dashboard reported no sessions while an
// editor was plainly attached. The first implementation parsed each entry with
// SessionDescriptor::fromJson, which requires the session token -- and
// runtime_list_sessions strips it, exactly as it should. Every entry failed to
// parse and was silently dropped.
// -----------------------------------------------------------------------
void test_listed_sessions_parse_without_a_token() {
    // The shape runtime_list_sessions really returns. No token: that is the point.
    const json payload = {
        {"execution_mode", "local_session_management"},
        {"diagnostics", json::array()},
        {"sessions", json::array({
            {{"session_id", "efce6dcccfca62699ee60536e3fc709d"},
             {"kind", "editor"},
             {"pid", 36404},
             {"project_path", "D:/projects/game"},
             {"protocol_version", "1.3"},
             {"schema_version", 1},
             {"started_at_ms", 1788828033158},
             {"alive", true},
             {"stale", false},
             {"endpoint", "\\\\.\\pipe\\godot_didi_abc"}},
            {{"session_id", "00000000000000000000000000000002"},
             {"kind", "game"},
             {"pid", 2},
             {"project_path", "D:/projects/game"},
             {"protocol_version", "1.3"},
             {"started_at_ms", 2},
             {"alive", false},
             {"stale", true}},
        })}
    };

    const auto parsed = parseListedSessions(payload);
    ASSERT_EQ(parsed.size(), 2u);
    ASSERT_EQ(parsed[0].descriptor.session_id, "efce6dcccfca62699ee60536e3fc709d");
    ASSERT_EQ(parsed[0].descriptor.kind, "editor");
    ASSERT_EQ(parsed[0].descriptor.pid, 36404u);
    ASSERT_TRUE(parsed[0].alive.has_value() && *parsed[0].alive);
    ASSERT_FALSE(parsed[0].stale);
    ASSERT_TRUE(parsed[1].alive.has_value() && !*parsed[1].alive);
    ASSERT_TRUE(parsed[1].stale);

    // The endpoint is read by nobody, so it cannot be forwarded by accident.
    ASSERT_TRUE(parsed[0].descriptor.endpoint.empty());

    // And it survives the whole way to the model.
    ControlRoomInputs in;
    in.sessions = parsed;
    in.selected_session_id = "efce6dcccfca62699ee60536e3fc709d";
    const auto model = buildControlRoomModel(in, {});
    ASSERT_EQ(model["sessions"].size(), 2u);
    ASSERT_TRUE(model["sessions"][0]["selected"].get<bool>());
    ASSERT_TRUE(model["sessions"][0]["alive"].get<bool>());
    ASSERT_TRUE(model["sessions"][1]["stale"].get<bool>());
    ASSERT_TRUE(model.dump().find("pipe") == std::string::npos);
}

void test_unknown_liveness_is_omitted_not_guessed() {
    const json payload = {{"sessions", json::array({
        {{"session_id", "aaaa"}, {"kind", "editor"}, {"pid", 7}},
    })}};
    const auto parsed = parseListedSessions(payload);
    ASSERT_EQ(parsed.size(), 1u);
    ASSERT_FALSE(parsed[0].alive.has_value());

    ControlRoomInputs in;
    in.sessions = parsed;
    const auto model = buildControlRoomModel(in, {});
    // Absent, rather than false. Rendering unknown as dead is a lie.
    ASSERT_FALSE(model["sessions"][0].contains("alive"));
}

void test_malformed_session_entries_are_skipped_not_fatal() {
    const json payload = {{"sessions", json::array({
        json("not an object"),
        json::object(),                                   // no session_id
        {{"session_id", ""}},                             // empty session_id
        {{"session_id", "good"}, {"kind", "editor"}},
    })}};
    const auto parsed = parseListedSessions(payload);
    ASSERT_EQ(parsed.size(), 1u);
    ASSERT_EQ(parsed[0].descriptor.session_id, "good");

    // A payload with no sessions key at all, and a non-object payload.
    ASSERT_EQ(parseListedSessions(json::object()).size(), 0u);
    ASSERT_EQ(parseListedSessions(json("nonsense")).size(), 0u);
    ASSERT_EQ(parseListedSessions(json{{"sessions", "not an array"}}).size(), 0u);
}

// -----------------------------------------------------------------------
// A3. The payload is bounded.
// -----------------------------------------------------------------------
void test_the_payload_is_bounded() {
    ControlRoomInputs in;
    for (std::size_t i = 0; i < kControlRoomMaxSessions + 25; ++i) {
        in.sessions.push_back(makeSession("session" + std::to_string(i), "game"));
    }
    for (std::size_t i = 0; i < kControlRoomMaxLogRecords + 300; ++i) {
        in.log.push_back({"t", "INFO", "TAG", "message " + std::to_string(i)});
    }

    std::vector<ToolDefinition> tools;
    for (std::size_t i = 0; i < kControlRoomMaxToolRows + 120; ++i) {
        tools.push_back(makeTool("tool_" + std::to_string(i), true, true, {"offline_fallback"}));
    }

    in.log_limit = kControlRoomMaxLogRecords;
    const auto model = buildControlRoomModel(in, tools);
    ASSERT_EQ(model["sessions"].size(), kControlRoomMaxSessions);
    ASSERT_EQ(model["log"].size(), kControlRoomMaxLogRecords);
    ASSERT_EQ(model["tools"].size(), kControlRoomMaxToolRows);
    // Truncation is disclosed rather than hidden, and the counts still describe
    // the whole surface rather than only the rows that fitted.
    ASSERT_TRUE(model["surface"]["truncated"].get<bool>());
    ASSERT_EQ(model["surface"]["canonical"], static_cast<int>(kControlRoomMaxToolRows + 120));

    // The newest records survive; the oldest are the ones dropped.
    ASSERT_EQ(model["log"].back()["message"],
              "message " + std::to_string(kControlRoomMaxLogRecords + 299));
}

// Clipping runs on bytes, and a project path or a Godot log line can hold any
// UTF-8. Cutting one mid-sequence leaves a string nlohmann::json refuses to
// serialise, so the whole dashboard would fail for a project whose path happens
// to put a multi-byte character on the boundary.
void test_clipping_never_splits_a_utf8_sequence() {
    // Two bytes for the e-acute, positioned so a byte-wise cut at the fact limit
    // lands between them.
    const std::string accent = "\xc3\xa9";
    for (std::size_t lead = kControlRoomMaxFactChars - 4; lead < kControlRoomMaxFactChars + 4;
         ++lead) {
        ControlRoomInputs in;
        in.project_root = std::string(lead, 'a') + accent + std::string(64, 'b');

        // Four bytes for an emoji, to catch a cut one, two or three bytes in.
        const std::string emoji = "\xf0\x9f\x8e\xae";
        in.log.push_back({"t", "INFO", "TAG",
                          std::string(lead, 'm') + emoji + std::string(64, 'n')});

        const auto model = buildControlRoomModel(in, {});
        // dump() throws type_error.316 on invalid UTF-8, which is exactly the
        // failure this guards. Serialising is the assertion.
        const std::string dumped = model.dump();
        ASSERT_TRUE(!dumped.empty());
    }
}

// The result reaches the model as well as the page, so the log is a glance by
// default and the whole ring only on request.
void test_log_budget_defaults_low_and_discloses_truncation() {
    ControlRoomInputs in;
    for (std::size_t i = 0; i < kControlRoomMaxLogRecords; ++i) {
        in.log.push_back({"t", "INFO", "TAG", "message " + std::to_string(i)});
    }

    const auto glance = buildControlRoomModel(in, {});
    ASSERT_EQ(glance["log"].size(), kControlRoomDefaultLogRecords);
    ASSERT_EQ(glance["log_returned"], kControlRoomDefaultLogRecords);
    ASSERT_EQ(glance["log_available"], kControlRoomMaxLogRecords);
    ASSERT_TRUE(glance["log_truncated"].get<bool>());
    // The newest records are the ones kept.
    ASSERT_EQ(glance["log"].back()["message"],
              "message " + std::to_string(kControlRoomMaxLogRecords - 1));

    in.log_limit = kControlRoomMaxLogRecords;
    const auto full = buildControlRoomModel(in, {});
    ASSERT_EQ(full["log"].size(), kControlRoomMaxLogRecords);
    ASSERT_FALSE(full["log_truncated"].get<bool>());

    // A request for more than the ring holds is clamped, not honoured.
    in.log_limit = kControlRoomMaxLogRecords * 10;
    const auto clamped = buildControlRoomModel(in, {});
    ASSERT_EQ(clamped["log"].size(), kControlRoomMaxLogRecords);

    in.log_limit = 0;
    const auto none = buildControlRoomModel(in, {});
    ASSERT_EQ(none["log"].size(), 0u);
    ASSERT_TRUE(none["log_truncated"].get<bool>());
}

void test_long_strings_are_clipped() {
    ControlRoomInputs in;
    in.project_root = std::string(kControlRoomMaxFactChars + 500, 'p');
    in.log.push_back({"t", "INFO", "TAG", std::string(kControlRoomMaxMessageChars + 500, 'm')});

    const auto model = buildControlRoomModel(in, {});
    ASSERT_TRUE(model["project"]["root"].get<std::string>().size() <=
                kControlRoomMaxFactChars + 3);
    ASSERT_TRUE(model["log"][0]["message"].get<std::string>().size() <=
                kControlRoomMaxMessageChars + 3);
}

// -----------------------------------------------------------------------
// A6. The lights cannot lie.
// -----------------------------------------------------------------------
void test_bridge_light_states() {
    // Nothing published at all: red, and it says what to start.
    {
        ControlRoomInputs in;
        const auto model = buildControlRoomModel(in, {});
        ASSERT_EQ(lightState(model, "Bridge"), "bad");
        ASSERT_TRUE(lightReason(model, "Bridge").find("addon") != std::string::npos);
    }
    // Published but not selected: amber, never red, because the bridge is there.
    {
        ControlRoomInputs in;
        in.descriptors_present = true;
        in.sessions = {makeSession("s1", "editor")};
        const auto model = buildControlRoomModel(in, {});
        ASSERT_EQ(lightState(model, "Bridge"), "warn");
        ASSERT_TRUE(!lightReason(model, "Bridge").empty());
    }
    // Selected but no authenticated lease: red, and that is a fact about the route.
    {
        ControlRoomInputs in;
        in.descriptors_present = true;
        in.managed_unavailable = true;
        in.selected_session_id = "s1";
        const auto model = buildControlRoomModel(in, {});
        ASSERT_EQ(lightState(model, "Bridge"), "bad");
        ASSERT_TRUE(lightReason(model, "Bridge").find("lease") != std::string::npos);
    }
    // Connected editor and connected game are both green.
    for (const char* kind : {"editor", "game"}) {
        ControlRoomInputs in;
        in.connected = true;
        in.session_kind = kind;
        in.descriptors_present = true;
        const auto model = buildControlRoomModel(in, {});
        ASSERT_EQ(lightState(model, "Bridge"), "ok");
    }
    // Connected but the route would not say what it is: amber, not green. An
    // unknown is never rendered as a success.
    {
        ControlRoomInputs in;
        in.connected = true;
        const auto model = buildControlRoomModel(in, {});
        ASSERT_EQ(lightState(model, "Bridge"), "warn");
        ASSERT_TRUE(lightReason(model, "Bridge").find("kind") != std::string::npos);
    }
}

void test_safety_and_project_and_work_lights() {
    {
        ControlRoomInputs in;
        const auto model = buildControlRoomModel(in, {});
        ASSERT_EQ(lightState(model, "Safety"), "ok");
        ASSERT_EQ(lightState(model, "Project"), "ok");
        ASSERT_EQ(lightState(model, "Work"), "unknown");
    }
    {
        ControlRoomInputs in;
        in.skip_confirmations = true;
        // YOLO outranks managed mode: the more dangerous fact is the one shown.
        in.managed_recovery_armed = true;
        const auto model = buildControlRoomModel(in, {});
        ASSERT_EQ(lightState(model, "Safety"), "bad");
        ASSERT_EQ(factValue(model, "Confirmations"), "skipped");
    }
    {
        ControlRoomInputs in;
        in.managed_recovery_armed = true;
        const auto model = buildControlRoomModel(in, {});
        ASSERT_EQ(lightState(model, "Safety"), "warn");
        ASSERT_TRUE(lightReason(model, "Safety").find("restart") != std::string::npos);
    }
    {
        ControlRoomInputs in;
        in.project_readable = false;
        const auto model = buildControlRoomModel(in, {});
        ASSERT_EQ(lightState(model, "Project"), "warn");
    }
    {
        ControlRoomInputs in;
        in.board_present = true;
        const auto model = buildControlRoomModel(in, {});
        ASSERT_EQ(lightState(model, "Work"), "ok");
    }
}

// -----------------------------------------------------------------------
// The dashboard cannot disagree with discovery.
// -----------------------------------------------------------------------
void test_tool_modes_match_discovery() {
    std::vector<ToolDefinition> tools = {
        // Editor-only live tool.
        makeTool("scene_get_hierarchy", true, true, {"live", "offline_fallback"}),
        // Game-only live tool.
        makeTool("runtime_step", true, false, {"live"}),
        // Offline tool.
        makeTool("project_search_text", true, true, {"offline_fallback"}),
        // Reserved.
        makeTool("nav_bake_mesh", false, false, {"unimplemented"}),
    };

    ControlRoomInputs in;
    in.connected = true;
    in.session_kind = "editor";
    const auto model = buildControlRoomModel(in, tools);

    const auto mode_of = [&](const std::string& name) {
        for (const auto& row : model["tools"]) {
            if (row.value("name", "") == name) return row.value("mode", "");
        }
        return std::string("<missing>");
    };

    // Every one of these is currentModeFor's answer, which is the same function
    // tools/list calls. If they ever diverge, this is where it shows.
    for (const auto& tool : tools) {
        ASSERT_EQ(mode_of(tool.name),
                  currentModeFor(tool.capability, tool.name, false, in.connected,
                                 in.session_kind, in.managed_unavailable));
    }
    ASSERT_EQ(mode_of("scene_get_hierarchy"), "live");
    // An editor route is an authoritative selection, so a game-only tool is
    // unavailable rather than quietly falling back.
    ASSERT_EQ(mode_of("runtime_step"), "unavailable");
    ASSERT_EQ(mode_of("project_search_text"), "offline_fallback");
    ASSERT_EQ(mode_of("nav_bake_mesh"), "unimplemented");
    ASSERT_EQ(model["surface"]["live_now"], 1);
    ASSERT_EQ(model["surface"]["implemented"], 3);
    ASSERT_EQ(model["surface"]["unimplemented"], 1);
}

// The registry is a hash map. Without an explicit order the rows arrive in
// whatever order it iterated, which is unreadable on a dashboard and makes the
// row that gets cut at the cap arbitrary as well.
void test_tool_rows_are_sorted_and_truncation_is_deterministic() {
    std::vector<ToolDefinition> tools;
    for (const char* name : {"zulu_tool", "alpha_tool", "mike_tool", "bravo_tool"}) {
        tools.push_back(makeTool(name, true, true, {"offline_fallback"}));
    }
    const auto model = buildControlRoomModel({}, tools);
    std::vector<std::string> names;
    for (const auto& row : model["tools"]) names.push_back(row.value("name", ""));
    const std::vector<std::string> expected{"alpha_tool", "bravo_tool", "mike_tool", "zulu_tool"};
    ASSERT_EQ(names, expected);

    // At the cap it is the alphabetically last rows that are dropped, every
    // time, rather than whichever ones the hash map happened to yield last.
    std::vector<ToolDefinition> many;
    for (std::size_t i = 0; i < kControlRoomMaxToolRows + 10; ++i) {
        char buffer[32];
        std::snprintf(buffer, sizeof(buffer), "tool_%04zu", i);
        many.push_back(makeTool(buffer, true, true, {"offline_fallback"}));
    }
    const auto capped = buildControlRoomModel({}, many);
    ASSERT_EQ(capped["tools"].size(), kControlRoomMaxToolRows);
    ASSERT_EQ(capped["tools"].front()["name"], "tool_0000");
    char last[32];
    std::snprintf(last, sizeof(last), "tool_%04zu", kControlRoomMaxToolRows - 1);
    ASSERT_EQ(capped["tools"].back()["name"], std::string(last));
}

void test_mutation_flag_follows_the_annotation() {
    std::vector<ToolDefinition> tools = {
        makeTool("scene_get_property", true, true, {"live"}),
        makeTool("scene_set_property", true, false, {"live"}),
    };
    const auto model = buildControlRoomModel({}, tools);
    ASSERT_FALSE(model["tools"][0]["mutates"].get<bool>());
    ASSERT_TRUE(model["tools"][1]["mutates"].get<bool>());
}

void test_legacy_names_are_counted_apart() {
    auto legacy = makeTool("get_scene_hierarchy", true, true, {"live"});
    legacy.legacy = true;
    std::vector<ToolDefinition> tools = {
        makeTool("scene_get_hierarchy", true, true, {"live"}),
        std::move(legacy),
    };
    const auto model = buildControlRoomModel({}, tools);
    ASSERT_EQ(model["surface"]["canonical"], 1);
    ASSERT_EQ(model["surface"]["legacy"], 1);
    ASSERT_EQ(model["surface"]["listed"], 2);
}

// -----------------------------------------------------------------------
// A7. The ring is safe under concurrency, and bounded.
// -----------------------------------------------------------------------
void test_log_ring_bounds_and_drop_count() {
    LogRing ring(4);
    ring.setMinimumLevel(LogLevel::Debug);
    for (int i = 0; i < 10; ++i) {
        ring.record(LogLevel::Info, "TAG", "message " + std::to_string(i));
    }
    const auto snapshot = ring.snapshot();
    ASSERT_EQ(snapshot.size(), 4u);
    ASSERT_EQ(snapshot.front().message, "message 6");
    ASSERT_EQ(snapshot.back().message, "message 9");
    ASSERT_EQ(ring.dropped(), 6u);
}

void test_log_ring_applies_a_level_floor() {
    LogRing ring(100);
    ring.setMinimumLevel(LogLevel::Warn);
    ring.record(LogLevel::Debug, "T", "debug");
    ring.record(LogLevel::Info, "T", "info");
    ring.record(LogLevel::Warn, "T", "warn");
    ring.record(LogLevel::Error, "T", "error");

    const auto snapshot = ring.snapshot();
    ASSERT_EQ(snapshot.size(), 2u);
    ASSERT_EQ(snapshot[0].level, "WARN");
    ASSERT_EQ(snapshot[1].level, "ERROR");

    // The sink runs before the logger's own level check, so without a floor of
    // its own the ring would keep records the process was configured not to log.
    LogRing silent(100);
    silent.setMinimumLevel(LogLevel::None);
    silent.record(LogLevel::Error, "T", "error");
    ASSERT_EQ(silent.snapshot().size(), 0u);
}

void test_log_ring_is_safe_under_concurrent_writers() {
    LogRing ring(256);
    ring.setMinimumLevel(LogLevel::Debug);
    std::atomic<bool> go{false};
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&ring, &go, t] {
            while (!go.load()) {}
            for (int i = 0; i < 500; ++i) {
                ring.record(LogLevel::Info, "T" + std::to_string(t),
                            "record " + std::to_string(i));
            }
        });
    }
    // A reader racing the writers, because that is the real shape: the tool
    // snapshots the ring while the stdio loop is still logging into it.
    std::atomic<bool> overflowed{false};
    std::thread reader([&ring, &go, &overflowed] {
        while (!go.load()) {}
        for (int i = 0; i < 500; ++i) {
            if (ring.snapshot().size() > 256) overflowed.store(true);
        }
    });

    go.store(true);
    for (auto& thread : threads) thread.join();
    reader.join();

    ASSERT_FALSE(overflowed.load());
    ASSERT_EQ(ring.snapshot().size(), 256u);
    ASSERT_EQ(ring.dropped(), 8u * 500u - 256u);
}

void test_zero_capacity_ring_still_works() {
    LogRing ring(0);
    ring.setMinimumLevel(LogLevel::Debug);
    ring.record(LogLevel::Error, "T", "kept");
    ASSERT_EQ(ring.snapshot().size(), 1u);
}

// -----------------------------------------------------------------------
// Shape guarantees the page depends on.
// -----------------------------------------------------------------------
void test_model_shape_is_complete_with_no_inputs() {
    const auto model = buildControlRoomModel({}, {});
    for (const char* key : {"captured_at", "server", "project", "lights", "tools", "surface",
                            "sessions", "session_note", "facts", "log", "log_note"}) {
        ASSERT_TRUE(model.contains(key));
    }
    ASSERT_TRUE(model["lights"].is_array());
    ASSERT_EQ(model["lights"].size(), 4u);
    for (const auto& entry : model["lights"]) {
        const auto state = entry.value("state", "");
        ASSERT_TRUE(state == "ok" || state == "warn" || state == "bad" || state == "unknown");
        ASSERT_TRUE(!entry.value("label", "").empty());
        ASSERT_TRUE(!entry.value("value", "").empty());
    }
    // No selected session means no key, rather than an empty string a page would
    // have to special-case.
    ASSERT_FALSE(model.contains("selected_session"));
    ASSERT_TRUE(model["captured_at"].get<std::string>().size() == 20);
}

void test_dropped_records_are_disclosed() {
    ControlRoomInputs in;
    in.log_dropped = 17;
    const auto model = buildControlRoomModel(in, {});
    ASSERT_TRUE(model["log_note"].get<std::string>().find("17") != std::string::npos);
}

struct Register {
    Register() {
        registerTest("ControlRoom.NoSessionTokenReachesTheModel",
                     test_no_session_token_reaches_the_model);
        registerTest("ControlRoom.ListedSessionsParseWithoutAToken",
                     test_listed_sessions_parse_without_a_token);
        registerTest("ControlRoom.UnknownLivenessIsOmitted",
                     test_unknown_liveness_is_omitted_not_guessed);
        registerTest("ControlRoom.MalformedSessionEntriesSkipped",
                     test_malformed_session_entries_are_skipped_not_fatal);
        registerTest("ControlRoom.PayloadIsBounded", test_the_payload_is_bounded);
        registerTest("ControlRoom.LogBudgetDefaultsLow",
                     test_log_budget_defaults_low_and_discloses_truncation);
        registerTest("ControlRoom.LongStringsAreClipped", test_long_strings_are_clipped);
        registerTest("ControlRoom.ClippingNeverSplitsUtf8",
                     test_clipping_never_splits_a_utf8_sequence);
        registerTest("ControlRoom.BridgeLightStates", test_bridge_light_states);
        registerTest("ControlRoom.SafetyProjectAndWorkLights",
                     test_safety_and_project_and_work_lights);
        registerTest("ControlRoom.ToolModesMatchDiscovery", test_tool_modes_match_discovery);
        registerTest("ControlRoom.ToolRowsSortedAndTruncationDeterministic",
                     test_tool_rows_are_sorted_and_truncation_is_deterministic);
        registerTest("ControlRoom.MutationFlagFollowsAnnotation",
                     test_mutation_flag_follows_the_annotation);
        registerTest("ControlRoom.LegacyNamesCountedApart", test_legacy_names_are_counted_apart);
        registerTest("ControlRoom.LogRingBoundsAndDropCount",
                     test_log_ring_bounds_and_drop_count);
        registerTest("ControlRoom.LogRingAppliesALevelFloor",
                     test_log_ring_applies_a_level_floor);
        registerTest("ControlRoom.LogRingSafeUnderConcurrentWriters",
                     test_log_ring_is_safe_under_concurrent_writers);
        registerTest("ControlRoom.ZeroCapacityRingStillWorks", test_zero_capacity_ring_still_works);
        registerTest("ControlRoom.ModelShapeIsComplete",
                     test_model_shape_is_complete_with_no_inputs);
        registerTest("ControlRoom.DroppedRecordsAreDisclosed", test_dropped_records_are_disclosed);
    }
} g_registerControlRoomTests;

}  // namespace
