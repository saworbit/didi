#include "didi/mcp/jsonrpc.hpp"

namespace didi {
namespace mcp {

std::optional<JsonRpcRequest> JsonRpcRequest::parse(const std::string& raw_json) {
    try {
        json j = json::parse(raw_json);
        return fromJson(j);
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<JsonRpcRequest> JsonRpcRequest::fromJson(const json& j) {
    if (!j.is_object()) return std::nullopt;

    JsonRpcRequest req;
    if (!j.contains("method") || !j["method"].is_string()) {
        return std::nullopt;
    }
    req.method = j["method"].get<std::string>();

    if (j.contains("id")) {
        req.id = j["id"];
        req.is_notification = false;
    } else {
        req.id = nullptr;
        req.is_notification = true;
    }

    if (j.contains("params")) {
        req.params = j["params"];
    } else {
        req.params = json::object();
    }

    return req;
}

json JsonRpcResponse::toJson() const {
    json j;
    j["jsonrpc"] = "2.0";
    j["id"] = id;

    if (error.has_value()) {
        json err_obj = {
            {"code", error->code},
            {"message", error->message}
        };
        if (!error->data.is_null()) {
            err_obj["data"] = error->data;
        }
        j["error"] = err_obj;
    } else {
        j["result"] = result;
    }

    return j;
}

std::string JsonRpcResponse::serialize() const {
    return toJson().dump();
}

JsonRpcResponse JsonRpcResponse::makeSuccess(const json& id, const json& result) {
    JsonRpcResponse res;
    res.id = id;
    res.result = result;
    res.error = std::nullopt;
    return res;
}

JsonRpcResponse JsonRpcResponse::makeError(const json& id, int code, const std::string& message, const json& data) {
    JsonRpcResponse res;
    res.id = id;
    res.result = nullptr;
    res.error = Error(code, message, data);
    return res;
}

} // namespace mcp
} // namespace didi
