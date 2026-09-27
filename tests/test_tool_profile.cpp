// Tool profile tests (Q4 in docs/BUILD_QUEUE.md).
//
// --tools chooses, at startup, which tools a session lists. `full` is the
// published surface and must not move. `core` lists fewer tools and says the
// facts that are the same on every entry once, on the listing. The byte
// budgets and the derivation of the core names are checked in Python
// (tests/test_tool_profiles.py) against the built binary and the trial data.

#include "didi/mcp/mcp_server.hpp"
#include "didi/mcp/tool_registry.hpp"

#include <functional>
#include <set>
#include <stdexcept>
#include <string>

#define ASSERT_TRUE(cond) if (!(cond)) throw std::runtime_error("Assertion failed: " #cond);
#define ASSERT_EQ(a, b) ASSERT_TRUE((a) == (b))

void registerTest(const std::string& name, std::function<void()> fn);

namespace {

const char* const kShared[] = {"confirmationsSkipped", "editorConnected", "sessionKind"};

didi::mcp::JsonRpcResponse request(didi::mcp::McpServer& server, const std::string& method,
                                   const didi::json& params) {
    didi::mcp::JsonRpcRequest req;
    req.id = 7;
    req.method = method;
    req.params = params;
    return server.handleRequest(req);
}

void initialize(didi::mcp::McpServer& server) {
    request(server, "initialize", {{"protocolVersion", didi::mcp::kProtocolVersion}});
}

std::set<std::string> listedNames(const didi::json& listing) {
    std::set<std::string> names;
    for (const auto& tool : listing["tools"]) names.insert(tool["name"].get<std::string>());
    return names;
}

}  // namespace

static void test_only_full_and_core_are_profiles() {
    ASSERT_TRUE(didi::mcp::parseToolProfile("full") == didi::mcp::ToolProfile::Full);
    ASSERT_TRUE(didi::mcp::parseToolProfile("core") == didi::mcp::ToolProfile::Core);
    for (const auto* refused : {"", "Core", "FULL", "all", "core "}) {
        ASSERT_TRUE(!didi::mcp::parseToolProfile(refused).has_value());
    }
}

// A name in the core set that is not an implemented canonical tool would be
// listed by nobody and refused as unknown, so the set is checked against the
// registry, and the manifest carries exactly the set.
static void test_core_names_are_implemented_canonical_tools() {
    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();
    for (const auto& name : didi::mcp::coreProfileTools()) {
        const auto* tool = registry.getTool(name);
        ASSERT_TRUE(tool != nullptr);
        ASSERT_TRUE(!tool->legacy);
        ASSERT_TRUE(tool->capability.implemented);
    }
    const auto manifest = registry.buildManifest();
    ASSERT_EQ(manifest.core.size(), didi::mcp::coreProfileTools().size());
    const auto document = manifest.toJson();
    ASSERT_EQ(document["counts"]["core"].get<size_t>(), manifest.core.size());
    ASSERT_EQ(document["names"]["core"].size(), manifest.core.size());
    // The published counts are the full surface whatever a session lists.
    ASSERT_EQ(document["counts"]["total"].get<size_t>(), registry.listTools().size());
}

// The default, and the published surface: every registration, the per-tool
// facts API_SPECIFICATION.md promises, and no listing-level _meta, so the shape
// a host already reads does not move. initialize says which profile it is.
static void test_full_lists_every_tool_and_keeps_its_metadata() {
    didi::mcp::McpServer server;
    initialize(server);
    const auto listed = request(server, "tools/list", didi::json::object());
    ASSERT_TRUE(!listed.error.has_value());
    ASSERT_EQ(listed.result["tools"].size(), didi::mcp::ToolRegistry::instance().listTools().size());
    ASSERT_TRUE(!listed.result.contains("_meta"));
    for (const auto& tool : listed.result["tools"]) {
        ASSERT_TRUE(tool["_meta"]["didi"].contains("confirmationsSkipped"));
        ASSERT_TRUE(tool["_meta"]["didi"].contains("editorConnected"));
    }
}

