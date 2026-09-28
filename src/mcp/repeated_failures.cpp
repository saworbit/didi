#include "didi/mcp/repeated_failures.hpp"

#include <algorithm>
#include <functional>

namespace didi::mcp {

namespace {

// The fields that say what fixes a refusal, in the order a caller should read
// them; the first one present is the one a repeat points at.
constexpr const char* kRemedyFields[] = {"next_call", "retry_with", "field", "argument",
                                         "parameter", "missing", "did_you_mean",
                                         "restart_with", "retry_after_ms", "no_remedy"};

std::string callKey(const std::string& conversation, const std::string& tool, const json& arguments) {
    json sent = arguments.is_object() ? arguments : json::object();
    // New on every attempt, so it would make every retry a different call.
    sent.erase("confirmation_token");
    // json keeps object keys sorted, so the same arguments dump the same way.
    return conversation + '\n' + tool + '\n' + std::to_string(std::hash<std::string>{}(sent.dump()));
}

}  // namespace

std::optional<json> errorEnvelopeOf(const CallToolResult& result) {
    for (const auto& item : result.content) {
        if (item.type != "text") continue;
        auto payload = json::parse(item.text, nullptr, false);
        if (payload.is_discarded() || !payload.is_object()) continue;
        const auto error = payload.find("error");
        if (error != payload.end() && error->is_object() &&
            error->value("code", json()).is_number_integer()) {
            return *error;
        }
    }
    return std::nullopt;
}

void RepeatedFailures::observe(const std::string& conversation, const std::string& tool,
                               const json& arguments, CallToolResult& result) {
    const auto key = callKey(conversation, tool, arguments);
    const auto envelope = errorEnvelopeOf(result);

    std::lock_guard<std::mutex> guard(m_mutex);
    if (!envelope.has_value()) {
        // A success, or a failure with no envelope to mark: the run is over.
        m_failures.erase(key);
        return;
    }
    const auto data = envelope->value("data", json::object());
    const std::string code = data.is_object() && data.contains("code") && data["code"].is_string()
                                 ? data["code"].get<std::string>()
                                 : std::to_string(envelope->value("code", 0));

    auto found = m_failures.find(key);
    if (found == m_failures.end()) {
        if (m_failures.size() >= kMaxRemembered && !m_order.empty()) {
            m_failures.erase(m_order.front());
            m_order.pop_front();
        }
        // A success leaves its key in the order behind it, so the order is
        // compacted before it can outgrow what it orders.
        if (m_order.size() >= 4 * kMaxRemembered) {
            std::deque<std::string> live;
            for (const auto& remembered : m_order) {
                if (m_failures.count(remembered) != 0) live.push_back(remembered);
            }
            m_order.swap(live);
        }
        m_order.push_back(key);
        found = m_failures.emplace(key, Failure{}).first;
    }
    Failure& failure = found->second;
    failure.count = failure.code == code ? failure.count + 1 : 1;
    failure.code = code;
    if (failure.count < 2) return;

    // The second time and every time after: say so, on the refusal itself.
    std::string follow;
    for (const char* field : kRemedyFields) {
        if (data.is_object() && data.contains(field)) {
            follow = field;
            break;
        }
    }
    json repeated = {{"count", failure.count},
                     {"note", "This call has failed the same way " + std::to_string(failure.count) +
                                  " times in a row. Sent unchanged it will fail again" +
                                  (follow.empty() ? std::string(".")
                                                  : "; do what " + follow + " says first.")}};
    if (!follow.empty()) repeated["follow"] = follow;

    for (auto& item : result.content) {
        if (item.type != "text") continue;
        auto payload = json::parse(item.text, nullptr, false);
        if (payload.is_discarded() || !payload.is_object()) continue;
        auto error = payload.find("error");
        if (error == payload.end() || !error->is_object() ||
            !error->value("code", json()).is_number_integer()) {
            continue;
        }
        auto& error_data = (*error)["data"];
        if (!error_data.is_object()) error_data = json::object();
        error_data["repeated"] = repeated;
        item.text = payload.dump();
        break;
    }
}

}  // namespace didi::mcp
