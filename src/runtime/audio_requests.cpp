#include "didi/runtime/audio_requests.hpp"

#include "didi/common/project_path.hpp"

#include <cmath>
#include <cstdio>
#include <vector>

namespace didi {
namespace runtime {

namespace {

// Characters, the unit the published schema's maxLength counts in (#663).
constexpr size_t kMaxBusNameCharacters = 256;

Error invalid(const char* parameter, std::string message, json extra = json::object()) {
    json data = {{"code", "invalid_arguments"}, {"parameter", parameter}, {"retryable", false}};
    if (extra.is_object()) data.update(extra);
    return Error(400, std::move(message), std::move(data));
}

// One code point and where its bytes are. The text came out of a JSON parser,
// so it is well-formed UTF-8; a stray byte is taken as itself rather than
// trusted to be impossible.
struct CodePoint {
    char32_t value;
    size_t offset;
    size_t length;
};

std::vector<CodePoint> codePoints(const std::string& text) {
    std::vector<CodePoint> points;
    size_t index = 0;
    while (index < text.size()) {
        const auto lead = static_cast<unsigned char>(text[index]);
        size_t length = 1;
        char32_t value = lead;
        if (lead >= 0xF0) { length = 4; value = lead & 0x07; }
        else if (lead >= 0xE0) { length = 3; value = lead & 0x0F; }
        else if (lead >= 0xC0) { length = 2; value = lead & 0x1F; }
        if (index + length > text.size()) { length = 1; value = lead; }
        for (size_t next = 1; next < length; ++next) {
            value = (value << 6) | (static_cast<unsigned char>(text[index + next]) & 0x3F);
        }
        points.push_back({value, index, length});
        index += length;
    }
    return points;
}

std::string codePointName(char32_t value) {
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "U+%04X", static_cast<unsigned>(value));
    return buffer;
}

// A character that is not text: the C0 and C1 controls and DEL, the line and
// paragraph separators, and the bidirectional embeddings, overrides and
// isolates, which reorder how the rest of the name is drawn. Godot keeps every
// one of them in a bus name. The C1 range and U+2028 are line breaks to plenty
// of renderers, and only their ASCII cousins used to be refused.
bool isControl(char32_t value) {
    return value < 0x20 || value == 0x7F || (value >= 0x80 && value <= 0x9F) ||
           value == 0x2028 || value == 0x2029 || (value >= 0x202A && value <= 0x202E) ||
           (value >= 0x2066 && value <= 0x2069);
}

// A character a person sees as a space or as nothing at all: Unicode's
// White_Space, and the default-ignorable characters that are drawn as nothing
// on their own. The Audio panel shows " Music" with a no-break space, and
// "Music" behind a zero width space or a byte-order mark, exactly as it shows
// "Music". Variation selectors and emoji tags are left out, because they end
// an emoji legitimately, and so is the zero width joiner inside one; only the
// ends of a name and a name made of nothing else are refused.
bool isBlank(char32_t value) {
    switch (value) {
        case 0x20: case 0xA0: case 0xAD: case 0x34F: case 0x61C: case 0x115F: case 0x1160:
        case 0x1680: case 0x17B4: case 0x17B5: case 0x180E: case 0x202F: case 0x205F:
        case 0x3000: case 0x3164: case 0xFEFF: case 0xFFA0:
            return true;
        default:
            break;
    }
    return (value >= 0x2000 && value <= 0x200F) || (value >= 0x2060 && value <= 0x206F);
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
    if (paths::codePointCount(request.name) > kMaxBusNameCharacters) {
        return invalid("name", "name must be at most 256 characters");
    }
    const auto points = codePoints(request.name);
    for (const auto& point : points) {
        if (isControl(point.value)) {
            return invalid("name", "name may not contain a control character such as a tab or a "
                                   "line break, and it holds " + codePointName(point.value) +
                                   ". Godot keeps one, and the Audio panel shows it as nothing "
                                   "or breaks the line there.",
                           {{"character", codePointName(point.value)}});
        }
    }
    size_t first = 0;
    while (first < points.size() && isBlank(points[first].value)) ++first;
    size_t last = points.size();
    while (last > first && isBlank(points[last - 1].value)) --last;
    if (first == last) {
        return invalid("name", "name may not be spaces or invisible characters alone. Godot "
                               "keeps such a name, and the Audio panel shows a bus with no name.");
    }
    if (first > 0 || last < points.size()) {
        const size_t begin = points[first].offset;
        const size_t end = points[last - 1].offset + points[last - 1].length;
        const std::string trimmed = request.name.substr(begin, end - begin);
        const char32_t edge = first > 0 ? points[0].value : points.back().value;
        // A no-break or ideographic space is drawn as a space, not as nothing,
        // and the sentence says which of the two the caller sent.
        const bool white_space = edge == 0x20 || edge == 0xA0 || edge == 0x1680 ||
                                 (edge >= 0x2000 && edge <= 0x200A) || edge == 0x202F ||
                                 edge == 0x205F || edge == 0x3000;
        const std::string what = edge == 0x20 ? std::string("a space")
                                 : white_space ? "a space (" + codePointName(edge) + ")"
                                               : "an invisible character (" + codePointName(edge) + ")";
        return invalid("name",
                       "name may not begin or end with a space or an invisible character, and it "
                       "has " + what + " there. Godot keeps it, so \"" + request.name +
                           "\" would be a different bus from \"" + trimmed +
                           "\" that nobody can tell apart in the Audio panel.",
                       {{"retry_with", {{"name", trimmed}}}, {"character", codePointName(edge)}});
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
        if (paths::codePointCount(request.send) > kMaxBusNameCharacters) {
            return invalid("send", "send must be at most 256 characters");
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

Error busNameChangedRefusal(const std::string& asked, const std::string& stored,
                            bool asked_name_taken) {
    if (asked_name_taken) {
        return Error(409,
                     "A bus named \"" + asked + "\" appeared between the check and this call, "
                     "so the engine named the new one \"" + stored + "\". The new bus was "
                     "removed again. audio_configure_bus changes the bus that has the name.",
                     json{{"code", "bus_name_in_use"}, {"name", asked}, {"retryable", false}});
    }
    return Error(409,
                 "The engine stored the name as \"" + stored + "\" rather than \"" + asked +
                     "\", and no other bus holds \"" + asked + "\", so the engine changed the "
                     "name on its way in rather than another bus taking it. The bus was removed "
                     "again. A retry is changed the same way.",
                 json{{"code", "bus_name_changed_by_engine"}, {"name", asked},
                      {"stored_as", stored}, {"retryable", false}});
}

} // namespace runtime
} // namespace didi
