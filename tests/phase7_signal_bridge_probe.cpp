#include "didi/common/ipc_channel.hpp"
#include "didi/common/json.hpp"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using didi::json;

namespace {

class Probe {
public:
    explicit Probe(const std::string& descriptor_path) {
        std::ifstream stream(descriptor_path);
        if (!stream) throw std::runtime_error("descriptor is unreadable");
        descriptor_ = json::parse(stream);
        client_ = didi::ipc::createIpcClient();
        if (!client_ || !client_->connect(descriptor_.at("endpoint").get<std::string>(), 3000)) {
            throw std::runtime_error("unable to connect to editor IPC");
        }
        auto handshake = request("session.handshake", {{"protocol_version", "1.3"}});
        require(!handshake.contains("error"), "session handshake failed");
    }

    ~Probe() { if (client_) client_->disconnect(); }

    json request(const std::string& method, json params = json::object()) {
        params["_didi_session_token"] = descriptor_.at("token");
        auto result = client_->sendRequest(method, params, 17000);
        if (result.isErr()) {
            json error = {{"code", result.error().code}, {"message", result.error().message}};
            if (!result.error().data.is_null() && !result.error().data.empty()) {
                error["data"] = result.error().data;
            }
            return {{"error", std::move(error)}};
        }
        return result.value();
    }

    static void require(bool condition, const std::string& message) {
        if (!condition) throw std::runtime_error(message);
    }

    static void success(const json& value) { require(!value.contains("error"), value.dump()); }
    static void error(const json& value, int code, const std::string& message = {}) {
        require(value.contains("error") && value["error"].value("code", 0) == code, value.dump());
        if (!message.empty()) require(value["error"].value("message", "") == message, value.dump());
    }

    json list(const std::string& node) {
        auto value = request("signal.listConnections", {{"target_node", node}});
        success(value);
        return value;
    }

    static const json* signal(const json& listing, const std::string& name) {
        for (const auto& value : listing.at("signals")) {
            if (value.value("name", "") == name) return &value;
        }
        return nullptr;
    }

