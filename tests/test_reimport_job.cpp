// asset_reimport as a job (Q8 in docs/BUILD_QUEUE.md, #996).
//
// A reimport that has to wait for the editor to apply a scan can take far
// longer than the fifteen seconds the bridge waits for any one command. As a
// job it is started detached, answered at once with an id, and read with
// asset.reimportStatus until the bridge has its answer. Asserted here: the
// bridge's store of detached answers and the lease that stops an unread one
// holding the reimport slot, that the read is answered without the main
// thread, and the server's job path against a scripted editor, an older bridge
// and a read that goes missing.

#include "didi/common/ipc_channel.hpp"
#include "didi/gdextension/editor_hook.hpp"
#include "didi/gdextension/runtime_request_router.hpp"
#include "didi/mcp/jobs.hpp"
#include "didi/mcp/mcp_server.hpp"
#include "didi/mcp/tool_registry.hpp"
#include "didi/runtime/session_client.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#define ASSERT_TRUE(cond) if (!(cond)) throw std::runtime_error("Assertion failed: " #cond);
#define ASSERT_FALSE(cond) ASSERT_TRUE(!(cond))
#define ASSERT_EQ(a, b) ASSERT_TRUE((a) == (b))

void registerTest(const std::string& name, std::function<void()> fn);

