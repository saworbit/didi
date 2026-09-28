// Response economy tests (Q5 in docs/BUILD_QUEUE.md, #776).
//
// A client that declares didi/responseEconomy can decline the text copy of
// structuredContent and the session descriptor it already holds. A client that
// declares nothing must get exactly what it got before, byte for byte, so most
// of what is asserted here is what does not change. The arc these knobs exist
// for is measured against a live editor in tests/run_godot_integration.ps1.

#include "didi/common/project_path.hpp"
#include "didi/mcp/mcp_server.hpp"
#include "didi/mcp/resource_registry.hpp"
#include "didi/mcp/response_economy.hpp"
#include "didi/mcp/tool_registry.hpp"
#include "didi/runtime/session_client.hpp"

#include <filesystem>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#define ASSERT_TRUE(cond) if (!(cond)) throw std::runtime_error("Assertion failed: " #cond);
#define ASSERT_FALSE(cond) ASSERT_TRUE(!(cond))
#define ASSERT_EQ(a, b) ASSERT_TRUE((a) == (b))

void registerTest(const std::string& name, std::function<void()> fn);

namespace {

using didi::json;
using didi::mcp::CallToolResult;
using didi::mcp::ResponseEconomy;

const ResponseEconomy kNone{};
const ResponseEconomy kTextOnly{true, false};
const ResponseEconomy kSessionOnly{false, true};
const ResponseEconomy kBoth{true, true};

json declaring(const json& omit) {
    return {{"extensions", {{didi::mcp::kResponseEconomyExtension, {{"omit", omit}}}}}};
}

const json kBothDeclared = declaring(json::array({"textCopy", "sessionDescriptor"}));

json descriptor(const std::string& session_id, const std::string& kind = "editor") {
    return {{"schema_version", 1},
            {"session_id", session_id},
            {"pid", 77},
            {"kind", kind},
            {"project_path", "C:/project"},
            {"endpoint", "\\\\.\\pipe\\godot_didi_77_" + session_id},
            {"started_at_ms", 123456789},
            {"protocol_version", "1.3"}};
}

json livePayload(const json& session) {
    return {{"execution_mode", "live"}, {"session", session}, {"value", 3}};
}

// Every result shape a tool answers with: JSON, plain text, an image with its
// caption, a failure built by hand and one built from an Error.
std::vector<CallToolResult> everyResultShape() {
    std::vector<CallToolResult> shapes;
    shapes.push_back(CallToolResult::successJson(livePayload(descriptor("a"))));
    shapes.push_back(CallToolResult::successJson(json{{"items", json::array({1, 2})}}));
    shapes.push_back(CallToolResult::success("plain words"));
    shapes.push_back(CallToolResult::successImage("AA==", "caption"));
    auto image_with_data = CallToolResult::successImage("AA==", "caption");
    image_with_data.structuredContent = json{{"capture_id", "c"}};
    shapes.push_back(image_with_data);
    auto live_error = CallToolResult::successJson(
        json{{"execution_mode", "live"}, {"session", descriptor("a")},
             {"error", {{"code", 404}, {"message", "gone"}}}});
    live_error.isError = true;
    shapes.push_back(live_error);
    CallToolResult by_hand;
    by_hand.content.push_back(didi::mcp::ContentItem::makeText("{\"error\":{\"code\":400}}"));
    by_hand.isError = true;
    shapes.push_back(by_hand);
    shapes.push_back(CallToolResult::fromError(didi::Error(409, "conflict")));
    return shapes;
}

bool neverHeld(const json&) { return false; }
bool alwaysHeld(const json&) { return true; }

std::vector<std::string> textItems(const json& encoded) {
    std::vector<std::string> texts;
    for (const auto& item : encoded["content"]) {
        if (item.value("type", "") == "text") texts.push_back(item["text"].get<std::string>());
    }
    return texts;
}

}  // namespace

