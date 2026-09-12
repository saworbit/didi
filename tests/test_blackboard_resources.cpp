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

// Break caught: only blackboard://default/* is a registered resource, so the
// mime type came from the registry and fell through to text/plain for every
// other board. The same JSON document was labelled two ways, and a client
// branching on mime parsed one board and rendered the next as a wall of text
// (#513).
void test_every_board_is_served_as_json() {
    ProjectFixture fixture("mime");
    ResourceRegistry::instance().registerAllDefaultResources();

    for (const char* uri : {"blackboard://default/state", "blackboard://default/tasks",
                            "blackboard://other/state", "blackboard://other/tasks"}) {
        ASSERT_TRUE(ResourceRegistry::instance().readResource(uri).isOk());
        ASSERT_EQ(ResourceRegistry::instance().mimeTypeFor(uri), std::string("application/json"));
    }
    // text/plain stays the answer for a scheme this server does not serve,
    // which is what that fallback is for.
    ASSERT_EQ(ResourceRegistry::instance().mimeTypeFor("nonsense://whatever"),
              std::string("text/plain"));
    // And for a blackboard URI it would refuse: labelling a refusal
    // application/json would promise a document there is none of.
    ASSERT_EQ(ResourceRegistry::instance().mimeTypeFor("blackboard://default/notes"),
              std::string("text/plain"));
}

// Break caught: a board nobody had ever written answered exactly like a board
// that exists and is empty, so an agent checking whether a coordination board
// is there before joining it could not tell "empty, go ahead" from "you have
// the name wrong and are about to start a second, private board nobody is
// reading" (#514).
void test_a_board_says_whether_it_exists() {
    ProjectFixture fixture("exists");
    ResourceRegistry::instance().registerAllDefaultResources();

    auto never = ResourceRegistry::instance().readResource("blackboard://never-written/state");
    ASSERT_TRUE(never.isOk());
    const auto never_payload = json::parse(never.value());
    ASSERT_EQ(never_payload["exists"].get<bool>(), false);
    ASSERT_TRUE(never_payload["state"].empty());

    // A board that exists and is empty is the other half of the distinction,
    // and it is the half that was indistinguishable. Written and then cleared,
    // so the file is real and the state is not.
    offline::BlackboardWriteRequest request;
    request.board = "realboard";
    request.path = "seed";
    request.value = 1;
    ASSERT_TRUE(offline::blackboardWrite(request).isOk());

    auto written = ResourceRegistry::instance().readResource("blackboard://realboard/state");
    ASSERT_TRUE(written.isOk());
    ASSERT_EQ(json::parse(written.value())["exists"].get<bool>(), true);

    // Tasks carry it too, and reading a board never creates one.
    auto tasks = ResourceRegistry::instance().readResource("blackboard://still-never/tasks");
    ASSERT_TRUE(tasks.isOk());
    ASSERT_EQ(json::parse(tasks.value())["exists"].get<bool>(), false);
    auto again = ResourceRegistry::instance().readResource("blackboard://still-never/state");
    ASSERT_TRUE(again.isOk());
    ASSERT_EQ(json::parse(again.value())["exists"].get<bool>(), false);
}

// Break caught: three differently malformed URIs came back with the same
// message, and it named the one segment that was fine in two of the three. The
// refusals were all correct; only the diagnosis was wrong (#515).
void test_a_bad_board_uri_names_the_part_that_is_wrong() {
    ProjectFixture fixture("uri-errors");
    ResourceRegistry::instance().registerAllDefaultResources();

    const auto messageFor = [](const char* uri) {
        auto result = ResourceRegistry::instance().readResource(uri);
        ASSERT_TRUE(result.isErr());
        return result.error().message;
    };

    // The kind really is the problem here, and still says so.
    ASSERT_TRUE(messageFor("blackboard://default/nope").find("kind") != std::string::npos);

    // A query string on a correct kind. Used to be blamed on the kind.
    const auto query = messageFor("blackboard://default/state?x=1");
    ASSERT_TRUE(query.find("query string") != std::string::npos);
    ASSERT_TRUE(query.find("?x=1") != std::string::npos);
    ASSERT_TRUE(query.find("kind") == std::string::npos);

    // A fragment is the same mistake and says which one it is.
    ASSERT_TRUE(messageFor("blackboard://default/state#f").find("fragment") != std::string::npos);

    // A traversal in the board name. Used to be blamed on the kind, so a caller
    // could not tell that the board name was the problem, or that it was
    // refused on purpose.
    const auto traversal = messageFor("blackboard://../../etc/state");
    ASSERT_TRUE(traversal.find("board name") != std::string::npos);
    ASSERT_TRUE(traversal.find("kind") == std::string::npos);

    // No kind at all is its own shape and keeps the shape message.
    ASSERT_TRUE(messageFor("blackboard://onlyboard").find("must be") != std::string::npos);
}

// Break caught: resources/templates/list was -32601, and resources/list can
// only ever publish the two URIs on `default`, because boards are created on
// demand. A board other than default was readable only by a client that already
// knew its name, with no way to learn one (#514).
void test_the_parameterised_board_shape_is_discoverable() {
    ProjectFixture fixture("templates");
    ResourceRegistry::instance().registerAllDefaultResources();

    const auto templates = ResourceRegistry::instance().listResourceTemplates();
    ASSERT_EQ(templates.size(), size_t{2});
    std::vector<std::string> uris;
    for (const auto& entry : templates) {
        uris.push_back(entry.uriTemplate);
        // A template nobody can read the purpose of is not discovery.
        ASSERT_TRUE(!entry.name.empty());
        ASSERT_TRUE(!entry.description.empty());
        ASSERT_EQ(entry.mimeType, std::string("application/json"));
    }
    ASSERT_EQ(uris[0], std::string("blackboard://{board}/state"));
    ASSERT_EQ(uris[1], std::string("blackboard://{board}/tasks"));

    // Substituting the parameter yields a URI this server actually serves,
    // which is the only thing that makes the template worth publishing.
    for (const auto& entry : templates) {
        std::string concrete = entry.uriTemplate;
        const auto open = concrete.find("{board}");
        concrete.replace(open, std::string("{board}").size(), "substituted");
        ASSERT_TRUE(ResourceRegistry::instance().readResource(concrete).isOk());
        ASSERT_EQ(ResourceRegistry::instance().mimeTypeFor(concrete),
                  std::string("application/json"));
    }
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
        registerTest("BlackboardResources.EveryBoardIsJson", test_every_board_is_served_as_json);
        registerTest("BlackboardResources.BoardSaysWhetherItExists",
                     test_a_board_says_whether_it_exists);
        registerTest("BlackboardResources.UriErrorNamesTheWrongPart",
                     test_a_bad_board_uri_names_the_part_that_is_wrong);
        registerTest("BlackboardResources.ParameterisedShapeIsDiscoverable",
                     test_the_parameterised_board_shape_is_discoverable);
        registerTest("BlackboardResources.SubscriptionLifecycle",
                     test_resources_subscription_lifecycle);
        registerTest("BlackboardResources.NotifiesOnExternalChange",
                     test_resources_notifies_on_external_change);
        registerTest("BlackboardResources.SerialisesConcurrentWrites",
                     test_resources_serialises_concurrent_writes);
    }
} g_registerBlackboardResourceTests;

} // namespace