static void test_core_lists_its_tools_and_says_shared_facts_once() {
    didi::mcp::McpServer server;
    server.setToolProfile(didi::mcp::ToolProfile::Core);
    server.setConfirmationsSkipped(true);
    initialize(server);
    const auto listed = request(server, "tools/list", didi::json::object());
    server.setConfirmationsSkipped(false);  // the registry behind it is shared
    ASSERT_TRUE(!listed.error.has_value());
    const auto& expected = didi::mcp::coreProfileTools();
    ASSERT_TRUE(listedNames(listed.result) == expected);
    for (const auto& tool : listed.result["tools"]) {
        for (const auto* shared : kShared) ASSERT_TRUE(!tool["_meta"]["didi"].contains(shared));
        // What differs per tool stays with the tool.
        ASSERT_TRUE(tool["_meta"]["didi"].contains("currentMode"));
        ASSERT_TRUE(tool["_meta"]["didi"].contains("liveAvailable"));
    }
    const auto& meta = listed.result["_meta"]["didi"];
    ASSERT_EQ(meta["toolProfile"], "core");
    ASSERT_EQ(meta["confirmationsSkipped"], true);
    ASSERT_EQ(meta["editorConnected"], false);
    // No route is selected, so there is no session kind to state.
    ASSERT_TRUE(!meta.contains("sessionKind"));
}

// A tool the session was never shown is refused before dispatch, with the
// profile that left it out and the one argument that brings it back. A name
// Didi has never registered keeps the plain answer, because no profile has it.
static void test_core_refuses_a_tool_it_does_not_list_and_names_the_remedy() {
    didi::mcp::McpServer server;
    server.setToolProfile(didi::mcp::ToolProfile::Core);
    initialize(server);
    const std::string hidden = "scene_duplicate_node";
    ASSERT_TRUE(didi::mcp::ToolRegistry::instance().getTool(hidden) != nullptr);
    ASSERT_TRUE(didi::mcp::coreProfileTools().count(hidden) == 0);

    const auto refused = request(server, "tools/call",
                                 {{"name", hidden}, {"arguments", didi::json::object()}});
    ASSERT_TRUE(refused.error.has_value());
    ASSERT_EQ(refused.error->code, -32602);
    ASSERT_TRUE(refused.error->message.find("--tools full") != std::string::npos);
    ASSERT_EQ(refused.error->data["name"], hidden);
    ASSERT_EQ(refused.error->data["toolProfile"], "core");
    ASSERT_EQ(refused.error->data["restart_with"], "--tools full");

    const auto misspelled = request(server, "tools/call",
                                    {{"name", "scene_duplicate_nod"}, {"arguments", didi::json::object()}});
    ASSERT_TRUE(misspelled.error.has_value());
    ASSERT_TRUE(!misspelled.error->data.contains("restart_with"));

    // A listed tool reaches its handler: whatever it answers, it is not refused
    // by the profile.
    const auto listed = request(server, "tools/call",
                                {{"name", "scene_get_hierarchy"}, {"arguments", didi::json::object()}});
    ASSERT_TRUE(!listed.error.has_value());
}

static void test_the_handshake_says_which_profile_it_serves() {
    for (const auto profile : {didi::mcp::ToolProfile::Full, didi::mcp::ToolProfile::Core}) {
        didi::mcp::McpServer server;
        server.setToolProfile(profile);
        const auto answered = request(server, "initialize",
                                      {{"protocolVersion", didi::mcp::kProtocolVersion}});
        ASSERT_TRUE(!answered.error.has_value());
        ASSERT_EQ(answered.result["_meta"]["didi"]["toolProfile"],
                  didi::mcp::toolProfileName(profile));
    }
}

struct RegisterToolProfileTests {
    RegisterToolProfileTests() {
        registerTest("ToolProfile.OnlyFullAndCore", test_only_full_and_core_are_profiles);
        registerTest("ToolProfile.CoreNamesAreImplementedCanonicalTools",
                     test_core_names_are_implemented_canonical_tools);
        registerTest("ToolProfile.FullListsEveryToolAndKeepsItsMetadata",
                     test_full_lists_every_tool_and_keeps_its_metadata);
        registerTest("ToolProfile.CoreSaysSharedFactsOnce",
                     test_core_lists_its_tools_and_says_shared_facts_once);
        registerTest("ToolProfile.CoreRefusesAHiddenToolWithTheRemedy",
                     test_core_refuses_a_tool_it_does_not_list_and_names_the_remedy);
        registerTest("ToolProfile.HandshakeSaysWhichProfile",
                     test_the_handshake_says_which_profile_it_serves);
    }
} g_register_tool_profile_tests;
