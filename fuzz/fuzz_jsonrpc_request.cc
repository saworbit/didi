// Fuzzes the JSON-RPC request parser, which is the first thing that touches
// bytes arriving on stdin from whatever client is driving the server.
//
// The parser is a nlohmann::json parse wrapped in a try/catch, followed by
// hand-written shape checks. The catch is what makes this interesting rather
// than uninteresting: it converts every parse failure into std::nullopt, so a
// malformed document can never be told apart from a well-formed one that fails
// a shape check. Anything that escapes as a crash rather than an exception is
// what this looks for.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

#include "didi/mcp/jsonrpc.hpp"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    // Bounded so the fuzzer spends its time on parser shapes rather than on
    // one enormous document; nlohmann's own depth handling is not the target.
    if (size > 64 * 1024) {
        return 0;
    }

    const std::string raw(reinterpret_cast<const char*>(data), size);
    auto request = didi::mcp::JsonRpcRequest::parse(raw);
    if (!request) {
        return 0;
    }

    // The shape checks are the contract. A request that parses must satisfy
    // them, because everything downstream dispatches on exactly these fields:
    // a request with no method would reach the registry as a lookup of the
    // empty string.
    if (request->method.empty()) {
        __builtin_trap();
    }

    // A notification is defined by the absence of an id. If both were true at
    // once, a response would be addressed to a caller that never asked.
    if (request->is_notification && !request->id.is_null()) {
        __builtin_trap();
    }

    // params, when present, is an object or an array. Nothing downstream
    // handles a bare scalar here.
    if (!request->params.is_null() && !request->params.is_object() &&
        !request->params.is_array()) {
        __builtin_trap();
    }
    return 0;
}
