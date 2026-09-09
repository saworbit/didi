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

    // Deliberately not asserted: that a parsed request has a non-empty method.
    // The first version of this target claimed that and the fuzzer produced
    // {"jsonrpc":"2.0","id":1,"method":""} within seconds. The parser requires
    // `method` to be present and a string, and says nothing about its length,
    // which matches JSON-RPC 2.0 -- an empty name is a name the registry does
    // not have, and it is answered with method-not-found like any other. The
    // assertion encoded what this author expected rather than what the parser
    // promises, and a fuzz target that asserts wishes reports them as bugs.

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
