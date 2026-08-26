#pragma once

#include <string>
#include <optional>
#include "didi/common/types.hpp"
#include "didi/common/json.hpp"

namespace didi {
namespace mcp {

enum JsonRpcErrorCode {
    ParseError = -32700,
    InvalidRequest = -32600,
    MethodNotFound = -32601,
    InvalidParams = -32602,
    InternalError = -32603,
    ServerErrorStart = -32000,
    ServerErrorEnd = -32099
};

struct JsonRpcRequest {
    json id; // int or string or null
    std::string method;
    json params;
    bool is_notification{false};

    static std::optional<JsonRpcRequest> parse(const std::string& raw_json);
    static std::optional<JsonRpcRequest> fromJson(const json& j);
};

struct JsonRpcResponse {
    json id;
    json result;
    std::optional<Error> error;

    json toJson() const;
    std::string serialize() const;

    static JsonRpcResponse makeSuccess(const json& id, const json& result);
    static JsonRpcResponse makeError(const json& id, int code, const std::string& message, const json& data = nullptr);
};

} // namespace mcp
} // namespace didi
