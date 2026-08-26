#include "didi/common/ipc_channel.hpp"
#include "didi/common/protocol.hpp"
#include <thread>
#include <chrono>

#define ASSERT_TRUE(cond) if (!(cond)) throw std::runtime_error("Assertion failed: " #cond);
#define ASSERT_EQ(a, b) ASSERT_TRUE((a) == (b))

void registerTest(const std::string& name, std::function<void()> fn);

static void test_ipc_framing() {
    didi::json msg = {{"test", 123}, {"str", "hello world"}};
    auto frame = didi::ipc::frameMessage(msg);
    ASSERT_TRUE(frame.size() > 4);

    size_t consumed = 0;
    auto parsed = didi::ipc::parseFramedMessage(frame.data(), frame.size(), consumed);
    ASSERT_TRUE(parsed.has_value());
    ASSERT_EQ(consumed, frame.size());
    ASSERT_EQ((*parsed)["test"].get<int>(), 123);
    ASSERT_EQ((*parsed)["str"].get<std::string>(), "hello world");
}

static void test_ipc_client_server_roundtrip() {
#if defined(_WIN32)
    std::string test_pipe = "\\\\.\\pipe\\godot_didi_ipc_unit_test";
#else
    std::string test_pipe = "/tmp/godot_didi_ipc_unit_test.sock";
#endif

    auto server = didi::ipc::createIpcServer();
    server->setHandler([](const didi::json& req) -> didi::json {
        std::string method = req.value("method", "");
        if (method == "test.echo") {
            return {{"echo", req.value("params", didi::json::object())}};
        }
        return {{"status", "unknown"}};
    });

    ASSERT_TRUE(server->start(test_pipe));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    auto client = didi::ipc::createIpcClient();
    ASSERT_TRUE(client->connect(test_pipe, 2000));
    ASSERT_TRUE(client->isConnected());

    auto res = client->sendRequest("test.echo", {{"msg", "ping"}});
    ASSERT_TRUE(res.isOk());
    ASSERT_EQ(res.value()["echo"]["msg"].get<std::string>(), "ping");

    client->disconnect();
    server->stop();
}

struct RegisterIpcTests {
    RegisterIpcTests() {
        registerTest("IPC.Framing", test_ipc_framing);
        registerTest("IPC.ClientServerRoundtrip", test_ipc_client_server_roundtrip);
    }
} g_registerIpcTests;