static void test_only_the_documented_declaration_declares_anything() {
    using didi::mcp::declaredResponseEconomy;
    for (const auto& nothing :
         {json(), json::array(), json::object(), json{{"extensions", json::array()}},
          json{{"extensions", json::object()}},
          json{{"extensions", {{"didi/responseEconomy", true}}}},
          json{{"extensions", {{"didi/responseEconomy", json::object()}}}},
          declaring("textCopy"), declaring(json::array()),
          declaring(json::array({"TextCopy", "session", 1, nullptr})),
          json{{"experimental", {{"didi/responseEconomy", {{"omit", json::array({"textCopy"})}}}}}}}) {
        ASSERT_FALSE(declaredResponseEconomy(nothing).any());
    }
    const auto text = declaredResponseEconomy(declaring(json::array({"textCopy"})));
    ASSERT_TRUE(text.omit_text_copy);
    ASSERT_FALSE(text.reference_session);
    const auto session = declaredResponseEconomy(declaring(json::array({"sessionDescriptor"})));
    ASSERT_FALSE(session.omit_text_copy);
    ASSERT_TRUE(session.reference_session);
    // A value from a later Didi is ignored, not a reason to refuse the rest.
    const auto both = declaredResponseEconomy(
        declaring(json::array({"sessionDescriptor", "somethingNewer", "textCopy"})));
    ASSERT_TRUE(both.omit_text_copy);
    ASSERT_TRUE(both.reference_session);
    ASSERT_EQ(didi::mcp::responseEconomyDeclaration()["didi/responseEconomy"]["omit"],
              json::array({"textCopy", "sessionDescriptor"}));
}

// The promise to every existing client. Not "equivalent": the same bytes.
static void test_a_client_that_declared_nothing_gets_the_same_bytes() {
    for (const auto& shape : everyResultShape()) {
        const auto before = shape.toJson().dump();
        ASSERT_EQ(didi::mcp::economizeToolResult(shape.toJson(), kNone, alwaysHeld).dump(), before);
    }
}

// A failure is the answer a caller most needs whole, and its session is
// provenance about which route failed, so nothing declared changes it.
static void test_a_failure_is_never_reshaped() {
    for (const auto& shape : everyResultShape()) {
        if (!shape.isError) continue;
        for (const auto& economy : {kTextOnly, kSessionOnly, kBoth}) {
            ASSERT_EQ(didi::mcp::economizeToolResult(shape.toJson(), economy, alwaysHeld).dump(),
                      shape.toJson().dump());
        }
    }
    // toJson() drops structuredContent from a failure today, which would hide
    // the rule above. The specification allows it there, so the rule is held
    // on its own: a failure that does carry it is still left whole.
    const auto payload = livePayload(descriptor("a"));
    const json failure = {{"content", json::array({{{"type", "text"}, {"text", payload.dump()}}})},
                          {"isError", true},
                          {"structuredContent", payload}};
    ASSERT_EQ(didi::mcp::economizeToolResult(failure, kBoth, alwaysHeld).dump(), failure.dump());
}

static void test_only_an_exact_copy_of_the_structured_payload_is_left_out() {
    using didi::mcp::economizeToolResult;
    // The ordinary case: one text item that is the payload. It goes, and the
    // structured half is untouched.
    const auto plain = CallToolResult::successJson(json{{"items", json::array({1, 2})}});
    const auto economized = economizeToolResult(plain.toJson(), kTextOnly, neverHeld);
    ASSERT_TRUE(economized["content"].is_array());
    ASSERT_TRUE(economized["content"].empty());
    ASSERT_EQ(economized["structuredContent"], plain.toJson()["structuredContent"]);
    ASSERT_EQ(economized["isError"], false);

    // A caption is said nowhere else, and neither is the image.
    auto image = CallToolResult::successImage("AA==", "caption");
    image.structuredContent = json{{"capture_id", "c"}};
    const auto kept = economizeToolResult(image.toJson(), kBoth, alwaysHeld);
    ASSERT_EQ(kept.dump(), image.toJson().dump());

    // Text that disagrees with the structured half, even slightly, is not a
    // copy of it.
    auto differs = CallToolResult::successJson(json{{"a", 1}});
    differs.content[0].text = "{\"a\": 1}";
    ASSERT_EQ(economizeToolResult(differs.toJson(), kTextOnly, neverHeld).dump(),
              differs.toJson().dump());

    // A copy beside other text: only the copy goes.
    auto mixed = CallToolResult::successJson(json{{"a", 1}});
    mixed.content.push_back(didi::mcp::ContentItem::makeText("note"));
    const auto trimmed = economizeToolResult(mixed.toJson(), kTextOnly, neverHeld);
    ASSERT_EQ(textItems(trimmed), std::vector<std::string>{"note"});

    // Text with no structured half has nothing to duplicate.
    const auto words = CallToolResult::success("plain words");
    ASSERT_EQ(economizeToolResult(words.toJson(), kBoth, alwaysHeld).dump(),
              words.toJson().dump());
}

