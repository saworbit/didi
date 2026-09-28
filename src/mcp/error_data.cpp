#include "didi/mcp/error_data.hpp"

namespace didi {
namespace mcp {

std::string errorCodeForStatus(int status) {
    switch (status) {
        case 400: return "invalid_arguments";
        case 401:
        case 403: return "forbidden";
        case 404: return "not_found";
        case 405: return "method_not_allowed";
        case 409: return "conflict";
        case 410: return "gone";
        case 413: return "response_too_large";
        case 422: return "unprocessable";
        case 428: return "confirmation_required";
        case 429: return "rate_limited";
        case 501: return "unimplemented";
        case 503: return "not_connected";
        case 504: return "timeout";
        default: break;
    }
    return status >= 500 ? "internal_error" : "request_failed";
}

bool retryableForStatus(int status) {
    // Retryable means the same call could succeed later without the caller
    // changing anything about the request itself. A confirmation qualifies: the
    // documented next step is to take the token the preview returns and call
    // again with the same arguments.
    return status == 428 || status == 429 || status == 503 || status == 504;
}

void applyErrorDataFloor(json& error, const std::string& tool,
                         const std::string& canonical_tool) {
    if (!error.is_object()) return;
    const int status = error.value("code", 500);

    json data = json::object();
    const auto found = error.find("data");
    if (found != error.end()) {
        if (found->is_object()) {
            data = *found;
        } else if (!found->is_null()) {
            // A call site that answered with a bare value rather than a map
            // still said something. Keep it where it can be read.
            data["details"] = *found;
        }
    }

    if (!data.contains("code")) data["code"] = errorCodeForStatus(status);
    if (!data.contains("tool")) data["tool"] = tool;
    if (!data.contains("canonical_tool")) data["canonical_tool"] = canonical_tool;
    if (!data.contains("retryable")) data["retryable"] = retryableForStatus(status);

    // What fixes it, when the site did not say (Q6). A fault is the server's.
    const bool fault = status >= 500 && status != 503 && status != 504;
    if (!fault && !hasRemedy(data)) {
        const auto code = data["code"].is_string() ? data["code"].get<std::string>() : std::string();
        const auto message_it = error.find("message");
        const std::string message = message_it != error.end() && message_it->is_string()
                                        ? message_it->get<std::string>() : std::string();
        const auto remedy = remedyForRefusal(code, status, message, canonical_tool, data);
        for (const auto& [key, value] : remedy.items()) {
            if (!data.contains(key)) data[key] = value;
        }
    }

    error["data"] = std::move(data);
}

} // namespace mcp
} // namespace didi
