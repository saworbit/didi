#include "didi/offline/deep_domain_support.hpp"

#include "didi/common/config_file_syntax.hpp"
#include "didi/common/project_path.hpp"
#include <map>
#include <algorithm>
#include <cctype>
#include <regex>
#include <fstream>
#include <sstream>
#include <optional>
#include <filesystem>
#include <set>

namespace didi::offline {

std::vector<std::string> isolatedGodotArguments(std::vector<std::string> arguments) {
    std::vector<std::string> isolated = {"--headless"};
    isolated.reserve(arguments.size() + 1);
    for (auto& argument : arguments) isolated.push_back(std::move(argument));
    return isolated;
}
namespace {

constexpr size_t kMaxDiagnostics = 1000;

// A preset field as the engine reads it. This undid `\"` and `\\` and no other
// escape, so a `\t` in a name came back as two characters; the rule is the
// parser's, shared with every other reader (#934). A string the parser refuses
// has already made the file `unloadable_value`, and is returned as written.
std::string unquote(const std::string& value) {
    if (auto decoded = config_file::stringValue(value); decoded && decoded->problem.empty()) {
        return std::move(decoded->text);
    }
    return std::string(strings::trim(value));
}

} // namespace

json DomainDiagnostic::toJson() const {
    json value = {{"severity", severity}, {"message", message}};
    if (!code.empty()) value["code"] = code;
    if (!path.empty()) value["path"] = path;
    if (line > 0) value["line"] = line;
    if (column > 0) value["column"] = column;
    return value;
}

std::vector<DomainDiagnostic> parseMsBuildDiagnostics(const std::string& output) {
    // Roslyn emits both (line,col) and four part (startLine,startCol,endLine,
    // endCol) spans, and diagnostic codes are not always letters then digits:
    // NETSDK1004, MSB3073 and analyzer ids all turn up here.
    //
    // The code is optional. MSBuild documents the shape as
    // "[Origin:] [Subcategory] category [Code]: [Text]", and NuGet uses the
    // codeless form for the one that matters most here:
    // "NuGet.targets(196,5): warning : Unable to find a project to restore!".
    // Requiring a code dropped it, so a build that compiled nothing reported
    // zero diagnostics (#702).
    static const std::regex pattern(
        R"(^(.+)\(([0-9]+),([0-9]+)(?:,[0-9]+,[0-9]+)?\):\s*(error|warning)\s+(?:([A-Za-z][A-Za-z0-9_.-]*[0-9][A-Za-z0-9_.-]*)\s*)?:\s*(.*?)(?:\s+\[[^\]]+\])?\s*$)",
        std::regex::icase);
    std::vector<DomainDiagnostic> diagnostics;
    // The console logger prints every diagnostic twice: once as it happens and
    // once in the Summary block it appends by default, so every count was
    // doubled (#702). The two printings are the same line, byte for byte,
    // project suffix included, so the trimmed line is the identity. Keying on
    // that rather than on the parsed fields keeps the genuinely distinct pair a
    // shared source file compiled by two projects produces, which differ only
    // in that suffix. Matching on text also means no reliance on the wording of
    // "Build FAILED.", which is localised.
    std::set<std::string> seen;
    for (const auto& raw : strings::split(output, '\n')) {
        if (diagnostics.size() >= kMaxDiagnostics) break;
        std::smatch match;
        const std::string line = strings::trim(raw);
        if (!std::regex_match(line, match, pattern)) continue;
        if (!seen.insert(line).second) continue;
        std::string severity = match[4].str();
        std::transform(severity.begin(), severity.end(), severity.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        diagnostics.push_back({severity, match[5].str(), strings::trim(match[6].str()),
                               match[1].str(), std::stoi(match[2].str()), std::stoi(match[3].str())});
    }
    return diagnostics;
}

int parseMsBuildProjectOutputCount(const std::string& output) {
    // "  Game -> D:\game\bin\Debug\net8.0\Game.dll". A diagnostic can carry an
    // arrow in its message text, so the diagnostic shape is excluded first
    // rather than trusted not to collide.
    static const std::regex diagnostic_shape(
        R"(^.+\([0-9]+,[0-9]+(?:,[0-9]+,[0-9]+)?\):\s*(?:error|warning)\b.*$)", std::regex::icase);
    static const std::regex output_line(R"(^([^>]+?)\s->\s(\S.*)$)");
    std::set<std::string> projects;
    for (const auto& raw : strings::split(output, '\n')) {
        const std::string line = strings::trim(raw);
        if (line.empty()) continue;
        if (std::regex_match(line, diagnostic_shape)) continue;
        std::smatch match;
        if (!std::regex_match(line, match, output_line)) continue;
        projects.insert(strings::trim(match[1].str()) + " -> " + strings::trim(match[2].str()));
    }
    return static_cast<int>(projects.size());
}

std::vector<DomainDiagnostic> parseGodotDiagnostics(const std::string& output) {
    static const std::regex resource_pattern(
        R"(^\s*(?:ERROR|SCRIPT ERROR):\s*(?:Parse Error:\s*)?(res://[^:]+):([0-9]+)\s*(?:-|:)\s*(.+?)\s*$)",
        std::regex::icase);
    static const std::regex trailing_resource_pattern(
        R"(^\s*(?:ERROR|SCRIPT ERROR):\s*(?:Parse Error:\s*)?(.+?)\s+in\s+(res://[^:]+):([0-9]+)\s*$)",
        std::regex::icase);
    static const std::regex shader_pattern(
        R"(^\s*SHADER ERROR:\s*(.+?)\s*$)", std::regex::icase);
    // Godot 4 prints shader locations several ways: "at: (res://x.gdshader:14)",
    // "at: (:14)" and a bare "at: :14". The engine also prints its own C++ frames
    // in the same shape, so a location that names a C++ source file is not the
    // user's shader line and must not be taken as one.
    // The leading (?:\S+\s+)* skips whatever Godot puts before the location: a
    // function name, or the literal "(null)" the dummy renderer prints.
    static const std::regex shader_location_pattern(
        R"(^\s*at:\s*(?:\S+\s+)*\(?\s*([^()\s]*?)\s*:\s*([0-9]+)\s*\)?\s*$)",
        std::regex::icase);
    const auto isEngineSource = [](const std::string& path) {
        static const std::regex engine_source(R"(\.(?:cpp|cc|cxx|h|hpp|inl)$)", std::regex::icase);
        return std::regex_search(path, engine_source);
    };
    std::vector<DomainDiagnostic> diagnostics;
    for (const auto& raw : strings::split(output, '\n')) {
        if (diagnostics.size() >= kMaxDiagnostics) break;
        std::smatch match;
        if (std::regex_match(raw, match, resource_pattern)) {
            diagnostics.push_back({"error", "", strings::trim(match[3].str()), match[1].str(),
                                   std::stoi(match[2].str()), 0});
        } else if (std::regex_match(raw, match, trailing_resource_pattern)) {
            diagnostics.push_back({"error", "", strings::trim(match[1].str()), match[2].str(),
                                   std::stoi(match[3].str()), 0});
        } else if (std::regex_match(raw, match, shader_pattern)) {
            diagnostics.push_back({"error", "GODOT_SHADER", strings::trim(match[1].str()), "", 0, 0});
        } else if (!diagnostics.empty() && diagnostics.back().code == "GODOT_SHADER" &&
                   diagnostics.back().line == 0 &&
                   std::regex_match(raw, match, shader_location_pattern)) {
            const auto path = match[1].str();
            if (isEngineSource(path)) continue;
            diagnostics.back().line = std::stoi(match[2].str());
            if (!path.empty() && diagnostics.back().path.empty()) {
                diagnostics.back().path = path;
            }
        }
    }
    return diagnostics;
}

ExportPresetsFile readExportPresets(const std::string& contents) {
    // Matched against the section name the engine reads, not against the raw
    // line. `[ preset.0 ]` is the section preset.0, so an anchored whole-line
    // pattern skipped every key under it and reported a project with a working
    // export preset as a project with none (#814).
    static const std::regex preset_section(R"(^preset\.([0-9]+)$)");
    std::vector<json> presets;
    std::map<std::string, size_t> preset_of_section;
    // Per preset, in the same order: the section name as written, and whether
    // it is the spelling the engine asks for. It asks for "preset." followed
    // by the number with no leading zero, so [preset.01] is never read.
    std::vector<std::string> section_names;
    std::vector<bool> canonical_section;
    bool malformed = false;

    // The first cause wins and the rest are not collected. A file with two
    // faults is repaired one at a time, and the first one is the one the reader
    // reaches: everything after a value the parser will not start is behind an
    // ERR_PARSE_ERROR anyway.
    std::string reason;
    std::string detail;
    int line = 0;
    const auto note = [&](const char* cause, std::string sentence, int at) {
        malformed = true;
        if (!reason.empty()) return;
        reason = cause;
        detail = std::move(sentence);
        line = at;
    };

    // `#` is not a comment in a ConfigFile. A `#` line with an `=` is a key
    // whose name carries the hash, and a `#` line without one joins forward
    // into the next line that has one, taking any section header in between
    // with it. Skipping those lines read a file whose presets are not what it
    // appears to say as a file that parsed (#812), and the key rule underneath
    // both is #813.
    // Godot does not skip a UTF-8 byte-order mark here: the mark joins the
    // first section header, the header becomes part of a key, and 4.5.1, 4.6.2
    // and 4.7.2 all detect no presets. It is what PowerShell 5.1's Set-Content
    // and older editors write, and the cause below it named a key the reader
    // could not see.
    if (contents.rfind("\xEF\xBB\xBF", 0) == 0) {
        note("byte_order_mark",
             "export_presets.cfg starts with a UTF-8 byte-order mark, and Godot does not skip "
             "one in this file: it reads the first section header as part of a key and detects "
             "no presets at all. Save the file as UTF-8 without a byte-order mark.",
             1);
    }
    const auto scanned = config_file::scan(contents);
    // A file that ends inside a value is ERR_PARSE_ERROR for the engine and
    // none of it loads, so nothing read above it describes an export.
    if (!scanned.complete) {
        // The last key read is the one whose value never closed.
        const int at = scanned.entries.empty() ? 0 : scanned.entries.back().line;
        note("truncated_value",
             std::string("export_presets.cfg ends part-way through a value") +
                 (at > 0 ? ", starting on line " + std::to_string(at) : "") +
                 ", so Godot answers ERR_PARSE_ERROR for the whole file and the editor "
                 "detects no presets at all. That is a truncated write rather than a preset "
                 "to correct, so repair the line or delete the file and export once from the "
                 "editor to write a new one.",
             at);
    }
    // Balanced is not loadable either. `export_path=)` closes every bracket it
    // opens, so counting brackets let it through and the list published a
    // preset with `)` as the path an export would write to (#823). Godot's own
    // answer to that file is `Invalid export preset name: Windows` with an
    // empty list of detected presets, on 4.5.1, 4.6.2 and 4.7.2 -- the keys
    // ahead of the bad value parse and the editor still has no preset. So a
    // value this cannot start is the whole file, not one field.
    for (const auto& entry : scanned.entries) {
        const auto problem = config_file::valueProblem(entry.value_text);
        if (problem.empty()) continue;
        // The sentence and the line were both already in hand here and both
        // were dropped. project_audit_assets publishes the pair for the same
        // failure in project.godot and in a .import.
        note("unloadable_value",
             "Godot's parser refuses the value of \"" + entry.key + "\" on line " +
                 std::to_string(entry.line) + ", because " + problem +
                 ". It answers ERR_PARSE_ERROR for the whole file, so the editor detects no "
                 "presets even though the keys ahead of this one are fine.",
             entry.line);
        break;
    }
    // A file with content but no section the engine honours is not an ini. A
    // trailing `# note` under a preset is not that, which is the difference a
    // one-token fix could not draw (#812), and an ini whose sections are all
    // something else is a project with no export presets rather than a broken
    // file (#651).
    if (scanned.headers.empty() && (!scanned.entries.empty() || scanned.trailing_key)) {
        note("no_section_header",
             "export_presets.cfg has content and no section header the engine honours, so it "
             "is not the file Godot writes. Delete it and export once from the editor to "
             "write a new one.",
             0);
    }

    for (const auto& header : scanned.headers) {
        std::smatch match;
        if (!std::regex_match(header.name, match, preset_section)) continue;
        if (preset_of_section.count(header.name) != 0) continue;
        // A run of digits longer than any real preset index would throw out of
        // std::stoi rather than parse, and a section nobody wrote is not worth
        // a crash.
        const auto digits = match[1].str();
        if (digits.size() > 9) continue;
        const int index = std::stoi(digits);
        presets.push_back({{"index", index}, {"name", ""}, {"platform", ""},
                           {"runnable", false}, {"export_filter", ""}, {"export_path", ""}});
        preset_of_section.emplace(header.name, presets.size() - 1);
        section_names.push_back(header.name);
        canonical_section.push_back(digits == std::to_string(index));
    }

    for (const auto& entry : scanned.entries) {
        // A key before any section at all is a file this cannot make sense of.
        // That is also where a swallowed `[preset.N]` header leaves the keys
        // that were meant to be under it.
        if (entry.section.empty()) {
            note("key_before_section",
                 "Line " + std::to_string(entry.line) + " carries the key \"" + entry.key +
                     "\" before any section header, so nothing owns it. That is also where "
                     "a [preset.N] header swallowed by the line above it leaves its keys.",
                 entry.line);
            continue;
        }
        const auto at = preset_of_section.find(entry.section);
        // [preset.N.options] and any other section are skipped. Their keys are
        // not about a preset, so they are not evidence that the file is broken:
        // a valid ini with no preset sections used to be reported as malformed,
        // which made "this project has no export presets" an error where the
        // same fact with no file at all was a success (#651).
        if (at == preset_of_section.end()) continue;
        const auto& key = entry.key;
        const std::string value = unquote(entry.value_text);
        if (key == "name" || key == "platform" || key == "export_filter" || key == "export_path") {
            presets[at->second][key] = value;
        } else if (key == "runnable") {
            // Anything the parser accepts here, read the way the engine reads
            // it. Comparing against the two words Godot's own writer emits was
            // the right guess about what the file usually holds and the wrong
            // rule for what the engine accepts, and it refused the whole file
            // for a value the engine loads: `runnable=1` reads back as int 1
            // through ConfigFile on 4.5.1 and 4.7.2 and converts to true, and
            // there is nothing left for this branch to refuse, because a value
            // the parser will not start is already ERR_PARSE_ERROR for the
            // whole file and is caught above (#842).
            presets[at->second][key] = config_file::booleanize(entry.value_text);
        }
    }

    std::set<std::string> names;
    for (const auto& preset : presets) {
        const std::string name = preset.value("name", "");
        const std::string platform = preset.value("platform", "");
        const auto index = std::to_string(preset.value("index", 0));
        if (name.empty() || platform.empty()) {
            note("incomplete_preset",
                 "[preset." + index + "] declares no " +
                     (name.empty() ? std::string("name") : std::string("platform")) +
                     ", and Godot needs both to detect a preset. Add it in the editor's "
                     "Export dialog.",
                 0);
            continue;
        }
        if (!names.insert(name).second) {
            note("duplicate_preset_name",
                 "[preset." + index + "] is named \"" + name +
                     "\", and so is an earlier preset. Godot addresses a preset by name, so "
                     "the second one cannot be reached. Rename one in the editor's Export "
                     "dialog.",
                 0);
        }
    }

    // Which of these the engine will detect. EditorExport::load_config reads
    // [preset.0], [preset.1] and so on and stops at the first number that is
    // not there, and it skips a preset whose platform no exporter has without
    // printing anything. Both used to be listed as ordinary presets, and
    // project_export's dry run previewed an export of them that the engine
    // then refused (#921). Measured on 4.5.1, 4.6.2 and 4.7.2 with
    // tools/vibe/probes/export_preset_engine.py.
    int first_missing_index = 0;
    if (!malformed) {
        std::set<int> read_numbers;
        for (size_t i = 0; i < presets.size(); ++i) {
            if (canonical_section[i]) read_numbers.insert(presets[i]["index"].get<int>());
        }
        int first_missing = 0;
        while (read_numbers.count(first_missing) != 0) ++first_missing;
        first_missing_index = first_missing;
        const std::string missing_section = "[preset." + std::to_string(first_missing) + "]";

        for (size_t i = 0; i < presets.size(); ++i) {
            auto& preset = presets[i];
            const int index = preset["index"].get<int>();
            const std::string platform = preset.value("platform", "");
            json not_detected;
            if (!canonical_section[i]) {
                const auto spelled = "[preset." + std::to_string(index) + "]";
                not_detected = {
                    {"reason", "section_not_read"},
                    {"section", "[" + section_names[i] + "]"},
                    {"detail", "Godot asks for the section " + spelled +
                                   ", spelled with no leading zero, so it never reads [" +
                                   section_names[i] + "]. Rename it " + spelled +
                                   ", and its .options section with it."}};
            } else if (index > first_missing) {
                // A preset whose platform is unknown still counts: the engine
                // skips it and goes on to the next number.
                not_detected = {
                    {"reason", "numbering_gap"},
                    {"missing_index", first_missing},
                    {"detail", "Godot reads [preset.0], [preset.1] and so on and stops at the "
                               "first number that is missing, which is " + missing_section +
                                   ", so it never reaches this preset. Renumber the sections so "
                                   "they run from 0 with no gap, and the .options sections with "
                                   "them. The Export dialog cannot do it, because it only shows "
                                   "the presets Godot detected."}};
            } else if (platform == "Linux/X11" ||
                       std::find(shippedExportPlatforms().begin(), shippedExportPlatforms().end(),
                                 platform) != shippedExportPlatforms().end()) {
                // Detected. Linux/X11 is the name before 4.3, which the engine
                // still reads as Linux.
            } else if (const auto meant = shippedPlatformFor(platform)) {
                not_detected = {
                    {"reason", "misspelled_platform"},
                    {"did_you_mean", *meant},
                    {"detail", "Godot has no export platform named \"" + platform +
                                   "\", and it matches the name exactly, so the editor skips this "
                                   "preset without a word. The platform it ships is \"" + *meant +
                                   "\"."}};
            } else {
                not_detected = {
                    {"reason", "platform_not_shipped"},
                    {"detail", "Godot ships no export platform named \"" + platform +
                                   "\". The editor skips this preset without a word unless an "
                                   "editor plugin or a GDExtension registers a platform by exactly "
                                   "that name, which is why project_export still hands it to "
                                   "Godot."}};
            }
            preset["detected"] = not_detected.is_null();
            if (!not_detected.is_null()) preset["not_detected"] = std::move(not_detected);
        }
    }

    ExportPresetsFile file;
    file.section_count = presets.size();
    file.malformed = malformed;
    file.reason = std::move(reason);
    file.detail = std::move(detail);
    file.line = line;
    file.first_missing_index = first_missing_index;
    for (const auto& header : scanned.headers) file.section_names.push_back(header.name);
    if (!malformed) file.presets = std::move(presets);
    return file;
}

std::vector<json> parseExportPresets(const std::string& contents) {
    return readExportPresets(contents).presets;
}


// One code, one opening, and the cause carried with it.
//
// The remedy is not the same for all six. A file that ends inside a value or
// holds a value the parser will not start is a broken write, and "fix it in the
// Export dialog" is not a remedy for it: the dialog will not open a file that
// does not parse. A duplicate name is two presets somebody made in the editor,
// and the dialog is exactly where that one is fixed. So each cause carries its
// own sentence and its own next step (#828).
std::string malformedPresetsMessage(const ExportPresetsFile& file) {
    std::string message = "export_presets.cfg is there and could not be parsed.";
    if (!file.detail.empty()) message += " " + file.detail;
    else message += " Fix it in the editor's Export dialog, or delete it to start again.";
    return message;
}

json malformedPresetsData(const ExportPresetsFile& file) {
    json data = {{"presets_file_exists", true},
                 {"declared_preset_sections", file.section_count}};
    // `reason` is the stable token a caller branches on; the sentence is for
    // the reader. A finding with no line does not publish `line: 0`, which
    // would be a line nobody can open.
    if (!file.reason.empty()) data["reason"] = file.reason;
    if (file.line > 0) data["line"] = file.line;
    return data;
}

const std::vector<std::string>& shippedExportPlatforms() {
    static const std::vector<std::string> platforms = {
        "Windows Desktop", "Linux", "macOS", "Android", "iOS", "Web", "visionOS"};
    return platforms;
}

std::optional<std::string> shippedPlatformFor(const std::string& written) {
    if (written == "Linux/X11") return std::nullopt;
    std::string folded = strings::trim(written);
    std::transform(folded.begin(), folded.end(), folded.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    for (const auto& name : shippedExportPlatforms()) {
        std::string candidate = name;
        std::transform(candidate.begin(), candidate.end(), candidate.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (candidate == folded && name != written) return name;
    }
    // The names a file written by hand or by an older engine uses for a
    // platform Godot 4 ships under another name. Each one is a preset the
    // editor skips on all three supported lines.
    static const std::map<std::string, std::string> other_names = {
        {"windows", "Windows Desktop"}, {"html5", "Web"},   {"linux/x11", "Linux"},
        {"x11", "Linux"},               {"osx", "macOS"},   {"mac osx", "macOS"},
    };
    const auto found = other_names.find(folded);
    if (found != other_names.end()) return found->second;
    return std::nullopt;
}

std::vector<std::string> detectedPresetsInEngineOutput(const std::string& output) {
    // ERROR: Invalid export preset name: Stranded.
    // The following presets were detected in this project's `export_presets.cfg`:
    //
    //         "Other"
    //
    //    at: _fs_changed (editor/editor_node.cpp:1417)
    std::vector<std::string> names;
    std::istringstream lines(output);
    std::string line;
    bool listing = false;
    while (std::getline(lines, line)) {
        const auto text = strings::trim(line);
        if (!listing) {
            listing = text.find("Invalid export preset name") != std::string::npos;
            continue;
        }
        if (text.rfind("at:", 0) == 0) break;
        if (text.size() >= 2 && text.front() == '"' && text.back() == '"') {
            names.push_back(text.substr(1, text.size() - 2));
        }
    }
    return names;
}

std::optional<std::string> invalidPresetNameInEngineOutput(const std::string& output) {
    static const std::string marker = "Invalid export preset name: ";
    std::istringstream lines(output);
    std::string line;
    while (std::getline(lines, line)) {
        const auto at = line.find(marker);
        if (at == std::string::npos) continue;
        auto name = strings::trim(line.substr(at + marker.size()));
        // Godot ends the sentence with a full stop after the name.
        if (!name.empty() && name.back() == '.') name.pop_back();
        return name;
    }
    return std::nullopt;
}

std::string presetNameForCommandLine(const std::string& name) {
    return strings::replaceAll(name, " ", "%20");
}

std::optional<std::string> presetNameCommandLineProblem(const std::string& name) {
    if (name.find("%20") != std::string::npos) {
        return "Godot turns %20 on its command line into a space, and nothing there escapes a "
               "percent sign, so it would look for a different name";
    }
    return std::nullopt;
}

ExportConfigurationErrors exportConfigurationErrors(const std::string& output) {
    // ERROR: Cannot export project with preset "Desk" due to configuration errors:
    // No export template found at the expected path:
    // C:/Users/.../export_templates/4.6.2.stable/windows_debug_x86_64.exe
    // No export template found at the expected path:
    // C:/Users/.../export_templates/4.6.2.stable/windows_release_x86_64.exe
    //
    //    at: _fs_changed (editor/editor_node.cpp:1332)
    //
    // The same block on 4.5.1, 4.6.2 and 4.7.2. Android adds its SDK checks to
    // it, one line each.
    static const std::string template_sentence = "No export template found at the expected path:";
    ExportConfigurationErrors found;
    std::istringstream lines(output);
    std::string line;
    bool listing = false;
    bool path_next = false;
    while (std::getline(lines, line)) {
        const auto text = strings::trim(line);
        if (!listing) {
            listing = text.find("due to configuration errors:") != std::string::npos;
            continue;
        }
        if (text.empty() || text.rfind("at:", 0) == 0) break;
        if (path_next) {
            path_next = false;
            found.missing_templates.push_back(text);
            found.errors.back() += " " + text;
            continue;
        }
        found.errors.push_back(text);
        path_next = text == template_sentence;
    }
    return found;
}

Result<json> findExportPreset(const std::string& preset) {
    std::error_code error;
    const auto root = std::filesystem::current_path(error);
    if (error) return Error::internal("The project root could not be resolved");
    const auto path = root / "export_presets.cfg";
    if (!std::filesystem::exists(path, error) || error) {
        // project_add_export_preset is the route through the surface (#779).
        return Error(404,
                     "This project has no export_presets.cfg, so it has no export presets. Add one "
                     "with project_add_export_preset, or in the editor's Export dialog.",
                     {{"preset", preset}, {"presets_file_exists", false},
                      {"use_tool", "project_add_export_preset"}});
    }
    // The same bound the Phase 5 readers apply, so this cannot pull in a file
    // they would have refused.
    constexpr uintmax_t kMaxPresetFileBytes = 1024u * 1024u;
    const auto size = std::filesystem::file_size(path, error);
    if (error) return Error(403, "export_presets.cfg is there and cannot be read");
    if (size > kMaxPresetFileBytes) {
        return Error::invalidArgument("export_presets.cfg exceeds the 1 MiB Phase 5 limit");
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return Error(403, "export_presets.cfg is there and cannot be read");
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    const auto file = readExportPresets(buffer.str());
    if (file.malformed) return Error(422, malformedPresetsMessage(file), malformedPresetsData(file));

    // A preset Godot can never detect is refused here rather than handed to
    // an export that ends in "Invalid export preset name" (#921). A platform
    // Godot does not ship is the exception: an editor plugin or a GDExtension
    // can register one, and only the engine knows whether it did.
    const auto accepted = [](const json& item) {
        if (item.value("detected", true)) return true;
        return item["not_detected"].value("reason", "") == "platform_not_shipped";
    };
    json available = json::array();
    json detected = json::array();
    for (const auto& item : file.presets) {
        if (accepted(item)) available.push_back(item.value("name", ""));
        if (item.value("detected", true)) detected.push_back(item.value("name", ""));
    }
    const auto match = std::find_if(file.presets.begin(), file.presets.end(),
                                    [&](const json& item) {
                                        return item.value("name", "") == preset;
                                    });
    if (match == file.presets.end()) {
        return Error(404,
                     "Export preset not found: " + preset +
                         ". project_add_export_preset adds one under this name.",
                     {{"preset", preset}, {"available_presets", std::move(available)},
                      {"use_tool", "project_add_export_preset"}});
    }
    if (!accepted(*match)) {
        const auto& why = (*match)["not_detected"];
        json data = {{"preset", preset},
                     {"platform", match->value("platform", "")},
                     {"reason", why.value("reason", "")},
                     {"detected_presets", std::move(detected)}};
        for (const char* key : {"missing_index", "did_you_mean", "section"}) {
            if (why.contains(key)) data[key] = why[key];
        }
        return Error(422,
                     "Godot will not detect the export preset \"" + preset + "\". " +
                         why.value("detail", ""),
                     std::move(data));
    }
    // Detected, and still not a name the export can ask for. Refused before
    // Godot starts, like the causes above, because what Godot answers to it is
    // a different preset or none.
    if (const auto problem = presetNameCommandLineProblem(preset)) {
        return Error(422,
                     "Godot cannot be asked for the export preset \"" + preset +
                         "\" on its command line, which is how project_export runs it: " +
                         *problem + ". Rename the preset in the editor's Export dialog.",
                     {{"preset", preset},
                      {"reason", "name_not_passable"},
                      {"detected_presets", std::move(detected)}});
    }
    return *match;
}

namespace {

// Characters, the unit the published schema's maxLength counts in. A bound in
// bytes refused 86 CJK characters under a schema that allowed 256 (#663's
// class, found again by vibe session nineteen).
constexpr size_t kMaxPresetNameCharacters = 256;

// A control character in a value is either a line break, which the tool
// refuses because it would put a second line in a one-line key, or something
// no preset name or path has a reason to hold.
bool hasControlCharacter(const std::string& text) {
    return std::any_of(text.begin(), text.end(), [](char c) {
        const auto byte = static_cast<unsigned char>(c);
        return byte < 0x20 || byte == 0x7f;
    });
}

// The two escapes Godot's string writer applies to a value with no control
// characters in it, which is every value this writes.
std::string configFileString(const std::string& text) {
    std::string out = "\"";
    for (const char c : text) {
        if (c == '\\') out += "\\\\";
        else if (c == '"') out += "\\\"";
        else out += c;
    }
    return out + "\"";
}

Error invalidPresetArgument(const std::string& parameter, std::string message, json extra = {}) {
    json data = {{"code", "invalid_arguments"}, {"parameter", parameter}, {"retryable", false}};
    if (extra.is_object()) data.update(extra);
    return Error(400, std::move(message), std::move(data));
}

std::optional<Error> platformRefusal(const std::string& platform) {
    const auto& platforms = shippedExportPlatforms();
    if (std::find(platforms.begin(), platforms.end(), platform) != platforms.end()) {
        return std::nullopt;
    }
    // Linux/X11 is a name the engine still reads, and not one it writes.
    const auto meant = platform == "Linux/X11" ? std::optional<std::string>("Linux")
                                               : shippedPlatformFor(platform);
    json extra = {{"platforms", platforms}};
    std::string message = "platform must be one of the seven export platforms Godot ships, "
                          "spelled exactly: " + [&] {
                              std::string joined;
                              for (const auto& item : platforms) {
                                  joined += (joined.empty() ? "" : ", ") + item;
                              }
                              return joined;
                          }() + ".";
    if (meant) {
        extra["did_you_mean"] = *meant;
        extra["retry_with"] = {{"platform", *meant}};
        message += " \"" + platform + "\" is \"" + *meant + "\" to Godot.";
    }
    return invalidPresetArgument("platform", std::move(message), std::move(extra));
}

} // namespace

std::optional<Error> exportPlatformRefusal(const json& args) {
    if (!args.is_object() || !args.contains("platform") || !args["platform"].is_string()) {
        return std::nullopt;
    }
    return platformRefusal(args["platform"].get<std::string>());
}

Result<ExportPresetAddition> planExportPresetAddition(const std::optional<std::string>& existing,
                                                      const std::string& name,
                                                      const std::string& platform,
                                                      const std::string& export_path) {
    if (name.empty()) return invalidPresetArgument("name", "name must not be empty");
    if (paths::codePointCount(name) > kMaxPresetNameCharacters) {
        return invalidPresetArgument("name", "name must be at most " +
                                                 std::to_string(kMaxPresetNameCharacters) +
                                                 " characters");
    }
    if (hasControlCharacter(name)) {
        return invalidPresetArgument(
            "name", "name must not contain a line break or another control character, because "
                    "export_presets.cfg keeps it on one line");
    }
    // project_export hands the name to Godot on its command line, and these
    // are the names that do not arrive as written. Godot takes an argument
    // that starts with - as one of its own options when it has one by that
    // name: --headless, -e and --verbose all were, and the export then asked
    // for a preset named after its output path.
    if (const auto problem = presetNameCommandLineProblem(name)) {
        return invalidPresetArgument(
            "name", "name must not contain %20, because project_export asks Godot for the preset "
                    "by name on its command line: " + *problem + ".");
    }
    if (name.front() == '-') {
        return invalidPresetArgument(
            "name", "name must not start with -, because project_export asks Godot for the "
                    "preset by name on its command line, and Godot reads an argument that starts "
                    "with - as one of its own options when it has one by that name, such as "
                    "--headless, -e or --verbose.");
    }
    if (auto refused = platformRefusal(platform)) return *refused;
    if (hasControlCharacter(export_path)) {
        return invalidPresetArgument("export_path",
                                     "export_path must not contain a control character");
    }

    ExportPresetAddition plan;
    plan.file_created = !existing.has_value();
    const std::string before = existing.value_or("");
    if (existing.has_value()) {
        const auto file = readExportPresets(before);
        if (file.malformed) {
            // Appending to a file the engine answers ERR_PARSE_ERROR for would
            // still leave a project with no presets.
            auto data = malformedPresetsData(file);
            data["code"] = "unprocessable";
            return Error(422, malformedPresetsMessage(file), std::move(data));
        }
        plan.presets_before = file.presets.size();
        plan.index = file.first_missing_index;

        json stranded = json::array();
        for (const auto& preset : file.presets) {
            const auto& why = preset.value("not_detected", json::object());
            if (why.value("reason", "") == "numbering_gap") stranded.push_back(preset["name"]);
        }
        if (!stranded.empty()) {
            // Filling the gap would also bring back every preset stranded
            // behind it, and appending after them would strand this one too.
            return Error(
                409,
                "export_presets.cfg has no [preset." + std::to_string(plan.index) +
                    "], and Godot stops reading at the first number that is missing, so it "
                    "never reads the presets after it. A preset added here would either be "
                    "stranded with them or bring them all back, and this call adds one preset. "
                    "Renumber the sections so they run from 0 with no gap, and the .options "
                    "sections with them, then add it.",
                {{"code", "conflict"},
                 {"reason", "numbering_gap"},
                 {"missing_index", plan.index},
                 {"stranded_presets", std::move(stranded)},
                 {"retryable", false}});
        }
        for (const auto& preset : file.presets) {
            if (preset.value("name", "") != name) continue;
            // The engine exports the first preset with a name, so a second
            // one could never be reached. Refused rather than replaced: this
            // tool only adds.
            return Error(409, "export_presets.cfg already has a preset named \"" + name + "\"",
                         {{"code", "already_exists"},
                          {"preset", name},
                          {"existing", {{"index", preset.value("index", 0)},
                                        {"platform", preset.value("platform", "")},
                                        {"detected", preset.value("detected", true)}}},
                          {"retryable", false}});
        }
        const auto options = "preset." + std::to_string(plan.index) + ".options";
        if (std::find(file.section_names.begin(), file.section_names.end(), options) !=
            file.section_names.end()) {
            // Godot merges a section declared twice, so a leftover options
            // section would become this preset's options.
            return Error(409,
                         "export_presets.cfg already has a [" + options +
                             "] section with no preset above it, and Godot would read its "
                             "options as the new preset's. Remove that section first.",
                         {{"code", "conflict"},
                          {"reason", "orphan_options_section"},
                          {"section", "[" + options + "]"},
                          {"retryable", false}});
        }
    }

    // The fewest keys every supported engine loads with no ERROR line. Each
    // one is in the amendment's table with the reason it is there.
    const std::string newline = before.find("\r\n") != std::string::npos ? "\r\n" : "\n";
    const auto index = std::to_string(plan.index);
    const std::vector<std::string> lines = {
        "[preset." + index + "]",
        "",
        "name=" + configFileString(name),
        "platform=" + configFileString(platform),
        "runnable=false",
        "export_filter=\"all_resources\"",
        "include_filter=\"\"",
        "exclude_filter=\"\"",
        "export_path=" + configFileString(export_path),
        "",
        "[preset." + index + ".options]",
        "",
        "custom_template/debug=\"\"",
    };
    for (const auto& line : lines) plan.section_text += line + newline;

    // Kept byte for byte. The section starts on a line of its own, after a
    // blank one, whatever the file ended with.
    std::string separator;
    if (!before.empty()) {
        if (before.back() != '\n') separator += newline;
        const bool blank_last = before.size() >= 2 &&
                                (strings::endsWith(before, "\n\n") ||
                                 strings::endsWith(before, "\n\r\n"));
        if (!blank_last) separator += newline;
    }
    plan.contents = before + separator + plan.section_text;
    return plan;
}

Result<ExportPresetAddition> planExportPresetForProject(const json& args) {
    if (!args.is_object() || !args.contains("name") || !args["name"].is_string()) {
        return invalidPresetArgument("name", "name is a required string");
    }
    if (!args.contains("platform") || !args["platform"].is_string()) {
        return invalidPresetArgument("platform", "platform is a required string");
    }
    if (args.contains("export_path") && !args["export_path"].is_string()) {
        return invalidPresetArgument("export_path", "export_path must be a string");
    }

    // Stored the way the Export dialog stores a path inside the project:
    // relative to it, with no res:// in front.
    std::string export_path;
    const std::string requested = args.value("export_path", "");
    if (!requested.empty()) {
        // A directory that does not exist yet is still a directory. "builds/"
        // was stored as it was sent, and the Export dialog then offered to
        // write a file with no name.
        if (requested.back() == '/' || requested.back() == '\\') {
            return invalidPresetArgument(
                "export_path", "export_path ends with a separator, so it names a directory, and "
                               "an export writes a file. Add the file name, such as "
                               "builds/game.exe.");
        }
        auto resolved = paths::resolveProjectFileForWrite(requested);
        if (resolved.isErr()) {
            return invalidPresetArgument("export_path", "export_path " + resolved.error().message);
        }
        std::error_code error;
        if (std::filesystem::is_directory(resolved.value(), error)) {
            return invalidPresetArgument(
                "export_path", "export_path names a directory, and an export writes a file");
        }
        export_path = paths::resourcePathOf(resolved.value());
        if (strings::startsWith(export_path, "res://")) export_path.erase(0, 6);
    }

    std::error_code error;
    const auto root = std::filesystem::current_path(error);
    if (error) return Error::internal("The project root could not be resolved");
    const auto path = root / "export_presets.cfg";
    std::optional<std::string> existing;
    if (std::filesystem::exists(path, error) && !error) {
        constexpr uintmax_t kMaxPresetFileBytes = 1024u * 1024u;
        const auto size = std::filesystem::file_size(path, error);
        if (error) return Error(403, "export_presets.cfg is there and cannot be read");
        if (size > kMaxPresetFileBytes) {
            return Error::invalidArgument("export_presets.cfg exceeds the 1 MiB Phase 5 limit");
        }
        std::ifstream input(path, std::ios::binary);
        if (!input) return Error(403, "export_presets.cfg is there and cannot be read");
        std::ostringstream buffer;
        buffer << input.rdbuf();
        existing = buffer.str();
    }
    return planExportPresetAddition(existing, args["name"].get<std::string>(),
                                    args["platform"].get<std::string>(), export_path);
}

} // namespace didi::offline