static void test_a_held_descriptor_on_a_live_answer_becomes_a_reference() {
    using didi::mcp::economizeToolResult;
    const auto live = CallToolResult::successJson(livePayload(descriptor("a", "game")));

    const auto referenced = economizeToolResult(live.toJson(), kBoth, alwaysHeld);
    ASSERT_EQ(referenced["structuredContent"]["session"],
              json({{"session_id", "a"}, {"kind", "game"}}));
    // Everything else in the answer is left as it was.
    ASSERT_EQ(referenced["structuredContent"]["value"], 3);
    ASSERT_EQ(referenced["structuredContent"]["execution_mode"], "live");

    // Not held: sent whole.
    const auto whole = economizeToolResult(live.toJson(), kBoth, neverHeld);
    ASSERT_EQ(whole["structuredContent"]["session"], descriptor("a", "game"));

    // Declaring the text copy alone never touches the descriptor.
    const auto text_only = economizeToolResult(live.toJson(), kTextOnly, alwaysHeld);
    ASSERT_EQ(text_only["structuredContent"]["session"], descriptor("a", "game"));

    // A client that kept the copy reads the same thing in both halves.
    const auto kept_copy = economizeToolResult(live.toJson(), kSessionOnly, alwaysHeld);
    ASSERT_EQ(textItems(kept_copy).size(), static_cast<size_t>(1));
    ASSERT_EQ(json::parse(textItems(kept_copy)[0]), kept_copy["structuredContent"]);
    ASSERT_EQ(kept_copy["structuredContent"]["session"]["session_id"], "a");
    ASSERT_FALSE(kept_copy["structuredContent"]["session"].contains("endpoint"));
}

// runtime_attach_session and runtime_get_session answer with a descriptor
// because it is what was asked for. Theirs is never replaced by a reference.
static void test_a_session_tool_always_answers_with_the_whole_descriptor() {
    using didi::mcp::economizeToolResult;
    const auto attach = CallToolResult::successJson(
        json{{"execution_mode", "local_session_management"}, {"session", descriptor("a")}});
    const auto answered = economizeToolResult(attach.toJson(), kBoth, alwaysHeld);
    ASSERT_EQ(answered["structuredContent"]["session"], descriptor("a"));
    // A null session, or one that is not a descriptor, is left alone too.
    for (const auto& odd : {json(nullptr), json("a"), json{{"pid", 77}}}) {
        const auto payload = CallToolResult::successJson(
            json{{"execution_mode", "live"}, {"session", odd}});
        ASSERT_EQ(economizeToolResult(payload.toJson(), kSessionOnly, alwaysHeld).dump(),
                  payload.toJson().dump());
    }
}

// The legacy conversation's memory: whole the first time, a reference while
// nothing changes, whole again the moment anything does.
static void test_the_ledger_sends_each_descriptor_once_and_again_on_any_change() {
    using didi::mcp::economizeToolResult;
    didi::mcp::SessionDescriptorLedger ledger;
    const didi::mcp::DescriptorHeld holds = [&](const json& d) { return ledger.alreadySent(d); };
    auto session_of = [&](const json& session) {
        const auto live = CallToolResult::successJson(livePayload(session));
        return economizeToolResult(live.toJson(), kBoth, holds)["structuredContent"]["session"];
    };

    ASSERT_TRUE(session_of(descriptor("a")).contains("endpoint"));
    ASSERT_FALSE(session_of(descriptor("a")).contains("endpoint"));
    ASSERT_FALSE(session_of(descriptor("a")).contains("endpoint"));
    // The editor restarted: a new session.
    ASSERT_TRUE(session_of(descriptor("b")).contains("endpoint"));
    ASSERT_FALSE(session_of(descriptor("b")).contains("endpoint"));
    // Same id, anything else different, is still a change.
    auto moved = descriptor("b");
    moved["endpoint"] = "\\\\.\\pipe\\elsewhere";
    ASSERT_TRUE(session_of(moved).contains("endpoint"));
    ASSERT_FALSE(session_of(moved).contains("endpoint"));

    // A session tool's descriptor counts as sent, so the live answer after an
    // attach can refer to it at once.
    didi::mcp::SessionDescriptorLedger after_attach;
    const didi::mcp::DescriptorHeld attached = [&](const json& d) {
        return after_attach.alreadySent(d);
    };
    const auto attach = CallToolResult::successJson(
        json{{"execution_mode", "local_session_management"}, {"session", descriptor("c")}});
    (void)economizeToolResult(attach.toJson(), kBoth, attached);
    const auto first_live = economizeToolResult(
        CallToolResult::successJson(livePayload(descriptor("c"))).toJson(), kBoth, attached);
    ASSERT_FALSE(first_live["structuredContent"]["session"].contains("endpoint"));
}

