#include "didi/mcp/mcp_server.hpp"
#include "didi/mcp/resource_registry.hpp"
#include "didi/offline/blackboard.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#define ASSERT_TRUE(cond) if (!(cond)) throw std::runtime_error("Assertion failed: " #cond);
#define ASSERT_EQ(a, b) ASSERT_TRUE((a) == (b))

void registerTest(const std::string& name, std::function<void()> fn);

namespace {

using namespace didi;
using namespace didi::mcp;

// Boards resolve from the working directory, so a test has to become a project
// and put the directory back even when it throws.
class ProjectFixture {
public:
    explicit ProjectFixture(const std::string& suffix) {
        m_previous = std::filesystem::current_path();
        m_root = std::filesystem::temp_directory_path() /
                 ("didi-bb-resources-" + suffix + "-" +
                  std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(m_root);
        std::filesystem::current_path(m_root);
    }

    ~ProjectFixture() {
        std::error_code ignored;
        std::filesystem::current_path(m_previous, ignored);
        std::filesystem::remove_all(m_root, ignored);
    }

    std::filesystem::path boardFile(const std::string& board = "default") const {
        return m_root / ".didi" / "blackboard" / (board + ".json");
    }

private:
    std::filesystem::path m_previous;
    std::filesystem::path m_root;
};

void write(const std::string& path, const json& value) {
    offline::BlackboardWriteRequest request;
    request.path = path;
    request.value = value;
    ASSERT_TRUE(offline::blackboardWrite(request).isOk());
}

void test_resources_read_state_and_tasks() {
    ProjectFixture fixture("read");
    ResourceRegistry::instance().registerAllDefaultResources();

    write("design.max_jumps", 2);
    offline::BlackboardTaskCreateRequest task;
    task.task_id = "T-1";
    task.title = "write CharacterBase.gd";
    ASSERT_TRUE(offline::blackboardTaskCreate(task).isOk());

    auto state = ResourceRegistry::instance().readResource("blackboard://default/state");
    ASSERT_TRUE(state.isOk());
    const auto state_payload = json::parse(state.value());
    ASSERT_EQ(state_payload["state"]["design"]["max_jumps"].get<int>(), 2);
    ASSERT_EQ(state_payload["execution_mode"].get<std::string>(), std::string("offline_fallback"));

    auto tasks = ResourceRegistry::instance().readResource("blackboard://default/tasks");
    ASSERT_TRUE(tasks.isOk());
    const auto tasks_payload = json::parse(tasks.value());
    ASSERT_EQ(tasks_payload["count"].get<size_t>(), size_t{1});
    ASSERT_EQ(tasks_payload["tasks"][0]["task_id"].get<std::string>(), std::string("T-1"));

    // A board nobody has written resolves to an empty one rather than failing:
    // "nothing there yet" is a real answer for a board created on demand.
    auto other = ResourceRegistry::instance().readResource("blackboard://scratch/state");
    ASSERT_TRUE(other.isOk());
    ASSERT_TRUE(json::parse(other.value())["state"].empty());

    // A malformed URI is refused rather than answered with an empty board,
    // because those two must not look the same.
    ASSERT_TRUE(ResourceRegistry::instance().readResource("blackboard://default").isErr());
    ASSERT_TRUE(ResourceRegistry::instance().readResource("blackboard://default/notes").isErr());
    ASSERT_TRUE(ResourceRegistry::instance().readResource("blackboard://").isErr());
}

void test_resources_subscription_lifecycle() {
    ProjectFixture fixture("lifecycle");
    McpServer server;

    ASSERT_TRUE(server.subscribedResources().empty());
    ASSERT_TRUE(server.subscribeResource("blackboard://default/state"));
    // Subscribing twice is not an error and does not double anything.
    ASSERT_TRUE(!server.subscribeResource("blackboard://default/state"));
    ASSERT_EQ(server.subscribedResources().size(), size_t{1});

    ASSERT_TRUE(server.subscribeResource("blackboard://default/tasks"));
    ASSERT_EQ(server.subscribedResources().size(), size_t{2});

    ASSERT_TRUE(server.unsubscribeResource("blackboard://default/state"));
    ASSERT_TRUE(!server.unsubscribeResource("blackboard://default/state"));
    ASSERT_TRUE(!server.unsubscribeResource("blackboard://never/subscribed"));
    ASSERT_EQ(server.subscribedResources().size(), size_t{1});

    ASSERT_TRUE(server.unsubscribeResource("blackboard://default/tasks"));
    ASSERT_TRUE(server.subscribedResources().empty());
}

// Captures stdout so a notification emitted by the watcher thread can be read
// back as the client would see it.
class StdoutCapture {
public:
    StdoutCapture() : m_original(std::cout.rdbuf()) { std::cout.rdbuf(m_buffer.rdbuf()); }
    ~StdoutCapture() { std::cout.rdbuf(m_original); }
    std::string text() const { return m_buffer.str(); }

private:
    std::stringstream m_buffer;
    std::streambuf* m_original;
};

void test_resources_notifies_on_external_change() {
    ProjectFixture fixture("notify");
    McpServer server;
    std::string captured;

    {
        StdoutCapture capture;
        server.subscribeResource("blackboard://default/state");

        // Let the watcher record what is already there. The first tick must not
        // announce a change, or every subscriber is told something happened the
        // moment it subscribed.
        std::this_thread::sleep_for(std::chrono::milliseconds(900));

        // A different process writing the board is the case this exists for, so
        // the change is made without going through the server at all.
        write("design.max_jumps", 3);

        for (int waited = 0; waited < 60; ++waited) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (capture.text().find("notifications/resources/updated") != std::string::npos) break;
        }
        captured = capture.text();
        server.unsubscribeResource("blackboard://default/state");
    }

