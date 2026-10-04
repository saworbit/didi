// Long work as jobs (Q8 in docs/BUILD_QUEUE.md, #1137).
//
// A job runs a tool call on its own thread and keeps the answer. Asserted here:
// the store's rules (limits, cancellation, expiry, eviction), the process
// runner stopping a child when its job is cancelled, and the server's three
// ways in: request_id on a job tool, the 2026-07-28 tasks extension, and
// neither, which must behave exactly as before.

#include "didi/common/cancellation.hpp"
#include "didi/mcp/jobs.hpp"
#include "didi/mcp/mcp_server.hpp"
#include "didi/offline/process_runner.hpp"

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#define ASSERT_TRUE(cond) if (!(cond)) throw std::runtime_error("Assertion failed: " #cond);
#define ASSERT_FALSE(cond) ASSERT_TRUE(!(cond))
#define ASSERT_EQ(a, b) ASSERT_TRUE((a) == (b))

void registerTest(const std::string& name, std::function<void()> fn);

namespace {

using didi::json;
using didi::mcp::JobState;
using didi::mcp::JobStore;
using namespace std::chrono_literals;

json done(int value) { return {{"value", value}}; }

void test_a_job_runs_off_the_calling_thread_and_keeps_its_answer() {
    JobStore store;
    const auto caller = std::this_thread::get_id();
    std::atomic<bool> ran_elsewhere{false};
    auto started = store.start("legacy", "csharp_check_build", "build-0001", "fp",
                               [&](const std::atomic<bool>&) {
                                   ran_elsewhere = std::this_thread::get_id() != caller;
                                   return done(7);
                               });
    ASSERT_TRUE(started.isOk());
    ASSERT_EQ(started.value().state, JobState::Working);
    ASSERT_EQ(started.value().id.size(), 32u);
    const auto settled = store.waitFor(started.value().id, 5s);
    ASSERT_TRUE(settled.has_value());
    ASSERT_EQ(settled->state, JobState::Completed);
    ASSERT_EQ((*settled->result)["value"], 7);
    ASSERT_TRUE(ran_elsewhere.load());

    const auto by_request = store.findByRequest("legacy", "csharp_check_build", "build-0001");
    ASSERT_TRUE(by_request.has_value());
    ASSERT_EQ(by_request->id, started.value().id);
    // Another conversation, or another tool, does not see it.
    ASSERT_FALSE(store.findByRequest("modern/x", "csharp_check_build", "build-0001").has_value());
    ASSERT_FALSE(store.findByRequest("legacy", "project_export", "build-0001").has_value());
}

void test_a_job_past_the_running_limit_is_refused_not_queued() {
    JobStore::Limits limits;
    limits.max_working = 1;
    JobStore store(limits);
    std::atomic<bool> release{false};
    auto first = store.start("legacy", "project_export", "", "fp", [&](const std::atomic<bool>&) {
        while (!release.load()) std::this_thread::sleep_for(5ms);
        return done(1);
    });
    ASSERT_TRUE(first.isOk());
    auto second = store.start("legacy", "project_export", "", "fp", [](const std::atomic<bool>&) {
        return done(2);
    });
    ASSERT_TRUE(second.isErr());
    ASSERT_EQ(second.error().code, 429);
    ASSERT_EQ(second.error().data["working_jobs"], 1);
    release = true;
    ASSERT_EQ(store.waitFor(first.value().id, 5s)->state, JobState::Completed);
    // Room again once the first has an answer.
    ASSERT_TRUE(store.start("legacy", "project_export", "", "fp", [](const std::atomic<bool>&) {
        return done(3);
    }).isOk());
}

void test_cancelling_a_job_drops_its_answer_and_reaches_the_thread() {
    JobStore store;
    std::atomic<bool> saw_flag{false};
    auto started = store.start("legacy", "csharp_check_build", "", "fp", [&](const std::atomic<bool>& cancelled) {
        while (!cancelled.load()) std::this_thread::sleep_for(5ms);
        // The flag is also published for the thread, which is how the
        // process runner, deep inside a handler, hears it.
        saw_flag = didi::cancellationRequested();
        return done(1);
    });
    ASSERT_TRUE(started.isOk());
    const auto asked = store.cancel(started.value().id);
    ASSERT_TRUE(asked.has_value());
    ASSERT_TRUE(asked->cancel_requested);
    const auto settled = store.waitFor(started.value().id, 5s);
    ASSERT_EQ(settled->state, JobState::Cancelled);
    ASSERT_FALSE(settled->result.has_value());
    ASSERT_TRUE(saw_flag.load());
    // A finished job is left as it was.
    ASSERT_EQ(store.cancel(started.value().id)->state, JobState::Cancelled);
    ASSERT_FALSE(store.cancel("no-such-job").has_value());
    // Off a job's thread nothing is ever cancelled.
    ASSERT_FALSE(didi::cancellationRequested());
}

void test_work_that_throws_is_abandoned_with_its_reason() {
    JobStore store;
    auto started = store.start("legacy", "project_export", "", "fp", [](const std::atomic<bool>&) -> json {
        throw std::runtime_error("disk full");
    });
    const auto settled = store.waitFor(started.value().id, 5s);
    ASSERT_EQ(settled->state, JobState::Abandoned);
    ASSERT_TRUE(settled->abandoned_reason.find("disk full") != std::string::npos);
}

void test_a_finished_job_expires_and_the_oldest_is_evicted() {
    std::atomic<int64_t> now{1'000'000};
    JobStore::Limits limits;
    limits.ttl_ms = 1000;
    limits.max_retained = 2;
    JobStore store(limits, [&] { return now.load(); });
    auto first = store.start("legacy", "project_export", "a-request-1", "fp", [](const std::atomic<bool>&) {
        return done(1);
    });
    ASSERT_EQ(store.waitFor(first.value().id, 5s)->state, JobState::Completed);
    now += 500;
    auto second = store.start("legacy", "project_export", "", "fp", [](const std::atomic<bool>&) {
        return done(2);
    });
    ASSERT_EQ(store.waitFor(second.value().id, 5s)->state, JobState::Completed);
    // A third start makes room by dropping the oldest finished job.
    auto third = store.start("legacy", "project_export", "", "fp", [](const std::atomic<bool>&) {
        return done(3);
    });
    ASSERT_TRUE(third.isOk());
    ASSERT_FALSE(store.find(first.value().id).has_value());
    ASSERT_TRUE(store.find(second.value().id).has_value());
    // Past its ttl a finished job is gone.
    now += 1000;
    ASSERT_FALSE(store.find(second.value().id).has_value());
}

void test_the_fingerprint_ignores_the_attempt_and_the_key_order() {
    const auto a = didi::mcp::jobFingerprint("project_export",
                                             {{"preset", "Pack"}, {"output_path", "a.pck"}, {"request_id", "x1234567"}});
    const auto b = didi::mcp::jobFingerprint("project_export",
                                             {{"output_path", "a.pck"}, {"confirmation_token", "t"}, {"preset", "Pack"}});
    ASSERT_EQ(a, b);
    ASSERT_TRUE(a != didi::mcp::jobFingerprint("project_export", {{"preset", "Other"}, {"output_path", "a.pck"}}));
    ASSERT_TRUE(a != didi::mcp::jobFingerprint("csharp_check_build", {{"preset", "Pack"}, {"output_path", "a.pck"}}));
    ASSERT_EQ(didi::mcp::isoTimestamp(1'790'000'000'123), "2026-09-21T14:13:20.123Z");
}

void test_a_cancelled_job_stops_the_process_it_started() {
    didi::offline::ProcessRequest request;
#if defined(_WIN32)
    request.executable = "ping";
    request.arguments = {"-n", "30", "127.0.0.1"};
#else
    request.executable = "sleep";
    request.arguments = {"30"};
#endif
    request.working_directory = std::filesystem::temp_directory_path();
    request.timeout = 60s;
    std::atomic<bool> cancelled{false};
    std::thread canceller([&] {
        std::this_thread::sleep_for(300ms);
        cancelled = true;
    });
    const auto started = std::chrono::steady_clock::now();
    didi::Result<didi::offline::ProcessResult> run = didi::Error::internal("not run");
    {
        const didi::CancellationScope scope(&cancelled);
        run = didi::offline::runProcess(request);
    }
    canceller.join();
    ASSERT_TRUE(run.isOk());
    ASSERT_TRUE(run.value().cancelled);
    ASSERT_FALSE(run.value().timed_out);
    ASSERT_EQ(run.value().exit_code, 130);
    ASSERT_TRUE(std::chrono::steady_clock::now() - started < 15s);
}

// --- A child's own environment ----------------------------------------------

// The value of `name` as a child launched with `environment` sees it, printed
// by the platform's shell, with an optional pause first.
std::string childSees(const std::string& name,
                      std::vector<std::pair<std::string, std::string>> environment,
                      int pause_seconds = 0) {
    didi::offline::ProcessRequest request;
#if defined(_WIN32)
    request.executable = "cmd";
    const std::string pause =
        pause_seconds > 0 ? "ping -n " + std::to_string(pause_seconds + 1) + " 127.0.0.1 >nul & " : "";
    request.arguments = {"/d", "/c", pause + "echo [%" + name + "%]"};
#else
    request.executable = "sh";
    const std::string pause = pause_seconds > 0 ? "sleep " + std::to_string(pause_seconds) + "; " : "";
    request.arguments = {"-c", pause + "printf '[%s]' \"$" + name + "\""};
#endif
    request.working_directory = std::filesystem::temp_directory_path();
    request.timeout = 30s;
    request.environment = std::move(environment);
    const auto run = didi::offline::runProcess(request);
    if (run.isErr()) return "error: " + run.error().message;
    const auto& output = run.value().output;
    const auto open = output.find('[');
    const auto close = output.rfind(']');
    if (open == std::string::npos || close == std::string::npos || close < open) return output;
    return output.substr(open + 1, close - open - 1);
}

void setParentVariable(const char* name, const char* value) {
#if defined(_WIN32)
    _putenv_s(name, value);
#else
    if (*value == '\0') unsetenv(name);
    else setenv(name, value, 1);
#endif
}

void test_a_childs_environment_is_its_own() {
    setParentVariable("DIDI_CHILD_ONLY", "");
    ASSERT_EQ(childSees("DIDI_CHILD_ONLY", {{"DIDI_CHILD_ONLY", "from-request"}}), "from-request");
    // Nothing was set on this process.
    ASSERT_TRUE(std::getenv("DIDI_CHILD_ONLY") == nullptr);

    // An override replaces the parent's value for the child alone.
    setParentVariable("DIDI_CHILD_OVERRIDE", "parent");
    ASSERT_EQ(childSees("DIDI_CHILD_OVERRIDE", {}), "parent");
    ASSERT_EQ(childSees("DIDI_CHILD_OVERRIDE", {{"DIDI_CHILD_OVERRIDE", "child"}}), "child");
    ASSERT_EQ(std::string(std::getenv("DIDI_CHILD_OVERRIDE")), "parent");
#if defined(_WIN32)
    // One name to Windows however it is spelled, so no second entry appears.
    ASSERT_EQ(childSees("DIDI_CHILD_OVERRIDE", {{"didi_child_override", "lower"}}), "lower");
    const auto entries = didi::offline::detail::childEnvironment({{"didi_child_override", "lower"}});
    size_t named = 0;
    for (const auto& entry : entries) {
        std::string upper = entry.substr(0, entry.find('=', 1));
        for (auto& c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        if (upper == "DIDI_CHILD_OVERRIDE") ++named;
    }
    ASSERT_EQ(named, 1u);
#endif
    setParentVariable("DIDI_CHILD_OVERRIDE", "");

    // A name that cannot be one is refused before anything runs.
    didi::offline::ProcessRequest bad;
    bad.executable = "git";
    bad.working_directory = std::filesystem::temp_directory_path();
    bad.environment = {{"A=B", "c"}};
    const auto refused = didi::offline::runProcess(bad);
    ASSERT_TRUE(refused.isErr());
    ASSERT_EQ(refused.error().code, 400);
}

// Every child the runner starts is a helper, so a Godot among them publishes
// no runtime session whichever site launched it, and a site that sets nothing
// of its own cannot leave the marker out (#1161).
void test_every_child_is_told_it_is_a_helper() {
    ASSERT_EQ(childSees("DIDI_OFFLINE_HELPER", {}), "1");
    ASSERT_EQ(childSees("DIDI_OFFLINE_HELPER", {{"DIDI_CHILD_ONLY", "x"}}), "1");
}

// Two launches with different settings at once, each seeing its own. A
// variable set on this process could only do this under a lock that made the
// second wait for the first, which is what an export job made every helper
// launch do for minutes.
void test_two_launches_with_different_environments_run_at_once() {
    std::string first;
    std::string second;
    const auto started = std::chrono::steady_clock::now();
    std::thread one([&] { first = childSees("DIDI_CHILD_LANE", {{"DIDI_CHILD_LANE", "one"}}, 2); });
    std::thread two([&] { second = childSees("DIDI_CHILD_LANE", {{"DIDI_CHILD_LANE", "two"}}, 2); });
    one.join();
    two.join();
    ASSERT_EQ(first, "one");
    ASSERT_EQ(second, "two");
    // Side by side: one after the other would take four seconds and more.
    ASSERT_TRUE(std::chrono::steady_clock::now() - started < 4s);
}

// --- The server ------------------------------------------------------------

class ScopedJobProject {
public:
    explicit ScopedJobProject(const std::string& name)
        : original(std::filesystem::current_path()),
          root(std::filesystem::temp_directory_path() / ("didi-jobs-" + name)) {
        std::error_code error;
        std::filesystem::remove_all(root, error);
        std::filesystem::create_directories(root);
        std::filesystem::current_path(root);
        std::ofstream("project.godot") << "[application]\nconfig/name=\"Jobs\"\n";
        std::ofstream("Game.csproj") << "<Project Sdk=\"Microsoft.NET.Sdk\"></Project>\n";
    }
    ~ScopedJobProject() {
        std::filesystem::current_path(original);
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }
    std::filesystem::path root;

private:
    std::filesystem::path original;
};

// DOTNET_BIN naming a script whose build takes about three seconds and writes
// a line to builds.txt each time it runs, so a test can count the builds.
class ScopedSlowBuild {
public:
    explicit ScopedSlowBuild(const std::filesystem::path& root) : counter(root / "builds.txt") {
#if defined(_WIN32)
        path = root / "slow_build.cmd";
        std::ofstream(path) << "@echo off\r\n"
                               "if \"%~1\"==\"--version\" (\r\n"
                               "  echo 8.0.100\r\n"
                               "  exit /b 0\r\n"
                               ")\r\n"
                               "echo built>>\"" << counter.string() << "\"\r\n"
                               "ping -n 4 127.0.0.1 >nul\r\n"
                               "echo Build succeeded.\r\n";
#else
        path = root / "slow_build.sh";
        std::ofstream(path) << "#!/bin/sh\n"
                               "if [ \"$1\" = \"--version\" ]; then echo 8.0.100; exit 0; fi\n"
                               "echo built >> \"" << counter.string() << "\"\n"
                               "sleep 3\n"
                               "echo Build succeeded.\n";
        std::filesystem::permissions(path, std::filesystem::perms::owner_all,
                                     std::filesystem::perm_options::add);
#endif
        if (const char* value = std::getenv("DOTNET_BIN")) previous = value;
        set(path.string());
    }
    ~ScopedSlowBuild() { set(previous); }

    int builds() const {
        std::ifstream file(counter);
        int lines = 0;
        for (std::string line; std::getline(file, line);) ++lines;
        return lines;
    }

    std::filesystem::path path;
    std::filesystem::path counter;

private:
    static void set(const std::string& value) {
#if defined(_WIN32)
        _putenv_s("DOTNET_BIN", value.c_str());
#else
        if (value.empty()) unsetenv("DOTNET_BIN");
        else setenv("DOTNET_BIN", value.c_str(), 1);
#endif
    }
    std::string previous;
};

json modernMeta(bool tasks) {
    json capabilities = json::object();
    if (tasks) capabilities["extensions"] = {{"io.modelcontextprotocol/tasks", json::object()}};
    return {{"io.modelcontextprotocol/protocolVersion", "2026-07-28"},
            {"io.modelcontextprotocol/clientCapabilities", capabilities}};
}

didi::mcp::JsonRpcResponse request(didi::mcp::McpServer& server, int id, const std::string& method,
                                   json params) {
    didi::mcp::JsonRpcRequest call;
    call.id = id;
    call.method = method;
    call.params = std::move(params);
    return server.handleRequest(call);
}

didi::mcp::JsonRpcResponse callTool(didi::mcp::McpServer& server, int id, const std::string& name,
                                    json arguments, json meta = json()) {
    json params = {{"name", name}, {"arguments", std::move(arguments)}};
    if (!meta.is_null()) params["_meta"] = std::move(meta);
    return request(server, id, "tools/call", std::move(params));
}

json textPayload(const didi::mcp::JsonRpcResponse& response) {
    return json::parse(response.result["content"][0]["text"].get<std::string>());
}

void startLegacy(didi::mcp::McpServer& server) {
    server.setIpcClient(nullptr);
    server.initializeRegistries();
    (void)request(server, 1, "initialize",
                  {{"protocolVersion", "2024-11-05"}, {"capabilities", json::object()},
                   {"clientInfo", {{"name", "jobs-test"}, {"version", "1"}}}});
}

void test_a_request_id_makes_a_job_that_answers_its_repeat_without_running_again() {
    ScopedJobProject project("request-id");
    ScopedSlowBuild dotnet(project.root);
    didi::mcp::McpServer server;
    startLegacy(server);
    server.setJobWaitsForTesting(200ms, 50ms);

    const json build = {{"timeout_seconds", 60}, {"request_id", "build-0001"}};
    const auto first = callTool(server, 10, "csharp_check_build", build);
    ASSERT_FALSE(first.error.has_value());
    ASSERT_FALSE(first.result.value("isError", false));
    const auto working = textPayload(first);
    ASSERT_EQ(working["status"], "working");
    ASSERT_EQ(working["job"]["request_id"], "build-0001");
    ASSERT_EQ(working["job"]["state"], "working");
    ASSERT_EQ(working["follow_up"][0]["work"], "poll");
    ASSERT_EQ(working["follow_up"][0]["tool"], "csharp_check_build");

    // The stdio loop is free while the build runs: another call answers at once.
    const auto before_list = std::chrono::steady_clock::now();
    const auto listed = request(server, 11, "tools/list", json::object());
    ASSERT_FALSE(listed.error.has_value());
    ASSERT_TRUE(std::chrono::steady_clock::now() - before_list < 2s);

    // A repeat while it runs reads the same job.
    const auto still = textPayload(callTool(server, 12, "csharp_check_build", build));
    ASSERT_EQ(still["job"]["job_id"], working["job"]["job_id"]);

    const auto settled = server.jobsForTesting().waitFor(working["job"]["job_id"].get<std::string>(), 30s);
    ASSERT_TRUE(settled.has_value());
    ASSERT_EQ(settled->state, JobState::Completed);

    // The repeat after it finished is the stored answer, named as the job's.
    const auto repeat = callTool(server, 13, "csharp_check_build", build);
    const auto answer = textPayload(repeat);
    ASSERT_FALSE(answer.contains("status") && answer["status"] == "working");
    ASSERT_EQ(answer["dotnet_version"], "8.0.100");
    ASSERT_EQ(repeat.result["_meta"]["didi"]["job"]["request_id"], "build-0001");
    ASSERT_EQ(repeat.result["_meta"]["didi"]["job"]["state"], "completed");
    // The id alone reads it too.
    const auto by_id_only = callTool(server, 14, "csharp_check_build", {{"request_id", "build-0001"}});
    ASSERT_EQ(by_id_only.result["_meta"]["didi"]["job"]["job_id"], working["job"]["job_id"]);
    ASSERT_EQ(dotnet.builds(), 1);

    // The same id for different work is refused, and runs nothing.
    const auto conflict = callTool(server, 15, "csharp_check_build",
                                   {{"timeout_seconds", 60}, {"configuration", "Release"},
                                    {"request_id", "build-0001"}});
    ASSERT_TRUE(conflict.result.value("isError", false));
    ASSERT_EQ(textPayload(conflict)["error"]["data"]["code"], "request_id_conflict");
    ASSERT_EQ(dotnet.builds(), 1);

    const auto malformed = callTool(server, 16, "csharp_check_build", {{"request_id", "short"}});
    ASSERT_TRUE(malformed.result.value("isError", false));
    ASSERT_EQ(textPayload(malformed)["error"]["data"]["field"], "request_id");
}

// A server that stops cancels and joins the jobs it is running. main can
// leave by _Exit, which runs no destructor, so the store's own one could not
// be relied on, and a job writing a file was killed half way (#1169).
void test_a_stopping_server_cancels_and_joins_its_jobs() {
    ScopedJobProject project("stop");
    ScopedSlowBuild dotnet(project.root);
    didi::mcp::McpServer server;
    startLegacy(server);
    server.setJobWaitsForTesting(200ms, 50ms);

    const auto first = callTool(server, 30, "csharp_check_build",
                                {{"timeout_seconds", 60}, {"request_id", "stop-0001"}});
    const auto working = textPayload(first);
    ASSERT_EQ(working["job"]["state"], "working");
    const auto id = working["job"]["job_id"].get<std::string>();

    const auto before = std::chrono::steady_clock::now();
    server.stop();
    // The build takes about three seconds, and the cancel kills it.
    ASSERT_TRUE(std::chrono::steady_clock::now() - before < 2500ms);
    ASSERT_EQ(server.jobsForTesting().workingCount(), 0u);
    const auto settled = server.jobsForTesting().find(id);
    ASSERT_TRUE(settled.has_value());
    ASSERT_EQ(settled->state, JobState::Cancelled);

    // Nothing new starts once it has stopped.
    const auto after = callTool(server, 31, "csharp_check_build",
                                {{"timeout_seconds", 60}, {"request_id", "stop-0002"}});
    ASSERT_TRUE(after.result.value("isError", false));
}

void test_without_a_request_id_or_the_extension_a_call_runs_as_before() {
    ScopedJobProject project("synchronous");
    ScopedSlowBuild dotnet(project.root);
    didi::mcp::McpServer server;
    startLegacy(server);
    server.setJobWaitsForTesting(200ms, 50ms);
    const auto answer = callTool(server, 20, "csharp_check_build", {{"timeout_seconds", 60}});
    const auto payload = textPayload(answer);
    ASSERT_FALSE(payload.contains("job"));
    ASSERT_EQ(payload["dotnet_version"], "8.0.100");
    ASSERT_FALSE(answer.result.contains("_meta") && answer.result["_meta"].contains("didi") &&
                 answer.result["_meta"]["didi"].contains("job"));
    ASSERT_EQ(dotnet.builds(), 1);
    ASSERT_EQ(server.jobsForTesting().workingCount(), 0u);

    // A modern request that did not declare the extension is answered the same
    // way: the extension forbids a task in answer to it.
    const auto modern = callTool(server, 21, "csharp_check_build", {{"timeout_seconds", 60}}, modernMeta(false));
    ASSERT_EQ(modern.result["resultType"], "complete");
    ASSERT_FALSE(modern.result.contains("taskId"));
    ASSERT_EQ(dotnet.builds(), 2);
}

void test_a_client_that_declared_tasks_gets_a_task_it_can_read_and_cancel() {
    ScopedJobProject project("tasks");
    ScopedSlowBuild dotnet(project.root);
    didi::mcp::McpServer server;
    server.setIpcClient(nullptr);
    server.initializeRegistries();
    server.setJobWaitsForTesting(200ms, 50ms);

    const auto created = callTool(server, 30, "csharp_check_build", {{"timeout_seconds", 60}}, modernMeta(true));
    ASSERT_FALSE(created.error.has_value());
    ASSERT_EQ(created.result["resultType"], "task");
    ASSERT_EQ(created.result["status"], "working");
    ASSERT_TRUE(created.result["pollIntervalMs"].get<int64_t>() > 0);
    ASSERT_TRUE(created.result["createdAt"].get<std::string>().back() == 'Z');
    const auto task_id = created.result["taskId"].get<std::string>();

    const auto polled = request(server, 31, "tasks/get", {{"_meta", modernMeta(true)}, {"taskId", task_id}});
    ASSERT_EQ(polled.result["resultType"], "complete");
    ASSERT_EQ(polled.result["status"], "working");

    ASSERT_EQ(server.jobsForTesting().waitFor(task_id, 30s)->state, JobState::Completed);
    const auto finished = request(server, 32, "tasks/get", {{"_meta", modernMeta(true)}, {"taskId", task_id}});
    ASSERT_EQ(finished.result["status"], "completed");
    const auto inner = json::parse(finished.result["result"]["content"][0]["text"].get<std::string>());
    ASSERT_EQ(inner["dotnet_version"], "8.0.100");

    // Cancelled while it runs: acknowledged, and the build is stopped.
    const auto second = callTool(server, 33, "csharp_check_build", {{"timeout_seconds", 60}}, modernMeta(true));
    const auto second_id = second.result["taskId"].get<std::string>();
    const auto cancelled = request(server, 34, "tasks/cancel", {{"_meta", modernMeta(true)}, {"taskId", second_id}});
    ASSERT_FALSE(cancelled.error.has_value());
    ASSERT_EQ(cancelled.result["resultType"], "complete");
    ASSERT_EQ(server.jobsForTesting().waitFor(second_id, 30s)->state, JobState::Cancelled);
    const auto after = request(server, 35, "tasks/get", {{"_meta", modernMeta(true)}, {"taskId", second_id}});
    ASSERT_EQ(after.result["status"], "cancelled");
    ASSERT_FALSE(after.result.contains("result"));

    const auto unknown = request(server, 36, "tasks/get", {{"_meta", modernMeta(true)}, {"taskId", "nope"}});
    ASSERT_TRUE(unknown.error.has_value());
    ASSERT_EQ(static_cast<int>(unknown.error->code), -32602);
}

void test_the_extension_is_declared_to_discovery_and_not_to_a_handshake() {
    didi::mcp::McpServer server;
    server.setIpcClient(nullptr);
    server.initializeRegistries();
    const auto discovered = request(server, 40, "server/discover", {{"_meta", modernMeta(false)}});
    ASSERT_TRUE(discovered.result["capabilities"]["extensions"].contains("io.modelcontextprotocol/tasks"));
    const auto handshake = request(server, 41, "initialize",
                                   {{"protocolVersion", "2024-11-05"}, {"capabilities", json::object()},
                                    {"clientInfo", {{"name", "jobs-test"}, {"version", "1"}}}});
    ASSERT_FALSE(handshake.result["capabilities"]["extensions"].contains("io.modelcontextprotocol/tasks"));
}

struct RegisterJobTests {
    RegisterJobTests() {
        registerTest("Jobs.RunsOffTheCallingThreadAndKeepsItsAnswer",
                     test_a_job_runs_off_the_calling_thread_and_keeps_its_answer);
        registerTest("Jobs.PastTheRunningLimitIsRefusedNotQueued",
                     test_a_job_past_the_running_limit_is_refused_not_queued);
        registerTest("Jobs.CancellingDropsTheAnswerAndReachesTheThread",
                     test_cancelling_a_job_drops_its_answer_and_reaches_the_thread);
        registerTest("Jobs.WorkThatThrowsIsAbandoned", test_work_that_throws_is_abandoned_with_its_reason);
        registerTest("Jobs.FinishedJobExpiresOldestIsEvicted",
                     test_a_finished_job_expires_and_the_oldest_is_evicted);
        registerTest("Jobs.FingerprintIgnoresTheAttemptAndKeyOrder",
                     test_the_fingerprint_ignores_the_attempt_and_the_key_order);
        registerTest("Jobs.CancelledJobStopsItsProcess", test_a_cancelled_job_stops_the_process_it_started);
        registerTest("Jobs.ChildEnvironmentIsItsOwn", test_a_childs_environment_is_its_own);
        registerTest("Jobs.EveryChildIsToldItIsAHelper", test_every_child_is_told_it_is_a_helper);
        registerTest("Jobs.TwoLaunchesWithDifferentEnvironmentsRunAtOnce",
                     test_two_launches_with_different_environments_run_at_once);
        registerTest("Jobs.RequestIdRepeatDoesNotRunAgain",
                     test_a_request_id_makes_a_job_that_answers_its_repeat_without_running_again);
        registerTest("Jobs.StoppingServerCancelsAndJoinsJobs",
                     test_a_stopping_server_cancels_and_joins_its_jobs);
        registerTest("Jobs.WithoutEitherTheCallRunsAsBefore",
                     test_without_a_request_id_or_the_extension_a_call_runs_as_before);
        registerTest("Jobs.TasksExtensionReadAndCancel",
                     test_a_client_that_declared_tasks_gets_a_task_it_can_read_and_cancel);
        registerTest("Jobs.ExtensionDeclaredToDiscoveryOnly",
                     test_the_extension_is_declared_to_discovery_and_not_to_a_handshake);
    }
} g_register_job_tests;

}  // namespace