namespace {

std::string thisProjectPath() {
    std::error_code error;
    const auto here =
        std::filesystem::weakly_canonical(std::filesystem::current_path(error), error);
    return didi::paths::nativePathToUtf8(
        (error ? std::filesystem::current_path() : here).lexically_normal());
}

didi::runtime::SessionDescriptor routeDescriptor(const std::string& session_id) {
    return didi::runtime::SessionDescriptor{
        1, session_id, std::string(64, 'a'), 77, "editor", thisProjectPath(),
#if defined(_WIN32)
        "\\\\.\\pipe\\godot_didi_77_" + session_id,
#else
        (std::filesystem::temp_directory_path() / ("godot_didi_77_" + session_id + ".sock")).string(),
#endif
        123456789, "1.3"};
}

// One editor that answers every live request, and can be swapped for another
// as though it had restarted.
class OneEditor final : public didi::runtime::IRuntimeSessionClient,
                        public std::enable_shared_from_this<OneEditor> {
public:
    explicit OneEditor(const std::string& session_id) : session(routeDescriptor(session_id)) {}

    bool connect(const std::string&, int) override { return true; }
    void disconnect() override {}
    bool isConnected() const override { return true; }
    didi::Result<json> sendRequest(const std::string& method, const json&, int) override {
        return json{{"status", "ok"}, {"method", method}};
    }
    didi::Result<json> listSessions(const std::optional<std::string>&) override {
        return json{{"sessions", json::array({session.toJson()})},
                    {"diagnostics", json::array()}};
    }
    didi::Result<json> attachSession(const std::string&) override { return json::object(); }
    didi::Result<json> detachSession() override { return json::object(); }
    std::optional<didi::runtime::SessionDescriptor> activeSession() const override {
        return session;
    }
    std::optional<didi::runtime::RuntimeRouteLease> acquireRouteLease() override {
        return didi::runtime::RuntimeRouteLease{
            std::static_pointer_cast<didi::ipc::IIpcClient>(shared_from_this()), session, 1};
    }
    std::optional<didi::runtime::RuntimeRouteLease> acquireRouteLeaseFor(
        const std::string& session_id) override {
        if (session_id != session.session_id) return std::nullopt;
        return acquireRouteLease();
    }

    didi::runtime::SessionDescriptor session;
};

const char* kEditorA = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
const char* kEditorB = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";

// A server with a live editor behind it, restored however the test leaves:
// the registry is a process-wide singleton and the next test must not inherit
// this editor.
struct LiveServer {
    explicit LiveServer(const std::string& session_id = kEditorA)
        : editor(std::make_shared<OneEditor>(session_id)) {
        server.setIpcClient(editor);
    }
    ~LiveServer() {
        auto& registry = didi::mcp::ToolRegistry::instance();
        registry.setRuntimeSessionClient(nullptr);
        registry.setIpcClient(nullptr);
        registry.registerAllDefaultTools();
        didi::mcp::ResourceRegistry::instance().setIpcClient(nullptr);
    }
    LiveServer(const LiveServer&) = delete;
    LiveServer& operator=(const LiveServer&) = delete;

    didi::mcp::McpServer server;
    std::shared_ptr<OneEditor> editor;
};

int g_next_id = 100;

json send(didi::mcp::McpServer& server, const std::string& method, const json& params) {
    didi::mcp::JsonRpcRequest req;
    req.id = ++g_next_id;
    req.method = method;
    req.params = params;
    const auto response = server.handleRequest(req);
    if (response.error.has_value()) {
        throw std::runtime_error(method + " answered a protocol error: " + response.toJson().dump());
    }
    return response.result;
}

json initialize(didi::mcp::McpServer& server, const json& capabilities = json::object()) {
    return send(server, "initialize",
                {{"protocolVersion", didi::mcp::kProtocolVersion},
                 {"capabilities", capabilities},
                 {"clientInfo", {{"name", "economy-test"}, {"version", "1"}}}});
}

const json kProbe = {{"node_type", "Node"}, {"parent_path", "/root"}, {"name", "Probe"}};

json liveCall(didi::mcp::McpServer& server, const json& meta = json()) {
    json params = {{"name", "scene_instantiate_node"}, {"arguments", kProbe}};
    if (!meta.is_null()) params["_meta"] = meta;
    const auto result = send(server, "tools/call", params);
    if (result.value("isError", false)) {
        throw std::runtime_error("live call failed: " + result.dump());
    }
    return result;
}

json modernMeta(const json& capabilities, const std::string& session_id) {
    return {{didi::mcp::kProtocolVersionMetaKey, didi::mcp::kModernProtocolVersion},
            {"io.modelcontextprotocol/clientCapabilities", capabilities},
            {"didi", {{didi::mcp::kRuntimeSessionMetaKey, session_id}}}};
}

bool carriesTextCopy(const json& result) {
    return !result["content"].empty() &&
           result["content"][0]["text"].get<std::string>() == result["structuredContent"].dump();
}

}  // namespace