    static bool connection(const json& listing, const std::string& signal_name,
                           const std::string& node, const std::string& method, int flags) {
        const auto* item = signal(listing, signal_name);
        if (!item) return false;
        const auto canonical_node = node.starts_with("/")
            ? node : "/root/SignalBridgeFixture/" + node;
        for (const auto& candidate : item->at("connections")) {
            if (candidate.value("target_node", "") == canonical_node &&
                candidate.value("target_method", "") == method &&
                candidate.value("flags", -1) == flags) return true;
        }
        return false;
    }

private:
    json descriptor_;
    std::unique_ptr<didi::ipc::IIpcClient> client_;
};

json relation(const std::string& signal_name, const std::string& method, bool flags = true) {
    json value = {{"emitter_node", "BasicEmitter"}, {"signal_name", signal_name},
                  {"target_node", "Receiver"}, {"target_method", method}};
    if (flags) value["flags"] = 2;
    return value;
}

void run(Probe& probe) {
    Probe::error(probe.request("signal.listConnections", {{"target_node", "BasicEmitter"}, {"extra", 1}}), 400);
    Probe::error(probe.request("signal.connect", {{"emitter_node", 7}}), 400);

    auto ordering = probe.list("OrderingEmitter");
    std::vector<std::string> names;
    for (const auto& item : ordering.at("signals")) names.push_back(item.at("name").get<std::string>());
    auto utf8_less = [](const std::string& left, const std::string& right) {
        return std::lexicographical_compare(
            left.begin(), left.end(), right.begin(), right.end(),
            [](char a, char b) {
                return static_cast<unsigned char>(a) < static_cast<unsigned char>(b);
            });
    };
    Probe::require(std::is_sorted(names.begin(), names.end(), utf8_less), ordering.dump());
    const auto alpha = std::find(names.begin(), names.end(), "alpha");
    const auto zeta = std::find(names.begin(), names.end(), "zeta");
    const auto angstrom = std::find(names.begin(), names.end(), "Ångstrom");
    const auto eclair = std::find(names.begin(), names.end(), "éclair");
    Probe::require(alpha < zeta && zeta < angstrom && angstrom < eclair, ordering.dump());
    Probe::require(ordering == probe.list("OrderingEmitter"), "listing changed across repeated calls");

    auto capped = probe.list("SignalCapEmitter");
    Probe::require(capped.at("signals").size() == 256 && capped.at("truncated") == true &&
                   capped.at("truncated_at") == "signals", capped.dump());
    std::vector<std::string> capped_names;
    for (const auto& item : capped.at("signals")) {
        capped_names.push_back(item.at("name").get<std::string>());
    }
    Probe::require(std::is_sorted(capped_names.begin(), capped_names.end(), utf8_less) &&
                   std::find(capped_names.begin(), capped_names.end(), "signal_000") != capped_names.end() &&
                   std::find(capped_names.begin(), capped_names.end(), "signal_299") == capped_names.end(),
                   capped.dump());

    auto bulk = probe.list("BulkEmitter");
    const auto* bulk_signal = Probe::signal(bulk, "a_bulk");
    Probe::require(bulk_signal && bulk_signal->at("connections").size() == 256 &&
                   bulk.at("truncated_at") == "connections", bulk.dump());
    Probe::require(bulk_signal->at("connections").front().at("target_node") ==
                       "/root/SignalBridgeFixture/BulkReceivers/Receiver_000" &&
                   bulk_signal->at("connections").back().at("target_node") ==
                       "/root/SignalBridgeFixture/BulkReceivers/Receiver_255", bulk.dump());
    Probe::error(probe.request("signal.listConnections", {{"target_node", "OverflowEmitter"}}), 413);
    Probe::error(probe.request("signal.listConnections", {{"target_node", "InvalidFlagsEmitter"}}),
                 500, "extension_protocol_error");

    auto connect = probe.request("signal.connect", relation("basic", "receive_basic"));
    Probe::success(connect);
    Probe::require(connect.at("flags") == 2 && connect.at("connected") == true, connect.dump());
    Probe::require(Probe::connection(probe.list("BasicEmitter"), "basic", "Receiver", "receive_basic", 2),
                   "connect identity was not observable");
    Probe::error(probe.request("signal.connect", relation("basic", "receive_basic")), 409);
    Probe::success(probe.request("editor.undo"));
    Probe::require(!Probe::connection(probe.list("BasicEmitter"), "basic", "Receiver", "receive_basic", 2),
                   "undo did not remove exact callable");
    Probe::success(probe.request("editor.redo"));
    Probe::require(Probe::connection(probe.list("BasicEmitter"), "basic", "Receiver", "receive_basic", 2),
                   "redo did not restore exact callable");

    auto disconnect = probe.request("signal.disconnect", relation("basic", "receive_basic", false));
    Probe::success(disconnect);
    Probe::require(disconnect.at("flags") == 2 && disconnect.at("disconnected") == true, disconnect.dump());
    Probe::error(probe.request("signal.disconnect", relation("basic", "receive_basic", false)), 409);
    Probe::success(probe.request("editor.undo"));
    Probe::require(Probe::connection(probe.list("BasicEmitter"), "basic", "Receiver", "receive_basic", 2),
                   "disconnect undo did not restore exact callable");
    Probe::success(probe.request("editor.redo"));
    Probe::require(!Probe::connection(probe.list("BasicEmitter"), "basic", "Receiver", "receive_basic", 2),
                   "disconnect redo did not remove exact callable");

    Probe::error(probe.request("signal.disconnect", relation("unsupported", "receive_basic", false)), 409);
    Probe::require(Probe::connection(probe.list("BasicEmitter"), "unsupported", "Receiver", "receive_basic", 1),
                   "unsupported existing connection changed");

    const std::vector<std::pair<std::string, json>> supported = {
        {"bool_value", json::array({true})}, {"int_value", json::array({9})},
        {"float_value", json::array({7})}, {"float_negative_value", json::array({-3})},
        {"string_value", json::array({"utf8-✓"})},
        {"array_value", json::array({json::array({1, json{{"nested", true}}, nullptr})})},
        {"dictionary_value", json::array({json{{"alpha", json::array({1, 2})}, {"null", nullptr}}})},
        {"nullable_node", json::array({nullptr})},
    };
    const std::vector<std::string> markers = {"marker_bool", "marker_int", "marker_float",
        "marker_float_negative", "marker_string", "marker_array", "marker_dictionary", "marker_null"};
    for (size_t index = 0; index < supported.size(); ++index) {
        auto emitted = probe.request("signal.emit", {{"target_node", "ValueEmitter"},
            {"signal_name", supported[index].first}, {"arguments", supported[index].second}});
        Probe::success(emitted);
        Probe::require(Probe::connection(probe.list("ValueEmitter"), markers[index], "Receiver", "marker", 2),
                       "callback side effect/type was not observed for " + supported[index].first);
    }

    for (const auto& rejected : std::vector<std::pair<std::string, json>>{
             {"typed_array_value", json::array({json::array({1, 2})})},
             {"typed_dictionary_value", json::array({json{{"a", 1}}})},
             {"nullable_node", json::array({json{{"object_id", 1}}})},
             {"callable_value", json::array({json{{"target", "Receiver"}, {"method", "marker"}}})},
             {"resource_value", json::array({json{{"resource", "res://fake.tres"}}})},
             {"string_value", json::array({nullptr})}}) {
        Probe::error(probe.request("signal.emit", {{"target_node", "ValueEmitter"},
            {"signal_name", rejected.first}, {"arguments", rejected.second}}), 400);
    }

    auto configure = [&](const std::string& seam) {
        Probe::success(probe.request("phase7SignalTest.configure", {{"seam", seam}}));
    };
    configure("missing_destination_float_constructor");
    Probe::error(probe.request("signal.emit", {{"target_node", "ValueEmitter"},
        {"signal_name", "float_preflight_value"}, {"arguments", json::array({11})}}),
        501, "required_bind_unavailable");
    Probe::require(!Probe::connection(probe.list("ValueEmitter"), "marker_float_preflight",
                                      "Receiver", "marker", 2),
                   "missing destination FLOAT constructor dispatched callback");
    Probe::success(probe.request("signal.emit", {{"target_node", "ValueEmitter"},
        {"signal_name", "float_preflight_value"}, {"arguments", json::array({11})}}));
    Probe::require(Probe::connection(probe.list("ValueEmitter"), "marker_float_preflight",
                                     "Receiver", "marker", 2),
                   "destination FLOAT widening did not preserve runtime FLOAT type");
    configure("malformed_metadata");
    Probe::error(probe.request("signal.listConnections", {{"target_node", "OrderingEmitter"}}),
                 500, "extension_protocol_error");
    configure("missing_required_api");
    Probe::error(probe.request("signal.emit", {{"target_node", "ValueEmitter"},
        {"signal_name", "array_value"}, {"arguments", json::array({json::array({1})})}}), 501);
    configure("conversion_failure");
    Probe::error(probe.request("signal.emit", {{"target_node", "ValueEmitter"},
        {"signal_name", "array_value"}, {"arguments", json::array({json::array({1})})}}),
        500, "extension_protocol_error");

    configure("connect_postcondition_mismatch");
    auto connect_mismatch = probe.request("signal.connect", relation("mismatch_connect", "receive_basic"));
    Probe::error(connect_mismatch, 500, "signal_postcondition_mismatch");
    Probe::require(connect_mismatch["error"]["data"].at("rollback") == "completed" &&
                   connect_mismatch["error"]["data"].at("outcome") == "rolled_back", connect_mismatch.dump());
    Probe::require(!Probe::connection(probe.list("BasicEmitter"), "mismatch_connect", "Receiver", "receive_basic", 2),
                   "connect mismatch rollback did not restore pre-state");

    Probe::success(probe.request("signal.connect", relation("mismatch_disconnect", "receive_basic")));
    configure("disconnect_postcondition_mismatch");
    auto disconnect_mismatch = probe.request("signal.disconnect", relation("mismatch_disconnect", "receive_basic", false));
    Probe::error(disconnect_mismatch, 500, "signal_postcondition_mismatch");
    Probe::require(disconnect_mismatch["error"]["data"].at("rollback") == "completed" &&
                   disconnect_mismatch["error"]["data"].at("outcome") == "rolled_back", disconnect_mismatch.dump());
    Probe::require(Probe::connection(probe.list("BasicEmitter"), "mismatch_disconnect", "Receiver", "receive_basic", 2),
                   "disconnect mismatch rollback did not restore pre-state");

    configure("connect_postcondition_mismatch_rollback_failure");
    auto unknown = probe.request("signal.connect", relation("mismatch_connect", "receive_basic"));
    Probe::error(unknown, 500, "signal_postcondition_mismatch");
    Probe::require(unknown["error"]["data"].at("rollback") == "failed" &&
                   unknown["error"]["data"].at("outcome") == "unknown", unknown.dump());
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: phase7_signal_bridge_probe <descriptor.json>\n";
        return 2;
    }
    try {
        Probe probe(argv[1]);
        run(probe);
        std::cout << "PHASE7_SIGNAL_RAW_METHODS|list,connect,disconnect,emit|ok\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "PHASE7_SIGNAL_BRIDGE_FAILURE|" << error.what() << "\n";
        return 1;
    }
}