    const auto position = captured.find("notifications/resources/updated");
    ASSERT_TRUE(position != std::string::npos);

    // Every emitted line must be parseable on its own, and the notification must
    // carry the URI and not the contents.
    bool found = false;
    std::istringstream lines(captured);
    std::string line;
    while (std::getline(lines, line)) {
        if (line.empty()) continue;
        const auto message = json::parse(line);
        if (message.value("method", "") != "notifications/resources/updated") continue;
        ASSERT_EQ(message["params"]["uri"].get<std::string>(),
                  std::string("blackboard://default/state"));
        ASSERT_TRUE(!message["params"].contains("contents"));
        ASSERT_TRUE(!message.contains("id"));
        found = true;
    }
    ASSERT_TRUE(found);
}

void test_resources_serialises_concurrent_writes() {
    ProjectFixture fixture("interleave");
    McpServer server;
    std::string captured;
    constexpr int kWriters = 4;
    constexpr int kEach = 60;

    {
        StdoutCapture capture;
        server.subscribeResource("blackboard://default/state");
        std::this_thread::sleep_for(std::chrono::milliseconds(700));

        // The watcher is emitting notifications from its own thread while these
        // write responses from theirs. Without one lock over stdout the two
        // interleave into lines neither side can parse.
        std::atomic<bool> churning{true};
        std::thread churn([&churning] {
            for (int index = 0; index < 40 && churning.load(); ++index) {
                write("churn.index", index);
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        });

        std::vector<std::thread> writers;
        for (int writer = 0; writer < kWriters; ++writer) {
            writers.emplace_back([&server, writer] {
                for (int index = 0; index < kEach; ++index) {
                    json message = {{"jsonrpc", "2.0"},
                                    {"id", writer * 1000 + index},
                                    {"result", {{"writer", writer}, {"payload", std::string(200, 'x')}}}};
                    server.writeLineForTest(message.dump());
                }
            });
        }
        for (auto& writer : writers) writer.join();
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        churning.store(false);
        churn.join();
        captured = capture.text();
        server.unsubscribeResource("blackboard://default/state");
    }

    size_t responses = 0;
    std::istringstream stream(captured);
    std::string line;
    while (std::getline(stream, line)) {
        if (line.empty()) continue;
        // The assertion that matters: every line is intact JSON on its own.
        const auto message = json::parse(line);
        ASSERT_TRUE(message.is_object());
        if (message.contains("result")) {
            ASSERT_EQ(message["result"]["payload"].get<std::string>().size(), size_t{200});
            ++responses;
        }
    }
    // Nothing was dropped either, so the lock serialises rather than discards.
    ASSERT_EQ(responses, size_t{kWriters * kEach});
}

struct Register {
    Register() {
        registerTest("BlackboardResources.ReadsStateAndTasks", test_resources_read_state_and_tasks);
        registerTest("BlackboardResources.SubscriptionLifecycle",
                     test_resources_subscription_lifecycle);
        registerTest("BlackboardResources.NotifiesOnExternalChange",
                     test_resources_notifies_on_external_change);
        registerTest("BlackboardResources.SerialisesConcurrentWrites",
                     test_resources_serialises_concurrent_writes);
    }
} g_registerBlackboardResourceTests;

} // namespace