static void test_the_server_declares_the_extension_in_both_handshakes() {
    for (const auto mode : {didi::mcp::McpServer::UiAppMode::Auto,
                            didi::mcp::McpServer::UiAppMode::Off}) {
        didi::mcp::McpServer server;
        server.setUiAppMode(mode);
        const auto discovered = send(server, "server/discover", json::object());
        const auto initialized = initialize(server);
        for (const auto& answer : {discovered, initialized}) {
            ASSERT_EQ(answer["capabilities"]["extensions"]["didi/responseEconomy"],
                      didi::mcp::responseEconomyDeclaration()["didi/responseEconomy"]);
            // Turning the UI off withdraws that extension and only that one.
            ASSERT_EQ(answer["capabilities"]["extensions"].contains("io.modelcontextprotocol/ui"),
                      mode != didi::mcp::McpServer::UiAppMode::Off);
        }
    }
}

// End to end through a real server: the undeclared client gets the text copy
// and the whole descriptor on every live answer; the client that declared both
// at the handshake gets the descriptor once and no copy.
static void test_a_handshake_declaration_applies_to_every_legacy_call() {
    {
        LiveServer live;
        initialize(live.server);
        for (int i = 0; i < 3; ++i) {
            const auto answer = liveCall(live.server);
            ASSERT_TRUE(carriesTextCopy(answer));
            ASSERT_TRUE(answer["structuredContent"]["session"].contains("endpoint"));
        }
    }
    {
        LiveServer live;
        initialize(live.server, kBothDeclared);
        const auto first = liveCall(live.server);
        ASSERT_TRUE(first["content"].empty());
        ASSERT_EQ(first["structuredContent"]["execution_mode"], "live");
        ASSERT_TRUE(first["structuredContent"]["session"].contains("endpoint"));
        for (int i = 0; i < 2; ++i) {
            const auto later = liveCall(live.server);
            ASSERT_TRUE(later["content"].empty());
            ASSERT_EQ(later["structuredContent"]["session"],
                      json({{"session_id", kEditorA}, {"kind", "editor"}}));
        }
        // The editor restarts under a new session: the next answer says so in
        // full, and the one after refers to it.
        live.editor->session = routeDescriptor(kEditorB);
        const auto restarted = liveCall(live.server);
        ASSERT_EQ(restarted["structuredContent"]["session"]["session_id"], kEditorB);
        ASSERT_TRUE(restarted["structuredContent"]["session"].contains("endpoint"));
        ASSERT_FALSE(liveCall(live.server)["structuredContent"]["session"].contains("endpoint"));
    }
}

// A legacy client can also declare on a single call, the way it can for the UI
// extension. That call is economized; the next undeclared one is not.
static void test_a_legacy_request_can_declare_for_itself() {
    LiveServer live;
    initialize(live.server);
    const json declared = {{"io.modelcontextprotocol/clientCapabilities",
                            declaring(json::array({"textCopy"}))}};
    ASSERT_TRUE(liveCall(live.server, declared)["content"].empty());
    ASSERT_TRUE(carriesTextCopy(liveCall(live.server)));
}

