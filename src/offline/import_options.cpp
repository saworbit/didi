#include "didi/offline/import_options.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <system_error>

namespace didi::offline {
namespace {

// A scene import grows by about 69 KB for each animation configured in it,
// because the engine writes every option the animation has (#1425).
constexpr size_t kMaxSidecarBytes = 16 * 1024 * 1024;
constexpr size_t kMaxOptions = 16;

enum class OptionKind { boolean, seconds, loop_mode, frames, animation_loops };

struct OptionRule {
    const char* importer;
    const char* key;
    OptionKind kind;
};

// The table the amendment records. Each row's type is the one the importer
// declares, identically at 4.5.1-stable, 4.6.2-stable and 4.7.2-stable; the
// MP3 importer moved from modules/minimp3 to modules/mp3 in 4.6 with the same
// options. Widening this table is a contract change with its own probe rows.
constexpr OptionRule kRules[] = {
    {"wav", "edit/loop_mode", OptionKind::loop_mode},
    {"wav", "edit/loop_begin", OptionKind::frames},
    {"wav", "edit/loop_end", OptionKind::frames},
    {"oggvorbisstr", "loop", OptionKind::boolean},
    {"oggvorbisstr", "loop_offset", OptionKind::seconds},
    {"mp3", "loop", OptionKind::boolean},
    {"mp3", "loop_offset", OptionKind::seconds},
    // An imported scene's per-animation loop mode, which the scene importer
    // reads from _subresources.animations.<name>.settings/loop_mode (#1425).
    {"scene", "_subresources", OptionKind::animation_loops},
};

// The WAV importer's enum, in its declared order: Detect From WAV, Disabled,
// Forward, Ping-Pong, Backward. A window applies only under the last three.
constexpr const char* kLoopModeLabels[] = {"Detect From WAV", "Disabled", "Forward", "Ping-Pong",
                                           "Backward"};

std::string lowered(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

std::string joined(const std::vector<std::string>& items) {
    std::string out;
    for (size_t i = 0; i < items.size(); ++i) {
        if (i) out += i + 1 == items.size() ? " and " : ", ";
        out += items[i];
    }
    return out;
}

const OptionRule* ruleFor(const std::string& importer, const std::string& key) {
    for (const auto& rule : kRules) {
        if (importer == rule.importer && key == rule.key) return &rule;
    }
    return nullptr;
}

Error refuse(const std::string& parameter, const std::string& message, json extra = json::object()) {
    json data = {{"code", "invalid_arguments"}, {"parameter", parameter}, {"retryable", false}};
    for (auto& [name, value] : extra.items()) data[name] = value;
    return Error(400, message, data);
}

// The shortest text that reads back as the same double, with a decimal point,
// which is how Godot writes a float it parsed.
std::string numberText(double value) {
    char buffer[64];
    for (int precision = 1; precision <= 17; ++precision) {
        std::snprintf(buffer, sizeof(buffer), "%.*g", precision, value);
        if (std::strtod(buffer, nullptr) == value) break;
    }
    std::string text = buffer;
    if (text.find_first_of(".eEn") == std::string::npos) text += ".0";
    return text;
}

bool sameNumber(double a, double b) {
    return std::fabs(a - b) <= 1e-9 * std::max(1.0, std::max(std::fabs(a), std::fabs(b)));
}

// The last [params] entry for a key, which is the one the engine keeps.
std::vector<const config_file::Entry*> paramsEntries(const ImportSidecar& sidecar,
                                                     const std::string& key) {
    std::vector<const config_file::Entry*> found;
    for (const auto& entry : sidecar.scan.entries) {
        if (entry.section == "params" && entry.key == key) found.push_back(&entry);
    }
    return found;
}

std::optional<int64_t> integerOf(const json& value) {
    if (value.is_number_integer()) return value.get<int64_t>();
    if (value.is_number_unsigned()) {
        const auto unsigned_value = value.get<uint64_t>();
        if (unsigned_value > static_cast<uint64_t>(INT64_MAX)) return std::nullopt;
        return static_cast<int64_t>(unsigned_value);
    }
    return std::nullopt;
}

std::optional<int64_t> currentInteger(const ImportSidecar& sidecar, const std::string& key) {
    const auto entries = paramsEntries(sidecar, key);
    if (entries.empty()) return std::nullopt;
    return integerOf(importParamValue(entries.back()->value_text));
}

// A Godot dictionary literal, parsed only as far as an edit reaches. Each entry
// keeps its key and its value as the text the file holds, so everything an
// edit does not touch is written back as it was. Once an animation is
// configured the engine writes every option it has under it, 2056 keys and
// about 69 KB, measured on 4.5.1, 4.6.2 and 4.7.2, and none of that is this
// tool's to parse (#1425).
struct DictionaryText {
    struct Item {
        std::string key_text;    // as written, quotes and escapes included
        std::string key;         // the string the key holds
        std::string value_text;  // as written, trimmed
    };
    std::vector<Item> items;
};

size_t skipSpace(std::string_view text, size_t at) {
    while (at < text.size() && (text[at] == ' ' || text[at] == '\t' || text[at] == '\n' || text[at] == '\r')) {
        ++at;
    }
    return at;
}

// One past the closing quote of the string that opens at `at`.
std::optional<size_t> quotedEnd(std::string_view text, size_t at) {
    for (size_t i = at + 1; i < text.size(); ++i) {
        if (text[i] == '\\') {
            ++i;
            continue;
        }
        if (text[i] == '"') return i + 1;
    }
    return std::nullopt;
}

// Where the value that starts at `at` ends: the first comma or closing brace
// outside every string and bracket.
std::optional<size_t> valueEnd(std::string_view text, size_t at) {
    std::vector<char> open;
    for (size_t i = at; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '"') {
            const auto end = quotedEnd(text, i);
            if (!end) return std::nullopt;
            i = *end - 1;
        } else if (c == '{' || c == '[' || c == '(') {
            open.push_back(c);
        } else if (c == '}' || c == ']' || c == ')') {
            if (open.empty()) {
                if (c == '}') return i;
                return std::nullopt;
            }
            if (open.back() != (c == '}' ? '{' : c == ']' ? '[' : '(')) return std::nullopt;
            open.pop_back();
        } else if (c == ',' && open.empty()) {
            return i;
        }
    }
    return std::nullopt;
}

// Every key in these dictionaries is a string, as Godot writes them.
std::optional<DictionaryText> parseDictionaryText(std::string_view text) {
    DictionaryText dictionary;
    size_t at = skipSpace(text, 0);
    if (at >= text.size() || text[at] != '{') return std::nullopt;
    at = skipSpace(text, at + 1);
    while (at < text.size()) {
        if (text[at] == '}') {
            if (skipSpace(text, at + 1) != text.size()) return std::nullopt;
            return dictionary;
        }
        if (text[at] != '"') return std::nullopt;
        const auto key_end = quotedEnd(text, at);
        if (!key_end) return std::nullopt;
        DictionaryText::Item item;
        item.key_text = std::string(text.substr(at, *key_end - at));
        const auto decoded = config_file::stringValue(item.key_text);
        if (!decoded || !decoded->problem.empty()) return std::nullopt;
        item.key = decoded->text;
        at = skipSpace(text, *key_end);
        if (at >= text.size() || text[at] != ':') return std::nullopt;
        at = skipSpace(text, at + 1);
        const auto end = valueEnd(text, at);
        if (!end) return std::nullopt;
        item.value_text = strings::trim(text.substr(at, *end - at));
        if (item.value_text.empty()) return std::nullopt;
        dictionary.items.push_back(std::move(item));
        at = *end;
        if (text[at] == ',') at = skipSpace(text, at + 1);
    }
    return std::nullopt;
}

// Godot's own layout for a dictionary: one entry a line, nested ones the same.
std::string dictionaryText(const DictionaryText& dictionary) {
    if (dictionary.items.empty()) return "{}";
    std::string text = "{\n";
    for (size_t i = 0; i < dictionary.items.size(); ++i) {
        if (i) text += ",\n";
        text += dictionary.items[i].key_text + ": " + dictionary.items[i].value_text;
    }
    return text + "\n}";
}

std::string quotedKey(const std::string& key) {
    std::string text = "\"";
    for (const char c : key) {
        switch (c) {
            case '"': text += "\\\""; break;
            case '\\': text += "\\\\"; break;
            case '\n': text += "\\n"; break;
            case '\r': text += "\\r"; break;
            case '\t': text += "\\t"; break;
            default: text += c;
        }
    }
    return text + "\"";
}

// The entry for `key`. False when the dictionary names it twice, which Godot
// never writes.
bool findItem(DictionaryText& dictionary, const std::string& key, DictionaryText::Item*& found) {
    found = nullptr;
    for (auto& item : dictionary.items) {
        if (item.key != key) continue;
        if (found) return false;
        found = &item;
    }
    return true;
}

DictionaryText::Item& itemOrNew(DictionaryText& dictionary, DictionaryText::Item* found, const std::string& key,
                                const std::string& empty_value) {
    if (found) return *found;
    dictionary.items.push_back({quotedKey(key), key, empty_value});
    return dictionary.items.back();
}

// settings/loop_mode under each animation _subresources configures, null for
// one that does not set it. Nothing when the text does not read as Godot
// writes it.
std::optional<std::map<std::string, json>> configuredLoopModes(const std::string& subresources) {
    auto root = parseDictionaryText(subresources);
    if (!root) return std::nullopt;
    std::map<std::string, json> modes;
    DictionaryText::Item* animations = nullptr;
    if (!findItem(*root, "animations", animations)) return std::nullopt;
    if (!animations) return modes;
    auto named = parseDictionaryText(animations->value_text);
    if (!named) return std::nullopt;
    for (const auto& entry : named->items) {
        if (modes.count(entry.key)) return std::nullopt;
        auto settings = parseDictionaryText(entry.value_text);
        if (!settings) return std::nullopt;
        DictionaryText::Item* mode = nullptr;
        if (!findItem(*settings, "settings/loop_mode", mode)) return std::nullopt;
        modes[entry.key] = mode ? importParamValue(mode->value_text) : json();
    }
    return modes;
}

// The scene importer's enum for settings/loop_mode, "None,Linear,Pingpong",
// identical at 4.5.1-stable, 4.6.2-stable and 4.7.2-stable.
constexpr const char* kAnimationLoopLabels[] = {"None", "Linear", "Pingpong"};
constexpr size_t kMaxAnimations = 64;

std::optional<Error> planAnimationLoops(const config_file::Entry& entry, const json& value,
                                        const std::optional<ImportedStreamFacts>& stream,
                                        std::vector<ImportOptionChange>& changes) {
    const std::string key = "_subresources";
    const std::string shape = "{\"animations\": {\"<name>\": {\"settings/loop_mode\": 1}}}";
    if (!value.is_object() || value.size() != 1 || !value.contains("animations")) {
        return refuse("options",
                      "_subresources takes " + shape + ". This tool sets an imported animation's "
                      "loop mode and no other sub-resource option.",
                      {{"key", key}});
    }
    const auto& animations = value["animations"];
    if (!animations.is_object() || animations.empty()) {
        return refuse("options", "_subresources.animations must name at least one animation, as " + shape + ".",
                      {{"key", key}});
    }
    if (animations.size() > kMaxAnimations) {
        return refuse("options", "_subresources.animations may name at most 64 animations.", {{"key", key}});
    }
    const auto current = configuredLoopModes(entry.value_text);
    if (!current) {
        return Error(422,
                     "The _subresources value in this sidecar does not read as the dictionary Godot "
                     "writes. Reimport the asset once in the editor, which rewrites the file, and "
                     "call again.",
                     json{{"code", "hand_edited_import_metadata"}, {"key", key}, {"retryable", false}});
    }
    json known = json::array();
    if (stream && stream->animations) {
        for (const auto& [name, _] : *stream->animations) {
            if (known.size() < kMaxAnimations) known.push_back(name);
        }
    }
    for (const auto& [name, settings] : animations.items()) {
        if (!settings.is_object() || settings.size() != 1 || !settings.contains("settings/loop_mode")) {
            return refuse("options",
                          "_subresources.animations." + name + " takes settings/loop_mode and no "
                          "other animation option.",
                          {{"key", key}, {"animation", name}});
        }
        const auto& requested = settings["settings/loop_mode"];
        std::optional<int64_t> mode = integerOf(requested);
        if (requested.is_string()) {
            const auto wanted = lowered(requested.get<std::string>());
            for (int64_t index = 0; index < 3; ++index) {
                if (wanted == lowered(kAnimationLoopLabels[index])) mode = index;
            }
        }
        if (!mode || *mode < 0 || *mode > 2) {
            return refuse("options",
                          "settings/loop_mode for " + name + " must be 0 None, 1 Linear or 2 Pingpong, "
                          "as the number or the name. Godot loads 3 as a loop mode it has no name "
                          "for, -1 as 255, and a word as 0, without a word.",
                          {{"key", key}, {"animation", name}});
        }
        if (stream && stream->animations && !stream->animations->count(name)) {
            return refuse("options",
                          "The imported scene has no animation named " + name + ". Godot keeps a "
                          "setting for a name it does not have and applies it to nothing.",
                          {{"key", key}, {"animation", name}, {"animations", known}});
        }
        ImportOptionChange change;
        change.key = key;
        change.animation = name;
        change.value = *mode;
        change.text = std::to_string(*mode);
        const auto held = current->find(name);
        if (held != current->end() && !held->second.is_null()) {
            change.previous = held->second;
        } else if (stream && stream->animations && stream->animations->count(name)) {
            // Configured nowhere yet, so the way back is the mode it loads
            // with now.
            change.previous = stream->animations->at(name);
        }
        change.previous_text = change.previous.is_null() ? std::string() : change.previous.dump();
        changes.push_back(std::move(change));
    }
    return std::nullopt;
}

// resource_inspect's view of _subresources: each category and name, and an
// animation's settings/loop_mode, rather than the text, which runs to about
// 69 KB for each animation once one is configured (#1425).
std::optional<json> subresourcesSummary(const std::string& subresources) {
    auto root = parseDictionaryText(subresources);
    if (!root) return std::nullopt;
    json summary = json::object();
    for (const auto& category : root->items) {
        auto named = parseDictionaryText(category.value_text);
        if (!named) return std::nullopt;
        json entries = json::object();
        for (const auto& entry : named->items) {
            json kept = json::object();
            if (category.key == "animations") {
                auto settings = parseDictionaryText(entry.value_text);
                if (!settings) return std::nullopt;
                DictionaryText::Item* mode = nullptr;
                if (findItem(*settings, "settings/loop_mode", mode) && mode) {
                    kept["settings/loop_mode"] = importParamValue(mode->value_text);
                }
            }
            entries[entry.key] = std::move(kept);
        }
        summary[category.key] = std::move(entries);
    }
    return summary;
}

} // namespace

Result<ImportConfigureRequest> parseImportConfigureRequest(const json& arguments) {
    if (!arguments.is_object()) return refuse("arguments", "The arguments must be an object.");
    for (const auto& [name, _] : arguments.items()) {
        if (name != "asset_path" && name != "options") {
            return refuse(name, "Unknown argument '" + name +
                                    "'. This tool accepts: asset_path, options.");
        }
    }
    if (!arguments.contains("asset_path") || !arguments["asset_path"].is_string() ||
        arguments["asset_path"].get<std::string>().empty()) {
        return refuse("asset_path", "asset_path must name one imported asset, as a res:// path.");
    }
    ImportConfigureRequest request;
    request.asset_path = arguments["asset_path"].get<std::string>();
    if (request.asset_path.size() > 1024) {
        return refuse("asset_path", "asset_path must be at most 1024 bytes.");
    }
    const auto lower_path = lowered(request.asset_path);
    if (strings::endsWith(lower_path, ".import")) {
        const auto source = request.asset_path.substr(0, request.asset_path.size() - 7);
        return refuse("asset_path",
                      "asset_path names the asset, not its .import sidecar. The tool finds the "
                      "sidecar beside it.",
                      {{"retry_with", {{"asset_path", source}}}});
    }
    if (strings::startsWith(lower_path, "res://.godot/") || strings::startsWith(lower_path, ".godot/")) {
        return refuse("asset_path",
                      "asset_path is under .godot/, which holds what the editor generated from "
                      "the project. Name the source asset.");
    }
    if (!arguments.contains("options") || !arguments["options"].is_object() ||
        arguments["options"].empty()) {
        return refuse("options",
                      "options must be an object naming at least one import option, such as "
                      "{\"loop\": true}.");
    }
    if (arguments["options"].size() > kMaxOptions) {
        return refuse("options", "options may name at most 16 keys.");
    }
    request.options = arguments["options"];
    return request;
}

Result<ImportSidecar> readImportSidecar(std::string text) {
    ImportSidecar sidecar;
    sidecar.scan = config_file::scan(text);
    if (const auto failure = config_file::loadFailure(sidecar.scan)) {
        std::string reason = failure->unterminated
                                 ? "the file ends part-way through a value"
                                 : failure->value_reason;
        if (reason.empty()) reason = "Godot's parser refuses it";
        return Error(422,
                     "The .import sidecar does not parse, at line " + std::to_string(failure->line) +
                         ": " + reason + ". Godot would reimport the asset with every option at "
                         "its default and a new uid, so repair that line by hand first.",
                     json{{"code", "unparseable_import_metadata"},
                          {"line", failure->line},
                          {"detail", reason},
                          {"retryable", false}});
    }
    for (const auto& entry : sidecar.scan.entries) {
        if (entry.section != "remap") continue;
        const auto quoted = config_file::stringValue(entry.value_text);
        const auto text_value = quoted && quoted->problem.empty() ? quoted->text : entry.value_text;
        if (entry.key == "importer") sidecar.importer = text_value;
        else if (entry.key == "type") sidecar.resource_type = text_value;
        else if (entry.key == "uid") sidecar.uid = text_value;
        else if (entry.key == "valid") sidecar.valid = config_file::booleanize(entry.value_text);
    }
    sidecar.text = std::move(text);
    return sidecar;
}

Result<std::string> readImportSidecarFile(const std::filesystem::path& sidecar) {
    std::error_code error;
    const auto size = std::filesystem::file_size(sidecar, error);
    if (error) return Error::notFound("The .import sidecar is not there");
    if (size > kMaxSidecarBytes) {
        return Error(413, "The .import sidecar is larger than 16 MiB", json{{"code", "file_too_large"}});
    }
    std::ifstream input(sidecar, std::ios::binary);
    if (!input) return Error::notFound("The .import sidecar cannot be opened");
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

std::vector<std::string> importDestinations(const ImportSidecar& sidecar) {
    std::vector<std::string> destinations;
    for (const auto& entry : sidecar.scan.entries) {
        if (entry.section != "deps" || entry.key != "dest_files") continue;
        // Godot writes the value as an array of quoted res:// paths. A path
        // holds no quote, so each quoted run is one entry.
        const std::string& text = entry.value_text;
        size_t cursor = 0;
        while (destinations.size() < 1024) {
            const auto open = text.find('"', cursor);
            if (open == std::string::npos) break;
            const auto close = text.find('"', open + 1);
            if (close == std::string::npos) break;
            if (close > open + 1) destinations.push_back(text.substr(open + 1, close - open - 1));
            cursor = close + 1;
        }
    }
    return destinations;
}

json importParamValue(const std::string& value_text) {
    const auto text = strings::trim(value_text);
    if (text == "true") return true;
    if (text == "false") return false;
    if (text == "null") return nullptr;
    const auto digits = [](std::string_view part) {
        return !part.empty() && std::all_of(part.begin(), part.end(), [](unsigned char c) {
            return std::isdigit(c) != 0;
        });
    };
    std::string_view body = text;
    if (!body.empty() && (body.front() == '-' || body.front() == '+')) body.remove_prefix(1);
    if (digits(body)) {
        errno = 0;
        const auto parsed = std::strtoll(text.c_str(), nullptr, 10);
        if (errno == 0) return static_cast<int64_t>(parsed);
    }
    if (!body.empty() && (std::isdigit(static_cast<unsigned char>(body.front())) != 0 || body.front() == '.')) {
        char* end = nullptr;
        const double parsed = std::strtod(text.c_str(), &end);
        if (end && *end == '\0' && std::isfinite(parsed)) return parsed;
    }
    if (const auto quoted = config_file::stringValue(text); quoted && quoted->problem.empty()) {
        return quoted->text;
    }
    return text;
}

bool configurableImporter(const std::string& importer) {
    return std::any_of(std::begin(kRules), std::end(kRules),
                       [&](const OptionRule& rule) { return importer == rule.importer; });
}

std::vector<std::string> configurableKeys(const std::string& importer) {
    std::vector<std::string> keys;
    for (const auto& rule : kRules) {
        if (importer == rule.importer) keys.emplace_back(rule.key);
    }
    return keys;
}

Result<std::vector<ImportOptionChange>> planImportChanges(
    const ImportSidecar& sidecar, const json& options,
    const std::optional<ImportedStreamFacts>& stream) {
    if (!configurableImporter(sidecar.importer)) {
        return Error(400,
                     "asset_configure_import sets the loop options of WAV, OGG and MP3 imports "
                     "and of an imported scene's animations, and this asset is imported by the \"" +
                         sidecar.importer + "\" importer. resource_inspect lists every option it has.",
                     json{{"code", "invalid_arguments"},
                          {"parameter", "asset_path"},
                          {"importer", sidecar.importer},
                          {"configurable_importers", json::array({"wav", "oggvorbisstr", "mp3", "scene"})},
                          {"retryable", false}});
    }
    const auto keys = configurableKeys(sidecar.importer);
    std::vector<ImportOptionChange> changes;
    for (const auto& [key, value] : options.items()) {
        const auto* rule = ruleFor(sidecar.importer, key);
        if (!rule) {
            json extra = {{"key", key}, {"configurable_keys", keys}};
            std::string hint;
            if (sidecar.importer == "wav" && key == "loop") {
                hint = " A WAV loops by edit/loop_mode: 2 is Forward.";
                extra["retry_with"] = {{"options", {{"edit/loop_mode", 2}}}};
            } else if (sidecar.importer != "wav" && key == "edit/loop_mode") {
                hint = " An " + std::string(sidecar.importer == "mp3" ? "MP3" : "OGG") +
                       " loops by loop: true.";
                extra["retry_with"] = {{"options", {{"loop", true}}}};
            }
            return refuse("options", "\"" + key + "\" is not an option this tool sets on a \"" +
                                         sidecar.importer + "\" import. It sets " + joined(keys) +
                                         "." + hint,
                          extra);
        }
        const auto entries = paramsEntries(sidecar, key);
        if (entries.empty()) {
            return refuse("options",
                          "This sidecar has no " + key + " line under [params], so the engine "
                          "that wrote it does not have that option. Reimport the asset once in "
                          "the editor, which writes every option it has.",
                          {{"key", key}});
        }
        if (rule->kind == OptionKind::animation_loops) {
            if (auto refused = planAnimationLoops(*entries.back(), value, stream, changes)) return *refused;
            continue;
        }
        ImportOptionChange change;
        change.key = key;
        change.previous_text = strings::trim(entries.back()->value_text);
        change.previous = importParamValue(entries.back()->value_text);
        switch (rule->kind) {
            case OptionKind::boolean:
                if (!value.is_boolean()) {
                    return refuse("options", key + " must be true or false. Godot would read "
                                             "any other value as one of them without saying "
                                             "which.",
                                  {{"key", key}});
                }
                change.value = value;
                change.text = value.get<bool>() ? "true" : "false";
                break;
            case OptionKind::seconds: {
                if (!value.is_number() || !std::isfinite(value.get<double>())) {
                    return refuse("options", key + " must be a number of seconds.", {{"key", key}});
                }
                const double seconds = value.get<double>();
                if (seconds < 0) {
                    return refuse("options", key + " must be at least 0. Godot stores a negative "
                                             "offset without complaint.",
                                  {{"key", key}});
                }
                if (stream && stream->length_seconds && seconds >= *stream->length_seconds) {
                    return refuse("options",
                                  key + " must be below the track's length, " +
                                      numberText(*stream->length_seconds) +
                                      " seconds. Godot stores an offset past the end without "
                                      "complaint.",
                                  {{"key", key}, {"length_seconds", *stream->length_seconds}});
                }
                change.value = seconds;
                change.text = numberText(seconds);
                break;
            }
            case OptionKind::loop_mode: {
                std::optional<int64_t> mode = integerOf(value);
                if (value.is_string()) {
                    const auto wanted = lowered(value.get<std::string>());
                    for (int64_t index = 0; index < 5; ++index) {
                        if (wanted == lowered(kLoopModeLabels[index])) mode = index;
                    }
                    if (!mode) {
                        return refuse("options",
                                      key + " must be 0 to 4 or one of Detect From WAV, Disabled, "
                                            "Forward, Ping-Pong and Backward. Godot reads any "
                                            "other word as 0 without saying so.",
                                      {{"key", key}});
                    }
                }
                if (!mode || *mode < 0 || *mode > 4) {
                    return refuse("options",
                                  key + " must be an integer from 0 to 4: Detect From WAV, "
                                        "Disabled, Forward, Ping-Pong or Backward. Godot loads 5 "
                                        "or more as a loop mode it has no name for.",
                                  {{"key", key}});
                }
                change.value = *mode;
                change.text = std::to_string(*mode);
                break;
            }
            case OptionKind::animation_loops:
                break;
            case OptionKind::frames: {
                const auto frames = integerOf(value);
                if (!frames) {
                    return refuse("options", key + " must be a whole number of frames.", {{"key", key}});
                }
                if (key == "edit/loop_begin" && *frames < 0) {
                    return refuse("options", key + " must be at least 0. Godot counts a negative "
                                             "begin back from the end, which nothing declares.",
                                  {{"key", key}});
                }
                if (key == "edit/loop_end" && *frames < 1 && *frames != -1) {
                    return refuse("options", key + " must be at least 1, or -1 for the last "
                                             "frame.",
                                  {{"key", key}});
                }
                change.value = *frames;
                change.text = std::to_string(*frames);
                break;
            }
        }
        changes.push_back(std::move(change));
    }

    // The WAV window, taken as a whole: the mode it runs under and both ends,
    // whichever of them the call changes.
    if (sidecar.importer == "wav") {
        const auto changed = [&](const char* key) -> const ImportOptionChange* {
            for (const auto& change : changes) {
                if (change.key == key) return &change;
            }
            return nullptr;
        };
        const auto* begin_change = changed("edit/loop_begin");
        const auto* end_change = changed("edit/loop_end");
        const auto* mode_change = changed("edit/loop_mode");
        if (begin_change || end_change) {
            const auto mode = mode_change ? integerOf(mode_change->value)
                                          : currentInteger(sidecar, "edit/loop_mode");
            if (!mode || *mode < 2) {
                return refuse("options",
                              "A loop window applies only under edit/loop_mode 2, 3 or 4, "
                              "Forward, Ping-Pong or Backward. Godot ignores it under Detect From "
                              "WAV and Disabled. Set edit/loop_mode in the same call.",
                              {{"key", begin_change ? "edit/loop_begin" : "edit/loop_end"},
                               {"retry_with", {{"options", {{"edit/loop_mode", 2}}}}}});
            }
        }
        std::optional<int64_t> frames;
        if (stream && stream->length_seconds && stream->mix_rate && *stream->mix_rate > 0) {
            frames = static_cast<int64_t>(std::llround(*stream->length_seconds * *stream->mix_rate));
        }
        const auto begin = begin_change ? integerOf(begin_change->value)
                                        : currentInteger(sidecar, "edit/loop_begin");
        auto end = end_change ? integerOf(end_change->value) : currentInteger(sidecar, "edit/loop_end");
        if ((begin_change || end_change) && frames) {
            if (begin && *begin >= *frames) {
                return refuse("options",
                              "edit/loop_begin must be inside the stream, which has " +
                                  std::to_string(*frames) + " frames.",
                              {{"key", "edit/loop_begin"}, {"frames", *frames}});
            }
            if (end && *end > *frames) {
                return refuse("options",
                              "edit/loop_end must be inside the stream, which has " +
                                  std::to_string(*frames) + " frames. Godot stores an end past "
                                  "the stream without complaint.",
                              {{"key", "edit/loop_end"}, {"frames", *frames}});
            }
            if (end && *end == -1) end = frames;
        }
        if ((begin_change || end_change) && begin && end && *end != -1 && *begin >= *end) {
            return refuse("options",
                          "edit/loop_begin must be below edit/loop_end. Godot stores a begin "
                          "after the end without complaint.",
                          {{"key", begin_change ? "edit/loop_begin" : "edit/loop_end"}});
        }
    }

    std::sort(changes.begin(), changes.end(), [](const ImportOptionChange& a, const ImportOptionChange& b) {
        return a.key != b.key ? a.key < b.key : a.animation < b.animation;
    });
    return changes;
}

Result<std::string> applyImportChanges(const ImportSidecar& sidecar,
                                       const std::vector<ImportOptionChange>& changes) {
    // The lines, each with the terminator it had, so a CRLF file stays one
    // and a file with no final newline gains none.
    struct Line {
        std::string content;
        std::string ending;
    };
    std::vector<Line> lines;
    size_t start = 0;
    const auto& text = sidecar.text;
    while (start < text.size()) {
        const auto newline = text.find('\n', start);
        if (newline == std::string::npos) {
            lines.push_back({text.substr(start), ""});
            break;
        }
        const bool crlf = newline > start && text[newline - 1] == '\r';
        lines.push_back({text.substr(start, newline - start - (crlf ? 1 : 0)), crlf ? "\r\n" : "\n"});
        start = newline + 1;
    }

    const auto hand_edited = [](const std::string& key, const std::string& why) {
        return Error(422,
                     "The " + key + " line in this sidecar " + why +
                         ", which Godot never writes. Reimport the asset once in the editor, "
                         "which rewrites the file, and call again.",
                     json{{"code", "hand_edited_import_metadata"}, {"key", key}, {"retryable", false}});
    };
    std::vector<const ImportOptionChange*> animation_loops;
    for (const auto& change : changes) {
        if (!change.animation.empty()) {
            animation_loops.push_back(&change);
            continue;
        }
        const auto entries = paramsEntries(sidecar, change.key);
        const auto hand_edited_here = [&](const std::string& why) { return hand_edited(change.key, why); };
        if (entries.size() != 1) return hand_edited_here("appears " + std::to_string(entries.size()) + " times");
        const auto& entry = *entries.front();
        if (entry.joined || entry.key_line != entry.line || entry.line != entry.value_end_line) {
            return hand_edited_here("spans more than one line");
        }
        for (const auto& other : sidecar.scan.entries) {
            if (&other != &entry && (other.line == entry.line || other.key_line == entry.line ||
                                     (other.key_line <= entry.line && entry.line <= other.value_end_line))) {
                return hand_edited_here("shares its line with another key");
            }
        }
        if (entry.line < 1 || static_cast<size_t>(entry.line) > lines.size()) {
            return hand_edited_here("could not be found");
        }
        lines[static_cast<size_t>(entry.line) - 1].content = change.key + "=" + change.text;
    }

    // An imported scene's animations, all in the one _subresources value,
    // which the engine writes over many lines. The whole value is replaced,
    // and every entry no change reaches keeps its text.
    if (!animation_loops.empty()) {
        const std::string key = "_subresources";
        const auto entries = paramsEntries(sidecar, key);
        if (entries.size() != 1) return hand_edited(key, "appears " + std::to_string(entries.size()) + " times");
        const auto& entry = *entries.front();
        if (entry.joined || entry.key_line != entry.line) return hand_edited(key, "has its key split across lines");
        for (const auto& other : sidecar.scan.entries) {
            if (&other != &entry && other.key_line <= entry.value_end_line && entry.line <= other.value_end_line) {
                return hand_edited(key, "shares its lines with another key");
            }
        }
        if (entry.line < 1 || static_cast<size_t>(entry.value_end_line) > lines.size()) {
            return hand_edited(key, "could not be found");
        }
        auto subresources = parseDictionaryText(entry.value_text);
        if (!subresources) return hand_edited(key, "does not read as a dictionary");
        for (const auto* change : animation_loops) {
            DictionaryText::Item* found = nullptr;
            if (!findItem(*subresources, "animations", found)) return hand_edited(key, "names animations twice");
            auto& animations = itemOrNew(*subresources, found, "animations", "{}");
            auto named = parseDictionaryText(animations.value_text);
            if (!named) return hand_edited(key, "does not read as a dictionary");
            if (!findItem(*named, change->animation, found)) {
                return hand_edited(key, "names animation " + change->animation + " twice");
            }
            auto& animation = itemOrNew(*named, found, change->animation, "{}");
            auto settings = parseDictionaryText(animation.value_text);
            if (!settings) return hand_edited(key, "does not read as a dictionary");
            if (!findItem(*settings, "settings/loop_mode", found)) {
                return hand_edited(key, "names settings/loop_mode twice for " + change->animation);
            }
            itemOrNew(*settings, found, "settings/loop_mode", change->text).value_text = change->text;
            animation.value_text = dictionaryText(*settings);
            animations.value_text = dictionaryText(*named);
        }
        const auto first = static_cast<size_t>(entry.line) - 1;
        const auto last = static_cast<size_t>(entry.value_end_line) - 1;
        const auto inner_ending = lines[first].ending.empty() ? std::string("\n") : lines[first].ending;
        const auto final_ending = lines[last].ending;
        std::vector<Line> replacement;
        const auto written = key + "=" + dictionaryText(*subresources);
        size_t cursor = 0;
        while (true) {
            const auto newline = written.find('\n', cursor);
            if (newline == std::string::npos) {
                replacement.push_back({written.substr(cursor), final_ending});
                break;
            }
            replacement.push_back({written.substr(cursor, newline - cursor), inner_ending});
            cursor = newline + 1;
        }
        lines.erase(lines.begin() + static_cast<std::ptrdiff_t>(first),
                    lines.begin() + static_cast<std::ptrdiff_t>(last) + 1);
        lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(first), replacement.begin(), replacement.end());
    }

    std::string edited;
    edited.reserve(text.size() + 64);
    for (const auto& line : lines) {
        edited += line.content;
        edited += line.ending;
    }
    return edited;
}

std::vector<std::string> sidecarDivergences(const ImportSidecar& after,
                                            const std::vector<ImportOptionChange>& changes,
                                            const std::string& uid_before) {
    std::vector<std::string> found;
    for (const auto& change : changes) {
        const auto entries = paramsEntries(after, change.key);
        if (entries.empty()) {
            found.push_back(change.key + " is no longer in the sidecar");
            continue;
        }
        if (!change.animation.empty()) {
            const auto modes = configuredLoopModes(entries.back()->value_text);
            if (!modes) {
                found.push_back("_subresources does not read as a dictionary after the reimport");
                continue;
            }
            const auto held = modes->find(change.animation);
            if (held == modes->end() || held->second != change.value) {
                found.push_back("animation " + change.animation + " has settings/loop_mode " +
                                (held == modes->end() ? std::string("unset") : held->second.dump()) +
                                " in the sidecar after the reimport, not " + change.text);
            }
            continue;
        }
        const auto held = importParamValue(entries.back()->value_text);
        bool same = false;
        if (change.value.is_boolean()) {
            same = held.is_boolean() && held == change.value;
        } else if (change.value.is_number() && held.is_number()) {
            same = sameNumber(held.get<double>(), change.value.get<double>());
        }
        if (!same) {
            found.push_back(change.key + " reads " + strings::trim(entries.back()->value_text) +
                            " after the reimport, not " + change.text);
        }
    }
    if (after.valid.has_value() && !*after.valid) {
        found.push_back("the sidecar records valid=false: Godot could not import the asset");
    }
    if (after.uid != uid_before) {
        found.push_back("the uid changed from " + uid_before + " to " + after.uid);
    }
    return found;
}

std::vector<std::string> streamDivergences(const json& stream,
                                           const std::vector<ImportOptionChange>& changes) {
    std::vector<std::string> found;
    const auto properties = stream.is_object() ? stream.value("properties", json::object()) : json::object();
    const auto read = [&](const char* name) -> json {
        return properties.is_object() && properties.contains(name) ? properties[name] : json();
    };
    const auto animations = stream.is_object() ? stream.value("animations", json::object()) : json::object();
    for (const auto& change : changes) {
        if (!change.animation.empty()) {
            const auto loaded = animations.is_object() && animations.contains(change.animation)
                                    ? animations[change.animation]
                                    : json();
            if (loaded != change.value) {
                found.push_back("animation " + change.animation + " loads with loop_mode " + loaded.dump() +
                                ", not " + change.text);
            }
        } else if (change.key == "loop") {
            if (read("loop") != change.value) {
                found.push_back("the stream loads with loop " + read("loop").dump() + ", not " + change.text);
            }
        } else if (change.key == "loop_offset") {
            const auto offset = read("loop_offset");
            if (!offset.is_number() || !sameNumber(offset.get<double>(), change.value.get<double>())) {
                found.push_back("the stream loads with loop_offset " + offset.dump() + ", not " + change.text);
            }
        } else if (change.key == "edit/loop_mode") {
            // The importer's option counts from Detect From WAV and the
            // stream's LoopMode from Disabled, so option 2, Forward, loads as
            // 1. Detect From WAV depends on the file and is not predicted.
            const auto mode = change.value.get<int64_t>();
            if (mode >= 1 && read("loop_mode") != json(mode - 1)) {
                found.push_back("the stream loads with loop_mode " + read("loop_mode").dump() +
                                ", not " + std::to_string(mode - 1) + " for edit/loop_mode " + change.text);
            }
        } else if (change.key == "edit/loop_begin") {
            if (read("loop_begin") != change.value) {
                found.push_back("the stream loads with loop_begin " + read("loop_begin").dump() +
                                ", not " + change.text);
            }
        } else if (change.key == "edit/loop_end") {
            if (change.value.get<int64_t>() != -1 && read("loop_end") != change.value) {
                found.push_back("the stream loads with loop_end " + read("loop_end").dump() +
                                ", not " + change.text);
            }
        }
    }
    return found;
}

json describeImportSidecar(const std::string& text) {
    auto sidecar = readImportSidecar(text);
    if (sidecar.isErr()) {
        return {{"parse_error", sidecar.error().data.value("detail", sidecar.error().message)},
                {"line", sidecar.error().data.value("line", 0)}};
    }
    json options = json::object();
    for (const auto& entry : sidecar.value().scan.entries) {
        if (entry.section != "params") continue;
        if (entry.key == "_subresources") {
            if (auto summary = subresourcesSummary(entry.value_text)) {
                options[entry.key] = std::move(*summary);
                continue;
            }
        }
        options[entry.key] = importParamValue(entry.value_text);
    }
    json described = {{"importer", sidecar.value().importer},
                      {"type", sidecar.value().resource_type},
                      {"uid", sidecar.value().uid},
                      {"options", std::move(options)},
                      {"configurable", configurableKeys(sidecar.value().importer)}};
    if (sidecar.value().valid.has_value()) described["valid"] = *sidecar.value().valid;
    return described;
}

} // namespace didi::offline
