#include "didi/gdextension/engine_diagnostics.hpp"

#include <string>

namespace didi::godot {
namespace {

// A record from the ring, cut down to what a caller reads: the level, the
// sentence, and where in the engine it came from when the engine said.
json diagnosticFrom(const json& record) {
    json entry = json::object();
    entry["level"] = record.value("level", std::string("error"));
    std::string message = record.value("message", std::string());
    if (message.size() > kMaxEngineDiagnosticBytes) {
        // Cut on a character boundary, not a byte one: a split UTF-8 sequence
        // would make the whole answer invalid JSON text for a strict reader.
        size_t cut = kMaxEngineDiagnosticBytes;
        while (cut > 0 && (static_cast<unsigned char>(message[cut]) & 0xC0) == 0x80) --cut;
        message.resize(cut);
        entry["message_truncated"] = true;
    }
    entry["message"] = message;
    const auto details = record.find("details");
    if (details != record.end() && details->is_object()) {
        for (const auto* key : {"function", "file", "line"}) {
            const auto found = details->find(key);
            if (found != details->end() && !found->is_null()) entry[key] = *found;
        }
    }
    return entry;
}

} // namespace

void attachEngineDiagnostics(json& response, const json& records, size_t total, size_t limit) {
    if (!response.is_object() || !records.is_array() || records.empty()) return;
    json* target = &response;
    if (response.contains("error") && response["error"].is_object()) {
        auto& error = response["error"];
        if (!error.contains("data") || !error["data"].is_object()) error["data"] = json::object();
        target = &error["data"];
    }
    if (target->contains("engine_diagnostics")) return;
    json diagnostics = json::array();
    for (const auto& record : records) {
        if (diagnostics.size() >= limit) break;
        if (record.is_object()) diagnostics.push_back(diagnosticFrom(record));
    }
    if (diagnostics.empty()) return;
    (*target)["engine_diagnostics"] = std::move(diagnostics);
    const size_t shown = (*target)["engine_diagnostics"].size();
    if (total > shown) (*target)["engine_diagnostics_omitted"] = total - shown;
    (*target)["engine_diagnostics_note"] =
        "The engine printed these ERROR or WARNING lines while this call was running. They "
        "are the engine's own words; what the call did is the rest of this answer.";
}

bool methodReportsEngineOutputItself(std::string_view method) {
    return method == "runtime.getOutput" || method == "runtime.getLogs" ||
           method == "runtime.watchInvariants";
}

} // namespace didi::godot