// Two eras on one process must not read each other's declaration or ledger.
static void test_the_eras_do_not_share_a_declaration_or_a_ledger() {
    LiveServer live;
    initialize(live.server, kBothDeclared);
    // A modern request that declared nothing gets everything, even though the
    // legacy handshake on the same process declared both.
    const auto modern_plain = liveCall(live.server, modernMeta(json::object(), kEditorA));
    ASSERT_TRUE(carriesTextCopy(modern_plain));
    ASSERT_TRUE(modern_plain["structuredContent"]["session"].contains("endpoint"));
    // A modern request that declared both named the session, so it holds it,
    // and its first live answer is already a reference.
    const auto modern_declared = liveCall(live.server, modernMeta(kBothDeclared, kEditorA));
    ASSERT_TRUE(modern_declared["content"].empty());
    ASSERT_FALSE(modern_declared["structuredContent"]["session"].contains("endpoint"));
    // Neither modern request wrote to the legacy ledger, so the legacy
    // client's first live answer still carries the descriptor whole.
    ASSERT_TRUE(liveCall(live.server)["structuredContent"]["session"].contains("endpoint"));
    ASSERT_FALSE(liveCall(live.server)["structuredContent"]["session"].contains("endpoint"));
}

static void test_only_every_and_once_are_session_descriptor_modes() {
    using didi::mcp::SessionDescriptorMode;
    ASSERT_TRUE(didi::mcp::parseSessionDescriptorMode("every") == SessionDescriptorMode::Every);
    ASSERT_TRUE(didi::mcp::parseSessionDescriptorMode("once") == SessionDescriptorMode::Once);
    for (const auto* refused : {"", "Once", "EVERY", "never", "once "}) {
        ASSERT_FALSE(didi::mcp::parseSessionDescriptorMode(refused).has_value());
    }
    for (const auto mode : {SessionDescriptorMode::Every, SessionDescriptorMode::Once}) {
        ASSERT_TRUE(didi::mcp::parseSessionDescriptorMode(
                        didi::mcp::sessionDescriptorModeName(mode)) == mode);
    }
}

// A client learns the mode before its first call, the way it learns the tool
// profile, rather than from the shape of an answer.
static void test_both_handshakes_report_the_session_descriptor_mode() {
    for (const auto mode : {didi::mcp::SessionDescriptorMode::Every,
                            didi::mcp::SessionDescriptorMode::Once}) {
        didi::mcp::McpServer server;
        server.setSessionDescriptorMode(mode);
        const auto discovered = send(server, "server/discover", json::object());
        const auto initialized = initialize(server);
        for (const auto& answer : {discovered, initialized}) {
            ASSERT_EQ(answer["_meta"]["didi"]["sessionDescriptor"],
                      didi::mcp::sessionDescriptorModeName(mode));
        }
    }
}

// The operator's switch, for a host that cannot declare anything (#1031). It
// references the descriptor for every client and never touches the text copy,
// which only the client can decide it does not read.
static void test_once_references_the_descriptor_for_a_client_that_declared_nothing() {
    LiveServer live;
    live.server.setSessionDescriptorMode(didi::mcp::SessionDescriptorMode::Once);
    initialize(live.server);
    const auto first = liveCall(live.server);
    ASSERT_TRUE(carriesTextCopy(first));
    ASSERT_TRUE(first["structuredContent"]["session"].contains("endpoint"));
    for (int i = 0; i < 2; ++i) {
        const auto later = liveCall(live.server);
        // The copy is kept, and it says what the structured half says.
        ASSERT_TRUE(carriesTextCopy(later));
        ASSERT_EQ(later["structuredContent"]["session"],
                  json({{"session_id", kEditorA}, {"kind", "editor"}}));
    }
    // A restart is a new descriptor, sent whole.
    live.editor->session = routeDescriptor(kEditorB);
    ASSERT_TRUE(liveCall(live.server)["structuredContent"]["session"].contains("endpoint"));
    // A modern request named its session, so it holds it from the first call.
    const auto modern = liveCall(live.server, modernMeta(json::object(), kEditorB));
    ASSERT_TRUE(carriesTextCopy(modern));
    ASSERT_FALSE(modern["structuredContent"]["session"].contains("endpoint"));
}

// The default is the promise API_SPECIFICATION.md makes: every live answer
// carries the whole descriptor.
static void test_every_is_the_default_and_changes_nothing() {
    LiveServer live;
    ASSERT_TRUE(live.server.sessionDescriptorMode() == didi::mcp::SessionDescriptorMode::Every);
    initialize(live.server);
    for (int i = 0; i < 3; ++i) {
        ASSERT_TRUE(liveCall(live.server)["structuredContent"]["session"].contains("endpoint"));
    }
}

