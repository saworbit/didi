#include "didi/mcp/response_economy.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace didi::mcp {

ResponseEconomy declaredResponseEconomy(const json& capabilities) {
    ResponseEconomy economy;
    if (!capabilities.is_object()) return economy;
    const auto extensions = capabilities.find("extensions");
    if (extensions == capabilities.end() || !extensions->is_object()) return economy;
    const auto declared = extensions->find(kResponseEconomyExtension);
    if (declared == extensions->end() || !declared->is_object()) return economy;
    const auto omit = declared->find("omit");
    if (omit == declared->end() || !omit->is_array()) return economy;
    for (const auto& item : *omit) {
        if (!item.is_string()) continue;
        const auto& name = item.get_ref<const std::string&>();
        if (name == kOmitTextCopy) economy.omit_text_copy = true;
        else if (name == kOmitSessionDescriptor) economy.reference_session = true;
    }
    return economy;
}

json responseEconomyDeclaration() {
    return {{kResponseEconomyExtension,
             {{"omit", json::array({kOmitTextCopy, kOmitSessionDescriptor})}}}};
}

bool SessionDescriptorLedger::alreadySent(const json& descriptor) {
    std::lock_guard<std::mutex> guard(m_mutex);
    if (m_lastSent.has_value() && *m_lastSent == descriptor) return true;
    m_lastSent = descriptor;
    return false;
}

namespace {

bool isDescriptor(const json& session) {
    return session.is_object() && session.contains("session_id") &&
           session["session_id"].is_string();
}

// Enough to say which route answered, which is what a live answer's session
// is for. The kind stays because an editor route and a game route accept
// different tools, and the rest of the descriptor is one runtime_get_session
// away.
json sessionReference(const json& descriptor) {
    json reference = {{"session_id", descriptor["session_id"]}};
    if (descriptor.contains("kind")) reference["kind"] = descriptor["kind"];
    return reference;
}

}  // namespace

json economizeToolResult(json encoded, const ResponseEconomy& economy,
                         const DescriptorHeld& client_holds) {
    if (!economy.any() || !encoded.is_object()) return encoded;
    if (encoded.value("isError", false)) return encoded;
    const auto structured_it = encoded.find("structuredContent");
    const auto content_it = encoded.find("content");
    if (structured_it == encoded.end() || content_it == encoded.end() ||
        !content_it->is_array()) {
        return encoded;
    }

    json& structured = *structured_it;
    json& content = *content_it;
    // Only a text item that is exactly the structured payload is a copy.
    // Anything else, such as an image's caption or a message beside the data,
    // is said nowhere else and is kept whatever the client declared.
    const std::string original = structured.dump();
    std::vector<size_t> copies;
    for (size_t i = 0; i < content.size(); ++i) {
        const auto& item = content[i];
        if (item.is_object() && item.value("type", "") == "text" && item.contains("text") &&
            item["text"].is_string() && item["text"].get_ref<const std::string&>() == original) {
            copies.push_back(i);
        }
    }

    bool restructured = false;
    if (economy.reference_session && structured.is_object() && client_holds) {
        const auto session = structured.find("session");
        if (session != structured.end() && isDescriptor(*session)) {
            const bool held = client_holds(*session);
            // Only a live answer's session is the route that answered it.
            // runtime_attach_session, runtime_get_session and the others answer
            // with a descriptor because the descriptor is the answer, so theirs
            // is always sent whole; seeing one still counts as having sent it.
            if (held && structured.value("execution_mode", "") == "live") {
                *session = sessionReference(*session);
                restructured = true;
            }
        }
    }

    if (economy.omit_text_copy) {
        for (auto it = copies.rbegin(); it != copies.rend(); ++it) {
            content.erase(content.begin() + static_cast<std::ptrdiff_t>(*it));
        }
    } else if (restructured) {
        // The two halves of one result must not disagree, so a copy the
        // client kept says what the structured half now says.
        const std::string rewritten = structured.dump();
        for (const auto index : copies) content[index]["text"] = rewritten;
    }
    return encoded;
}

}  // namespace didi::mcp
