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

constexpr size_t kMaxSidecarBytes = 1024 * 1024;
constexpr size_t kMaxOptions = 16;

enum class OptionKind { boolean, seconds, loop_mode, frames };

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
        return Error(413, "The .import sidecar is larger than 1 MiB", json{{"code", "file_too_large"}});
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
                     "asset_configure_import sets the loop options of WAV, OGG and MP3 imports, "
                     "and this asset is imported by the \"" + sidecar.importer +
                         "\" importer. resource_inspect lists every option it has.",
                     json{{"code", "invalid_arguments"},
                          {"parameter", "asset_path"},
                          {"importer", sidecar.importer},
                          {"configurable_importers", json::array({"wav", "oggvorbisstr", "mp3"})},
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

    std::sort(changes.begin(), changes.end(),
              [](const ImportOptionChange& a, const ImportOptionChange& b) { return a.key < b.key; });
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

    for (const auto& change : changes) {
        const auto entries = paramsEntries(sidecar, change.key);
        const auto hand_edited = [&](const std::string& why) {
            return Error(422,
                         "The " + change.key + " line in this sidecar " + why +
                             ", which Godot never writes. Reimport the asset once in the editor, "
                             "which rewrites the file, and call again.",
                         json{{"code", "hand_edited_import_metadata"},
                              {"key", change.key},
                              {"retryable", false}});
        };
        if (entries.size() != 1) return hand_edited("appears " + std::to_string(entries.size()) + " times");
        const auto& entry = *entries.front();
        if (entry.joined || entry.key_line != entry.line || entry.line != entry.value_end_line) {
            return hand_edited("spans more than one line");
        }
        for (const auto& other : sidecar.scan.entries) {
            if (&other != &entry && (other.line == entry.line || other.key_line == entry.line ||
                                     (other.key_line <= entry.line && entry.line <= other.value_end_line))) {
                return hand_edited("shares its line with another key");
            }
        }
        if (entry.line < 1 || static_cast<size_t>(entry.line) > lines.size()) {
            return hand_edited("could not be found");
        }
        lines[static_cast<size_t>(entry.line) - 1].content = change.key + "=" + change.text;
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
    for (const auto& change : changes) {
        if (change.key == "loop") {
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
        if (entry.section == "params") options[entry.key] = importParamValue(entry.value_text);
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