namespace {

json readEditorState(didi::mcp::McpServer& server, const json& meta = json()) {
    json params = {{"uri", "godot://editor/state"}};
    if (!meta.is_null()) params["_meta"] = meta;
    const auto result = send(server, "resources/read", params);
    return json::parse(result["contents"][0]["text"].get<std::string>());
}

}  // namespace

// A live resource read states its session the way a live tool answer does, so
// it is shortened on the same terms and against the same ledger (#1033).
static void test_a_live_resource_read_references_a_held_descriptor() {
    {
        // Undeclared: every read carries the whole descriptor, as before.
        LiveServer live;
        initialize(live.server);
        for (int i = 0; i < 2; ++i) {
            const auto state = readEditorState(live.server);
            ASSERT_EQ(state["execution_mode"], "live");
            ASSERT_TRUE(state["session"].contains("endpoint"));
        }
    }
    {
        LiveServer live;
        initialize(live.server, kBothDeclared);
        ASSERT_TRUE(readEditorState(live.server)["session"].contains("endpoint"));
        ASSERT_EQ(readEditorState(live.server)["session"],
                  json({{"session_id", kEditorA}, {"kind", "editor"}}));
        // One ledger: the tool answer after two reads is already a reference.
        ASSERT_FALSE(liveCall(live.server)["structuredContent"]["session"].contains("endpoint"));
        // And a restart is sent whole again, by whichever answers first.
        live.editor->session = routeDescriptor(kEditorB);
        ASSERT_TRUE(readEditorState(live.server)["session"].contains("endpoint"));
        ASSERT_FALSE(liveCall(live.server)["structuredContent"]["session"].contains("endpoint"));
    }
    {
        // The operator's switch reaches resource reads too, and a modern read
        // that named its session holds it from the first.
        LiveServer live;
        live.server.setSessionDescriptorMode(didi::mcp::SessionDescriptorMode::Once);
        const auto modern = readEditorState(live.server, modernMeta(json::object(), kEditorA));
        ASSERT_FALSE(modern["session"].contains("endpoint"));
        ASSERT_EQ(modern["session"]["session_id"], kEditorA);
    }
}

namespace {

json callOffline(didi::mcp::McpServer& server, const std::string& tool, const json& arguments) {
    return send(server, "tools/call", {{"name", tool}, {"arguments", arguments}});
}

}  // namespace

// A large read made of sections takes `fields` (Q5). The schema publishes the
// sections, the answer keeps the selected ones and every key that is not a
// section, and names what it left out.
static void test_fields_select_the_sections_of_a_large_read() {
    didi::mcp::McpServer server;
    initialize(server);
    const auto whole = callOffline(server, "script_reflect_class", {{"class_name", "Node"}});
    ASSERT_FALSE(whole.value("isError", false));
    const auto& all = whole["structuredContent"];
    ASSERT_TRUE(all.contains("methods") && all.contains("properties") && all.contains("signals"));
    ASSERT_FALSE(all.contains("omitted_fields"));

    const auto narrowed = callOffline(server, "script_reflect_class",
                                      {{"class_name", "Node"}, {"fields", {"properties"}}});
    ASSERT_FALSE(narrowed.value("isError", false));
    const auto& some = narrowed["structuredContent"];
    ASSERT_EQ(some["properties"], all["properties"]);
    ASSERT_FALSE(some.contains("methods") || some.contains("signals") || some.contains("enums"));
    // Keys that are not sections stay, so the answer still says what it is.
    ASSERT_EQ(some["class_name"], "Node");
    ASSERT_EQ(some["execution_mode"], all["execution_mode"]);
    // Named in declared order, and only sections the whole answer carried.
    json expected = json::array();
    for (const auto* section : {"description", "methods", "signals", "enums"}) {
        if (all.contains(section)) expected.push_back(section);
    }
    ASSERT_EQ(some["omitted_fields"], expected);
    // The text copy is the narrowed payload, not the whole one.
    ASSERT_TRUE(carriesTextCopy(narrowed));
    ASSERT_TRUE(narrowed.dump().size() * 2 < whole.dump().size());

    // The control room's lights are required by its schema, so they are never
    // a section and survive any selection.
    const auto lights = callOffline(server, "didi_control_room", {{"fields", {"facts"}}});
    ASSERT_FALSE(lights.value("isError", false));
    ASSERT_TRUE(lights["structuredContent"].contains("lights"));
    ASSERT_TRUE(lights["structuredContent"].contains("facts"));
    ASSERT_FALSE(lights["structuredContent"].contains("tools"));
}