namespace {

using didi::json;
using didi::godot::DetachedReimports;
using didi::godot::EditorHook;
using didi::godot::EditorHookTestAccess;
using didi::mcp::JobState;
using namespace std::chrono_literals;

// --- The bridge's store ----------------------------------------------------

void test_a_detached_reimport_reads_working_then_its_answer() {
    DetachedReimports store;
    const auto start = DetachedReimports::Clock::now();
    store.begin("r1", start);
    const auto working = store.read("r1", start + 1500ms);
    ASSERT_TRUE(working.has_value());
    ASSERT_EQ((*working)["state"], "working");
    ASSERT_EQ((*working)["reimport_id"], "r1");
    ASSERT_EQ((*working)["elapsed_ms"], 1500);
    ASSERT_FALSE(working->contains("answer"));

    store.finish("r1", {{"paths", json::array({"res://a.gd"})}, {"idle", true}});
    const auto finished = store.read("r1", start + 2s);
    ASSERT_EQ((*finished)["state"], "finished");
    ASSERT_EQ((*finished)["answer"]["idle"], true);
    // Read again, it is the same answer: a read lost on the way can be repeated.
    ASSERT_EQ((*store.read("r1", start + 3s))["answer"], (*finished)["answer"]);
    // A second finish does not replace the first answer.
    store.finish("r1", {{"idle", false}});
    ASSERT_EQ((*store.read("r1", start + 4s))["answer"]["idle"], true);

    ASSERT_FALSE(store.read("no-such-reimport", start).has_value());
}

void test_the_lease_runs_from_the_last_read_and_ends_with_an_answer() {
    DetachedReimports store;
    const auto start = DetachedReimports::Clock::now();
    store.begin("r1", start);
    ASSERT_FALSE(store.unread("r1", start + 59s));
    ASSERT_TRUE(store.unread("r1", start + DetachedReimports::kReaderLease));
    // A read renews it.
    (void)store.read("r1", start + 50s);
    ASSERT_FALSE(store.unread("r1", start + 100s));
    ASSERT_TRUE(store.unread("r1", start + 110s));
    // A finished reimport is not waited for, so no lease applies to it.
    store.finish("r1", json::object());
    ASSERT_FALSE(store.unread("r1", start + 1h));
    ASSERT_FALSE(store.unread("no-such-reimport", start + 1h));
}

void test_the_store_keeps_the_newest_answers_and_never_drops_a_working_one() {
    DetachedReimports store;
    const auto start = DetachedReimports::Clock::now();
    store.begin("working", start);
    for (size_t index = 0; index < DetachedReimports::kKept + 3; ++index) {
        const auto id = "done-" + std::to_string(index);
        store.begin(id, start + std::chrono::seconds(index + 1));
        store.finish(id, {{"index", index}});
    }
    ASSERT_TRUE(store.read("working", start).has_value());
    ASSERT_FALSE(store.read("done-0", start).has_value());
    const auto newest = "done-" + std::to_string(DetachedReimports::kKept + 2);
    ASSERT_TRUE(store.read(newest, start).has_value());
}

// --- The hook ----------------------------------------------------------------

// A server that went away stops reading. Its reimport stops being waited for
// once the lease has run, before anything asks the engine, so the slot is free
// for the next reimport and a late reader is told why.
void test_an_unread_detached_reimport_frees_the_reimport_slot() {
    auto& hook = EditorHook::instance();
    const auto long_ago = std::chrono::steady_clock::now() - DetachedReimports::kReaderLease - 1s;
    EditorHookTestAccess::plantDetachedReimport(hook, "planted-unread", long_ago);
    ASSERT_TRUE(EditorHookTestAccess::hasPendingAssetReimport(hook));
    EditorHookTestAccess::processAssetReimportFrame(hook);
    ASSERT_FALSE(EditorHookTestAccess::hasPendingAssetReimport(hook));
    const auto read = EditorHookTestAccess::detachedReimports(hook).read(
        "planted-unread", std::chrono::steady_clock::now());
    ASSERT_TRUE(read.has_value());
    ASSERT_EQ((*read)["state"], "finished");
    ASSERT_EQ((*read)["answer"]["error"]["code"], 504);
    ASSERT_EQ((*read)["answer"]["error"]["data"]["code"], "reimport_unread");
    ASSERT_EQ((*read)["answer"]["error"]["data"]["outcome"], "unknown_outcome");
}

// The read never touches the main thread's queue: nothing pumps it here, and
// the answer still comes back, decorated like any other live answer.
void test_the_status_read_is_answered_without_the_main_thread() {
    auto& hook = EditorHook::instance();
    const auto depth = EditorHookTestAccess::queueDepth(hook);
    EditorHookTestAccess::detachedReimports(hook).begin("read-off-thread",
                                                        std::chrono::steady_clock::now());
    didi::runtime::SessionDescriptor session{1,           std::string(32, 'b'), std::string(64, 'c'),
                                             42,          "editor",             "C:/project",
                                             "endpoint",  123456789,            "1.3"};
    const auto answered = didi::godot::answerOffMainThread(
        "asset.reimportStatus", {{"reimport_id", "read-off-thread"}}, session);
    ASSERT_TRUE(answered.has_value());
    ASSERT_EQ((*answered)["state"], "working");
    ASSERT_EQ((*answered)["execution_mode"], "live");
    ASSERT_EQ(EditorHookTestAccess::queueDepth(hook), depth);

    const auto unknown = didi::godot::answerOffMainThread(
        "asset.reimportStatus", {{"reimport_id", "never-started"}}, session);
    ASSERT_EQ((*unknown)["error"]["code"], 404);
    ASSERT_EQ((*unknown)["error"]["data"]["code"], "reimport_not_found");
    const auto malformed = didi::godot::answerOffMainThread("asset.reimportStatus", json::object(), session);
    ASSERT_EQ((*malformed)["error"]["code"], 400);

    // Every other method still goes to the main thread.
    ASSERT_FALSE(didi::godot::answerOffMainThread("asset.reimport", json::object(), session).has_value());
}

// --- The server --------------------------------------------------------------

class ScopedReimportProject {
public:
    ScopedReimportProject()
        : original(std::filesystem::current_path()),
          root(std::filesystem::temp_directory_path() / "didi-reimport-job") {
        std::error_code error;
        std::filesystem::remove_all(root, error);
        std::filesystem::create_directories(root);
        std::filesystem::current_path(root);
        std::ofstream("project.godot") << "[application]\nconfig/name=\"Reimport\"\n";
    }
    ~ScopedReimportProject() {
        std::filesystem::current_path(original);
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }

private:
    std::filesystem::path original;
    std::filesystem::path root;
};

std::string descriptorEndpoint(uint64_t pid, const std::string& session_id) {
#if defined(_WIN32)
    return "\\\\.\\pipe\\godot_didi_" + std::to_string(pid) + "_" + session_id;
#else
    return (std::filesystem::temp_directory_path() /
            ("godot_didi_" + std::to_string(pid) + "_" + session_id + ".sock")).string();
#endif
}

// An attached editor whose reimport is answered from a script. It hands out a
// route lease the way the session client does, so the job's reads go down the
// lease and not through the registry's per-call dispatcher, as they do against
// a real editor. Called from the job's thread and the test's at once, so what
// it records is locked.
class ScriptedEditor final : public didi::ipc::IIpcClient,
                             public didi::runtime::IRuntimeRouteLeaseProvider,
                             public std::enable_shared_from_this<ScriptedEditor> {
public:
    bool connect(const std::string&, int) override { return true; }
    void disconnect() override {}
    bool isConnected() const override { return true; }
    std::optional<didi::runtime::RuntimeRouteLease> acquireRouteLease() override {
        const std::string session_id = "0123456789abcdef0123456789abcdef";
        return didi::runtime::RuntimeRouteLease{
            std::static_pointer_cast<didi::ipc::IIpcClient>(shared_from_this()),
            didi::runtime::SessionDescriptor{1, session_id, std::string(64, 'a'), 77, "editor",
                                             "C:/project", descriptorEndpoint(77, session_id),
                                             123456789, "1.3"},
            1};
    }
    bool quarantineRoute(const didi::runtime::RuntimeRouteLease&) override { return false; }
    didi::Result<json> sendRequest(const std::string& method, const json& params, int) override {
        {
            std::lock_guard<std::mutex> lock(mutex);
            calls.emplace_back(method, params);
        }
        return answer(method, params);
    }
    size_t count(const std::string& method) const {
        std::lock_guard<std::mutex> lock(mutex);
        size_t total = 0;
        for (const auto& call : calls) total += call.first == method ? 1 : 0;
        return total;
    }
    json firstParams(const std::string& method) const {
        std::lock_guard<std::mutex> lock(mutex);
        for (const auto& call : calls) {
            if (call.first == method) return call.second;
        }
        return nullptr;
    }
    std::function<didi::Result<json>(const std::string&, const json&)> answer;

private:
    mutable std::mutex mutex;
    std::vector<std::pair<std::string, json>> calls;
};

json reimported() {
    return {{"paths", json::array({"res://a.gd"})},     {"accepted_count", 1},
            {"reimported", json::array()},              {"refreshed", json::array({"res://a.gd"})},
            {"imported", json::array()},                {"announced", json::array({"res://a.gd"})},
            {"elapsed_ms", 31000},                      {"idle", true},
            {"execution_mode", "live"},                 {"is_live_engine", true},
            {"session_kind", "editor"}};
}

// The editor accepts the reimport, reads as working `working_reads` times, and
// then has its answer.
std::shared_ptr<ScriptedEditor> detachingEditor(int working_reads, json answer) {
    auto editor = std::make_shared<ScriptedEditor>();
    auto reads = std::make_shared<std::atomic<int>>(0);
    editor->answer = [reads, working_reads, answer](const std::string& method,
                                                    const json&) -> didi::Result<json> {
        if (method == "asset.reimport") {
            return json{{"status", "accepted"}, {"reimport_id", "0123456789abcdef"}};
        }
        if (method == "asset.reimportStatus") {
            if (reads->fetch_add(1) < working_reads) {
                return json{{"reimport_id", "0123456789abcdef"}, {"state", "working"}};
            }
            return json{{"reimport_id", "0123456789abcdef"}, {"state", "finished"}, {"answer", answer}};
        }
        return didi::Error(501, "not scripted: " + method);
    };
    return editor;
}

didi::mcp::JsonRpcResponse callTool(didi::mcp::McpServer& server, int id, json arguments) {
    didi::mcp::JsonRpcRequest call;
    call.id = id;
    call.method = "tools/call";
    call.params = {{"name", "asset_reimport"}, {"arguments", std::move(arguments)}};
    return server.handleRequest(call);
}

json textPayload(const didi::mcp::JsonRpcResponse& response) {
    return json::parse(response.result["content"][0]["text"].get<std::string>());
}

void startWith(didi::mcp::McpServer& server, std::shared_ptr<ScriptedEditor> editor) {
    server.initializeRegistries();
    // After the registries, which take their client from the server's session
    // client, and a scripted editor is not one.
    server.setIpcClient(std::move(editor));
    didi::mcp::JsonRpcRequest initialize;
    initialize.id = 1;
    initialize.method = "initialize";
    initialize.params = {{"protocolVersion", "2024-11-05"}, {"capabilities", json::object()},
                         {"clientInfo", {{"name", "reimport-job-test"}, {"version", "1"}}}};
    (void)server.handleRequest(initialize);
}

void test_a_reimport_job_waits_past_the_bridge_and_answers_its_repeat() {
    ScopedReimportProject project;
    auto editor = detachingEditor(3, reimported());
    didi::mcp::McpServer server;
    startWith(server, editor);
    server.setJobWaitsForTesting(100ms, 50ms);

    const json call = {{"paths", json::array({"res://a.gd"})}, {"timeout_ms", 600000},
                       {"request_id", "reimport-0001"}};
    const auto first = callTool(server, 10, call);
    ASSERT_FALSE(first.result.value("isError", false));
    const auto working = textPayload(first);
    ASSERT_EQ(working["status"], "working");
    const auto job_id = working["job"]["job_id"].get<std::string>();
    ASSERT_EQ(server.jobsForTesting().waitFor(job_id, 30s)->state, JobState::Completed);

    // The bridge was asked to detach, with a bound an older bridge still keeps.
    const auto sent = editor->firstParams("asset.reimport");
    ASSERT_EQ(sent["detach_timeout_ms"], 600000);
    ASSERT_EQ(sent["timeout_ms"], 10000);
    ASSERT_FALSE(sent.contains("request_id"));
    ASSERT_EQ(editor->count("asset.reimportStatus"), 4u);

    // The answer is the reimport's own, named as the job's.
    const auto repeat = callTool(server, 11, call);
    ASSERT_FALSE(repeat.result.value("isError", false));
    const auto answer = textPayload(repeat);
    ASSERT_EQ(answer["idle"], true);
    ASSERT_EQ(answer["reimport_id"], "0123456789abcdef");
    ASSERT_EQ(repeat.result["_meta"]["didi"]["job"]["job_id"], job_id);
    // Read again, not run again.
    ASSERT_EQ(editor->count("asset.reimport"), 1u);

    // Without a request_id a long wait is refused, and names the fix.
    const auto unbounded = callTool(server, 12, {{"paths", json::array({"res://a.gd"})}, {"timeout_ms", 60000}});
    ASSERT_TRUE(unbounded.result.value("isError", false));
    ASSERT_EQ(textPayload(unbounded)["error"]["data"]["code"], "reimport_needs_job");
    ASSERT_EQ(textPayload(unbounded)["error"]["data"]["field"], "request_id");
    ASSERT_EQ(editor->count("asset.reimport"), 1u);
}

void test_without_a_request_id_a_reimport_is_sent_as_before() {
    ScopedReimportProject project;
    auto editor = std::make_shared<ScriptedEditor>();
    editor->answer = [](const std::string& method, const json&) -> didi::Result<json> {
        if (method == "asset.reimport") return reimported();
        return didi::Error(501, "not scripted: " + method);
    };
    didi::mcp::McpServer server;
    startWith(server, editor);
    const auto answered = callTool(server, 20, {{"paths", json::array({"res://a.gd"})}, {"timeout_ms", 10000}});
    ASSERT_FALSE(answered.result.value("isError", false));
    const auto payload = textPayload(answered);
    ASSERT_EQ(payload["idle"], true);
    ASSERT_FALSE(payload.contains("reimport_id"));
    ASSERT_FALSE(editor->firstParams("asset.reimport").contains("detach_timeout_ms"));
    ASSERT_EQ(editor->count("asset.reimportStatus"), 0u);
}

// A bridge from before detaching ignores detach_timeout_ms and answers in full
// within timeout_ms. That answer is the job's.
void test_an_older_bridge_answers_the_job_in_full() {
    ScopedReimportProject project;
    auto editor = std::make_shared<ScriptedEditor>();
    editor->answer = [](const std::string& method, const json&) -> didi::Result<json> {
        if (method == "asset.reimport") return reimported();
        return didi::Error(501, "not scripted: " + method);
    };
    didi::mcp::McpServer server;
    startWith(server, editor);
    server.setJobWaitsForTesting(10s, 50ms);
    const auto answered = callTool(server, 30, {{"paths", json::array({"res://a.gd"})},
                                                {"request_id", "reimport-0002"}});
    ASSERT_FALSE(answered.result.value("isError", false));
    ASSERT_EQ(textPayload(answered)["idle"], true);
    ASSERT_EQ(answered.result["_meta"]["didi"]["job"]["state"], "completed");
    // A job that names no timeout waits the job's default, not ten seconds.
    ASSERT_EQ(editor->firstParams("asset.reimport")["detach_timeout_ms"], 300000);
    ASSERT_EQ(editor->count("asset.reimportStatus"), 0u);
}

// A read lost on the way is asked again; a refusal from the editor ends the
// job with it, naming the reimport.
void test_a_lost_read_is_asked_again_and_a_refused_one_is_the_answer() {
    ScopedReimportProject project;
    auto editor = std::make_shared<ScriptedEditor>();
    auto reads = std::make_shared<std::atomic<int>>(0);
    editor->answer = [reads](const std::string& method, const json&) -> didi::Result<json> {
        if (method == "asset.reimport") return json{{"status", "accepted"}, {"reimport_id", "fedcba9876543210"}};
        if (method != "asset.reimportStatus") return didi::Error(501, "not scripted: " + method);
        const int read = reads->fetch_add(1);
        // Lost twice in a row: once on the read, once on the repeat a route
        // makes after a transport failure. The next read is asked anyway.
        if (read < 2) {
            return didi::ipc::transportFailure("Failed or timed out reading response length",
                                               {true, true, true, "deadline", 17000});
        }
        if (read == 2) return json{{"reimport_id", "fedcba9876543210"}, {"state", "working"}};
        return didi::Error(404, "This editor keeps no reimport fedcba9876543210.",
                           {{"code", "reimport_not_found"}, {"outcome", "unknown_outcome"},
                            {"retryable", false}});
    };
    didi::mcp::McpServer server;
    startWith(server, editor);
    server.setJobWaitsForTesting(10s, 50ms);
    const auto answered = callTool(server, 40, {{"paths", json::array({"res://a.gd"})},
                                                {"request_id", "reimport-0003"}});
    ASSERT_TRUE(answered.result.value("isError", false));
    const auto error = textPayload(answered)["error"];
    ASSERT_EQ(error["data"]["code"], "reimport_not_found");
    ASSERT_EQ(error["data"]["reimport_id"], "fedcba9876543210");
    ASSERT_EQ(error["data"]["field"], "request_id");
    ASSERT_EQ(editor->count("asset.reimportStatus"), 4u);
    ASSERT_EQ(editor->count("asset.reimport"), 1u);
}

// The reimport's own refusal, such as an import the engine refused, comes back
// as the refusal it is, with what the bridge said under it.
void test_a_refused_reimport_is_the_jobs_refusal() {
    ScopedReimportProject project;
    auto editor = detachingEditor(
        0, {{"error", {{"code", 422},
                       {"message", "Godot could not import [\"res://bad.png\"]."},
                       {"data", {{"code", "asset_import_failed"},
                                 {"failed", json::array({"res://bad.png"})},
                                 {"outcome", "not_imported"},
                                 {"retryable", false}}}}}});
    didi::mcp::McpServer server;
    startWith(server, editor);
    server.setJobWaitsForTesting(10s, 50ms);
    const auto answered = callTool(server, 50, {{"paths", json::array({"res://bad.png"})},
                                                {"request_id", "reimport-0004"}});
    ASSERT_TRUE(answered.result.value("isError", false));
    const auto error = textPayload(answered)["error"];
    ASSERT_EQ(error["code"], 422);
    ASSERT_EQ(error["data"]["code"], "asset_import_failed");
    ASSERT_EQ(error["data"]["failed"][0], "res://bad.png");
    ASSERT_EQ(error["data"]["reimport_id"], "0123456789abcdef");
}

struct RegisterReimportJobTests {
    RegisterReimportJobTests() {
        registerTest("ReimportJob.DetachedReadsWorkingThenItsAnswer",
                     test_a_detached_reimport_reads_working_then_its_answer);
        registerTest("ReimportJob.LeaseRunsFromTheLastRead",
                     test_the_lease_runs_from_the_last_read_and_ends_with_an_answer);
        registerTest("ReimportJob.StoreKeepsNewestAnswersAndEveryWorkingOne",
                     test_the_store_keeps_the_newest_answers_and_never_drops_a_working_one);
        registerTest("ReimportJob.UnreadDetachedReimportFreesTheSlot",
                     test_an_unread_detached_reimport_frees_the_reimport_slot);
        registerTest("ReimportJob.StatusReadIsAnsweredWithoutTheMainThread",
                     test_the_status_read_is_answered_without_the_main_thread);
        registerTest("ReimportJob.WaitsPastTheBridgeAndAnswersItsRepeat",
                     test_a_reimport_job_waits_past_the_bridge_and_answers_its_repeat);
        registerTest("ReimportJob.WithoutARequestIdSentAsBefore",
                     test_without_a_request_id_a_reimport_is_sent_as_before);
        registerTest("ReimportJob.OlderBridgeAnswersInFull", test_an_older_bridge_answers_the_job_in_full);
        registerTest("ReimportJob.LostReadAskedAgainRefusedReadIsTheAnswer",
                     test_a_lost_read_is_asked_again_and_a_refused_one_is_the_answer);
        registerTest("ReimportJob.RefusedReimportIsTheJobsRefusal",
                     test_a_refused_reimport_is_the_jobs_refusal);
    }
} g_register_reimport_job_tests;

}  // namespace
