#include "didi/runtime/audio_requests.hpp"

#include <cmath>

namespace didi {
namespace runtime {

namespace {

constexpr size_t kMaxBusNameBytes = 256;

Error invalid(const char* parameter, std::string message, json extra = json::object()) {
    json data = {{"code", "invalid_arguments"}, {"parameter", parameter}, {"retryable", false}};
    if (extra.is_object()) data.update(extra);
    return Error(400, std::move(message), std::move(data));
}

bool isControl(unsigned char byte) {
    return byte < 0x20 || byte == 0x7f;
}

bool isSpace(char character) {
    return character == ' ';
}

std::string trimmedSpaces(const std::string& text) {
    size_t first = 0;
    while (first < text.size() && isSpace(text[first])) ++first;
    size_t last = text.size();
    while (last > first && isSpace(text[last - 1])) --last;
    return text.substr(first, last - first);
}

} // namespace

Result<AudioAddBusRequest> parseAudioAddBusRequest(const json& params) {
    if (!params.is_object()) return invalid("arguments", "audio_add_bus arguments must be an object");
    for (auto it = params.begin(); it != params.end(); ++it) {
        const auto& key = it.key();
        if (key != "name" && key != "send" && key != "volume_db" && key != "mute" &&
            key != "solo" && key != "preview") {
            return invalid(key.c_str(), "Unknown argument '" + key +
                                            "'. audio_add_bus takes name, send, volume_db, mute "
                                            "and solo.");
        }
    }

    AudioAddBusRequest request;
    if (!params.contains("name") || !params["name"].is_string()) {
        return invalid("name", "name is required, as a string: the name the new bus is given");
    }
    request.name = params["name"].get<std::string>();
    if (request.name.empty()) {
        return invalid("name", "name may not be empty. Godot keeps an empty bus name, and no "
                               "tool or player can name that bus afterwards.");
    }
    if (request.name.size() > kMaxBusNameBytes) {
        return invalid("name", "name must be at most 256 bytes");
    }
    for (const char character : request.name) {
        if (isControl(static_cast<unsigned char>(character))) {
            return invalid("name", "name may not contain a control character such as a tab or a "
                                   "line break. Godot keeps one, and the Audio panel shows it as "
                                   "nothing.");
        }
    }
    const auto trimmed = trimmedSpaces(request.name);
    if (trimmed.empty()) {
        return invalid("name", "name may not be spaces alone");
    }
    if (trimmed != request.name) {
        return invalid("name",
                       "name may not begin or end with a space. Godot keeps it, so \"" +
                           request.name + "\" would be a different bus from \"" + trimmed +
                           "\" that nobody can tell apart in the Audio panel.",
                       {{"retry_with", {{"name", trimmed}}}});
    }

    if (params.contains("send")) {
        const auto& send = params["send"];
        if (!send.is_string()) return invalid("send", "send must be a string: the bus to send to");
        request.send = send.get<std::string>();
        if (request.send.empty()) {
            // The engine's own default for a new bus, and it routes to Master.
            // One spelling for one route, so a reader never has to know that.
            return invalid("send",
                           "send may not be empty. An empty send routes to Master, so name "
                           "Master, or leave send out.",
                           {{"retry_with", {{"send", "Master"}}}});
        }
        if (request.send.size() > kMaxBusNameBytes) {
            return invalid("send", "send must be at most 256 bytes");
        }
    }

    if (params.contains("volume_db")) {
        const auto& value = params["volume_db"];
        if (!value.is_number()) return invalid("volume_db", "volume_db must be a number");
        const double db = value.get<double>();
        // The range audio_configure_bus takes, and for the same reason: the
        // engine bus editor spans -80 to 24 decibels, and a value outside it is
        // a linear gain or a slipped digit that clamping would hide.
        if (!std::isfinite(db) || db < -80.0 || db > 24.0) {
            return invalid("volume_db", "volume_db must be between -80 and 24 decibels");
        }
        request.volume_db = db;
    }
    for (const char* flag : {"mute", "solo"}) {
        if (!params.contains(flag)) continue;
        if (!params[flag].is_boolean()) {
            return invalid(flag, std::string(flag) + " must be a boolean");
        }
        (std::string(flag) == "mute" ? request.mute : request.solo) = params[flag].get<bool>();
    }
    if (params.contains("preview")) {
        if (!params["preview"].is_boolean()) return invalid("preview", "preview must be a boolean");
        request.preview = params["preview"].get<bool>();
    }
    return request;
}

} // namespace runtime
} // namespace didi