static void test_fields_are_published_and_checked_like_any_argument() {
    didi::mcp::McpServer server;
    initialize(server);
    const auto listing = send(server, "tools/list", json::object());
    int with_fields = 0;
    for (const auto& tool : listing["tools"]) {
        const auto& properties = tool["inputSchema"]["properties"];
        if (!properties.contains("fields")) continue;
        ++with_fields;
        ASSERT_EQ(properties["fields"]["type"], "array");
        ASSERT_TRUE(properties["fields"]["items"]["enum"].is_array());
        ASSERT_TRUE(tool["outputSchema"]["properties"].contains("omitted_fields"));
        // A section is never a key the output schema requires.
        for (const auto& section : properties["fields"]["items"]["enum"]) {
            for (const auto& required : tool["outputSchema"].value("required", json::array())) {
                ASSERT_TRUE(section != required);
            }
        }
    }
    ASSERT_EQ(with_fields, 2);
    // A name that is not a section, an empty selection, and a tool with no
    // sections are all refused by the schema, before any handler runs.
    for (const auto& refused :
         {json{{"class_name", "Node"}, {"fields", {"nope"}}},
          json{{"class_name", "Node"}, {"fields", json::array()}},
          json{{"class_name", "Node"}, {"fields", json::array({"methods", "methods"})}}}) {
        ASSERT_TRUE(callOffline(server, "script_reflect_class", refused).value("isError", false));
    }
    ASSERT_TRUE(callOffline(server, "project_list_autoloads", {{"fields", {"autoloads"}}})
                    .value("isError", false));
}

struct RegisterResponseEconomyTests {
    RegisterResponseEconomyTests() {
        registerTest("ResponseEconomy.OnlyTheDocumentedDeclarationDeclares",
                     test_only_the_documented_declaration_declares_anything);
        registerTest("ResponseEconomy.UndeclaredClientGetsTheSameBytes",
                     test_a_client_that_declared_nothing_gets_the_same_bytes);
        registerTest("ResponseEconomy.FailureIsNeverReshaped", test_a_failure_is_never_reshaped);
        registerTest("ResponseEconomy.OnlyAnExactCopyIsLeftOut",
                     test_only_an_exact_copy_of_the_structured_payload_is_left_out);
        registerTest("ResponseEconomy.HeldLiveDescriptorBecomesAReference",
                     test_a_held_descriptor_on_a_live_answer_becomes_a_reference);
        registerTest("ResponseEconomy.SessionToolAnswersWhole",
                     test_a_session_tool_always_answers_with_the_whole_descriptor);
        registerTest("ResponseEconomy.LedgerSendsOnceAndOnChange",
                     test_the_ledger_sends_each_descriptor_once_and_again_on_any_change);
        registerTest("ResponseEconomy.ServerDeclaresTheExtension",
                     test_the_server_declares_the_extension_in_both_handshakes);
        registerTest("ResponseEconomy.HandshakeDeclarationAppliesToLegacyCalls",
                     test_a_handshake_declaration_applies_to_every_legacy_call);
        registerTest("ResponseEconomy.LegacyRequestCanDeclareForItself",
                     test_a_legacy_request_can_declare_for_itself);
        registerTest("ResponseEconomy.ErasShareNoDeclarationOrLedger",
                     test_the_eras_do_not_share_a_declaration_or_a_ledger);
        registerTest("ResponseEconomy.OnlyEveryAndOnceAreModes",
                     test_only_every_and_once_are_session_descriptor_modes);
        registerTest("ResponseEconomy.HandshakesReportTheSessionDescriptorMode",
                     test_both_handshakes_report_the_session_descriptor_mode);
        registerTest("ResponseEconomy.OnceReferencesForAnUndeclaredClient",
                     test_once_references_the_descriptor_for_a_client_that_declared_nothing);
        registerTest("ResponseEconomy.EveryIsTheDefault",
                     test_every_is_the_default_and_changes_nothing);
        registerTest("ResponseEconomy.LiveResourceReadReferencesAHeldDescriptor",
                     test_a_live_resource_read_references_a_held_descriptor);
        registerTest("ResponseEconomy.FieldsSelectSections",
                     test_fields_select_the_sections_of_a_large_read);
        registerTest("ResponseEconomy.FieldsArePublishedAndChecked",
                     test_fields_are_published_and_checked_like_any_argument);
    }
} g_register_response_economy_tests;
