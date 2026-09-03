#include "didi/offline/project_impact.hpp"

#include "didi/common/project_path.hpp"
#include "didi/offline/project_text_scan.hpp"

#include <algorithm>
#include <cctype>
#include <functional>
#include <fstream>
#include <map>
#include <regex>
#include <sstream>
#include <tuple>

namespace didi::offline {
namespace {

constexpr size_t kMaxTargetBytes = 256;
constexpr size_t kMaxDetailBytes = 200;

struct Impact {
    std::string path;
    std::string kind;
    int line{0};
    std::string detail;
};

// The matched line, trimmed and bounded. A finding a caller cannot see the
// evidence for is a finding they have to go and reproduce by hand.
std::string detailFrom(const std::string& line) {
    auto detail = strings::trim(line);
    if (detail.size() > kMaxDetailBytes) {
        detail.resize(kMaxDetailBytes);
        detail += "...";
    }
    return detail;
}

bool looksLikeFileTarget(const std::string& target) {
    return strings::startsWith(target, "res://") || strings::startsWith(target, "uid://");
}

bool isValidFileTarget(const std::string& target) {
    if (strings::startsWith(target, "uid://")) {
        static const std::regex uid(R"re(^uid://[a-z0-9]+$)re");
        return std::regex_match(target, uid);
    }
    if (!strings::startsWith(target, "res://") || target.size() == 6 || target.back() == '/') {
        return false;
    }
    const auto relative = target.substr(6);
    size_t start = 0;
    while (start < relative.size()) {
        const auto slash = relative.find('/', start);
        const auto segment = relative.substr(
            start, slash == std::string::npos ? std::string::npos : slash - start);
        if (segment.empty() || segment == "." || segment == "..") return false;
        for (const unsigned char character : segment) {
            if (std::iscntrl(character) || character == '"' || character == '\\' ||
                character == ':') {
                return false;
            }
        }
        if (slash == std::string::npos) return true;
        start = slash + 1;
    }
    return false;
}

bool isNodeNameCharacter(unsigned char character) {
    return character >= 0x80 ||
           (character >= 0x20 && character != '.' && character != ':' &&
            character != '/' && character != '"' && character != '%' && character != '\\');
}

bool isUnquotedShorthandCharacter(unsigned char character) {
    return character >= 0x80 ||
           (character >= 'A' && character <= 'Z') ||
           (character >= 'a' && character <= 'z') ||
           (character >= '0' && character <= '9') || character == '_' ||
           character == '-' || character == '@';
}

bool hasValidNodePathSegments(const std::string& path) {
    size_t start = path.front() == '/' ? 1 : 0;
    while (start < path.size()) {
        const auto slash = path.find('/', start);
        auto segment = path.substr(start, slash == std::string::npos ? std::string::npos
                                                                     : slash - start);
        if (segment.empty()) return false;
        if (segment != "." && segment != "..") {
            if (!segment.empty() && segment.front() == '%') {
                if (segment.size() == 1) return false;
                segment.erase(0, 1);
            }
            if (segment.empty() ||
                !std::all_of(segment.begin(), segment.end(), [](unsigned char character) {
                    return isNodeNameCharacter(character);
                })) {
                return false;
            }
        }
        if (slash == std::string::npos) return true;
        start = slash + 1;
    }
    return false;
}

bool looksLikeNodePath(const std::string& target) {
    auto path = target;
    const bool dollar_shorthand = !path.empty() && path.front() == '$';
    if (dollar_shorthand) path.erase(0, 1);
    if (path.empty()) return false;
    const auto subname = path.find(':');
    const auto node_part = path.substr(0, subname);
    if (node_part.empty() || node_part.back() == '/' || !hasValidNodePathSegments(node_part)) {
        return false;
    }
    if (subname != std::string::npos) {
        const auto subnames = path.substr(subname + 1);
        if (subnames.empty() || subnames.find('/') != std::string::npos) return false;
        size_t start = 0;
        while (start < subnames.size()) {
            const auto colon = subnames.find(':', start);
            const auto item = subnames.substr(
                start, colon == std::string::npos ? std::string::npos : colon - start);
            if (item.empty()) return false;
            for (const unsigned char character : item) {
                if (std::iscntrl(character) || character == '"' || character == '\\') return false;
            }
            if (colon == std::string::npos) break;
            start = colon + 1;
        }
    }
    if (node_part.front() == '/') return !dollar_shorthand;
    if (dollar_shorthand || strings::startsWith(path, "%")) return true;
    return node_part == "." || node_part == ".." || strings::startsWith(node_part, "./") ||
           strings::startsWith(node_part, "../") || node_part.find('/') != std::string::npos ||
           subname != std::string::npos;
}

// Godot identifiers, which is what a symbol or signal target has to be. A
// target that is neither a path nor an identifier is a question this cannot
// answer, and saying so beats returning an empty report.
bool looksLikeIdentifier(const std::string& target) {
    static const std::regex identifier(R"re(^[A-Za-z_][A-Za-z0-9_]*$)re");
    return std::regex_match(target, identifier);
}

void forEachLine(const std::string& text,
                 const std::function<void(const std::string&, int)>& visit) {
    std::istringstream lines(text);
    std::string line;
    int number = 0;
    while (std::getline(lines, line)) {
        ++number;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        visit(line, number);
    }
}

std::string readFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    std::ostringstream contents;
    contents << input.rdbuf();
    return contents.str();
}

// project.godot is not a resource the indexer returns, so autoloads have to be
// read directly. An autoload is the one reference that can make a script
// project-wide, which is exactly the kind a rename must not miss.
void collectAutoloadImpacts(const std::filesystem::path& root, const std::string& target,
                            bool file_target, std::vector<Impact>& out) {
    const auto text = readFile(root / "project.godot");
    if (text.empty()) return;

    bool in_autoload = false;
    forEachLine(text, [&](const std::string& line, int number) {
        const auto trimmed = strings::trim(line);
        if (!trimmed.empty() && trimmed.front() == '[') {
            in_autoload = trimmed == "[autoload]";
            return;
        }
        if (!in_autoload || trimmed.empty()) return;
        const bool names_target =
            file_target ? trimmed.find(target) != std::string::npos
                        : trimmed.find(target + "=") == 0;
        if (names_target) out.push_back({"res://project.godot", "autoload", number, detailFrom(line)});
    });
}

void collectFileTargetImpacts(const ProjectTextScan& scan, const std::string& target,
                              std::vector<Impact>& out) {
    // A plain substring is right here: the target is already a full res:// or
    // uid:// string, so it cannot match a longer path by accident the way a
    // bare name could match a longer name.
    for (const auto& source : scan.sources) {
        if (source.path == target) continue;
        forEachLine(source.contents, [&](const std::string& line, int number) {
            if (line.find(target) == std::string::npos) return;
            const auto trimmed = strings::trim(line);
            if (strings::startsWith(trimmed, "[ext_resource")) {
                out.push_back({source.path, "ext_resource", number, detailFrom(line)});
                return;
            }
            if (trimmed.find("script = ") != std::string::npos ||
                trimmed.find("script=") != std::string::npos) {
                out.push_back({source.path, "script_attachment", number, detailFrom(line)});
                return;
            }
            if (trimmed.find("preload(") != std::string::npos ||
                trimmed.find("load(") != std::string::npos ||
                trimmed.find("Load(") != std::string::npos ||
                trimmed.find("Load<") != std::string::npos) {
                out.push_back({source.path, "script_load", number, detailFrom(line)});
                return;
            }
            out.push_back({source.path, "code_reference", number, detailFrom(line)});
        });
    }
}

void collectNameTargetImpacts(const ProjectTextScan& scan, const std::string& target,
                              std::vector<Impact>& out) {
    // Whole word, so renaming `health` does not report every `max_health`.
    const std::regex whole_word(R"re(\b)re" + target + R"re(\b)re");
    const std::regex connection_signal(R"re(\[connection[^\]]*signal=")re" + target + R"re(")re");
    const std::regex connection_method(R"re(\[connection[^\]]*method=")re" + target + R"re(")re");
    // A track keyframes a property through a NodePath whose last segment, after
    // a colon, is the property name. This is the reference a text search for
    // the symbol will find but not explain, and the one people forget.
    const std::regex animation_track(R"re(NodePath\("[^"]*:)re" + target +
                                     R"re((?::[A-Za-z0-9_]+)?"\))re");

    for (const auto& source : scan.sources) {
        if (source.contents.find(target) == std::string::npos) continue;
        forEachLine(source.contents, [&](const std::string& line, int number) {
            if (!std::regex_search(line, whole_word)) return;
            if (std::regex_search(line, connection_signal)) {
                out.push_back({source.path, "scene_connection", number, detailFrom(line)});
                return;
            }
            if (std::regex_search(line, connection_method)) {
                out.push_back({source.path, "scene_connection", number, detailFrom(line)});
                return;
            }
            if (std::regex_search(line, animation_track)) {
                out.push_back({source.path, "animation_track", number, detailFrom(line)});
                return;
            }
            out.push_back({source.path, "code_reference", number, detailFrom(line)});
        });
    }
}

std::string normalizedNodePath(std::string path) {
    if (!path.empty() && path.front() == '$') path.erase(0, 1);
    return path;
}

bool nodePathMatches(const std::string& candidate, const std::string& target,
                     bool strip_subnames) {
    auto normalized = normalizedNodePath(candidate);
    const auto normalized_target = normalizedNodePath(target);
    if (strip_subnames && normalized_target.find(':') == std::string::npos) {
        const auto subname = normalized.find(':');
        if (subname != std::string::npos) normalized.resize(subname);
    }
    return normalized == normalized_target;
}

std::string maskGdscriptNonCode(const std::string& text) {
    enum class State { Code, Comment, SingleQuote, DoubleQuote, TripleSingle, TripleDouble };
    auto masked = text;
    State state = State::Code;
    bool escaped = false;
    for (size_t index = 0; index < masked.size(); ++index) {
        const char character = masked[index];
        if (state == State::Code) {
            if (character == '#') {
                state = State::Comment;
                masked[index] = ' ';
            } else if (character == '"' || character == '\'') {
                const bool triple = index + 2 < masked.size() &&
                                    masked[index + 1] == character &&
                                    masked[index + 2] == character;
                state = triple ? (character == '"' ? State::TripleDouble : State::TripleSingle)
                               : (character == '"' ? State::DoubleQuote : State::SingleQuote);
                masked[index] = ' ';
                if (triple) {
                    masked[index + 1] = ' ';
                    masked[index + 2] = ' ';
                    index += 2;
                }
            }
            continue;
        }
        if (state == State::Comment) {
            if (character == '\n') {
                state = State::Code;
            } else if (character != '\r') {
                masked[index] = ' ';
            }
            continue;
        }

        const bool triple = state == State::TripleSingle || state == State::TripleDouble;
        const char quote = (state == State::SingleQuote || state == State::TripleSingle) ? '\'' : '"';
        if (character != '\n' && character != '\r') masked[index] = ' ';
        if (triple) {
            if (character == quote && index + 2 < masked.size() &&
                masked[index + 1] == quote && masked[index + 2] == quote) {
                masked[index + 1] = ' ';
                masked[index + 2] = ' ';
                index += 2;
                state = State::Code;
            }
        } else if (escaped) {
            escaped = false;
        } else if (character == '\\') {
            escaped = true;
        } else if (character == quote || character == '\n') {
            state = State::Code;
        }
    }
    return masked;
}

std::string maskGodotResourceComments(const std::string& text) {
    auto masked = text;
    bool quoted = false;
    bool escaped = false;
    bool comment = false;
    for (size_t index = 0; index < masked.size(); ++index) {
        const char character = masked[index];
        if (comment) {
            if (character == '\n') {
                comment = false;
            } else if (character != '\r') {
                masked[index] = ' ';
            }
        } else if (quoted) {
            if (escaped) {
                escaped = false;
            } else if (character == '\\') {
                escaped = true;
            } else if (character == '"') {
                quoted = false;
            }
        } else if (character == '"') {
            quoted = true;
        } else if (character == ';') {
            comment = true;
            masked[index] = ' ';
        }
    }
    return masked;
}

std::string maskCSharpNonCode(const std::string& text) {
    enum class State { Code, LineComment, BlockComment, String, VerbatimString, Character };
    auto masked = text;
    State state = State::Code;
    bool escaped = false;
    for (size_t index = 0; index < masked.size(); ++index) {
        const char character = masked[index];
        const char next = index + 1 < masked.size() ? masked[index + 1] : '\0';
        if (state == State::Code) {
            if (character == '/' && next == '/') {
                state = State::LineComment;
                masked[index] = masked[index + 1] = ' ';
                ++index;
            } else if (character == '/' && next == '*') {
                state = State::BlockComment;
                masked[index] = masked[index + 1] = ' ';
                ++index;
            } else if (character == '@' && next == '"') {
                state = State::VerbatimString;
                masked[index] = masked[index + 1] = ' ';
                ++index;
            } else if (character == '"' || character == '\'') {
                state = character == '"' ? State::String : State::Character;
                masked[index] = ' ';
            }
            continue;
        }
        if (state == State::LineComment) {
            if (character == '\n') state = State::Code;
            else if (character != '\r') masked[index] = ' ';
            continue;
        }
        if (state == State::BlockComment) {
            if (character != '\n' && character != '\r') masked[index] = ' ';
            if (character == '*' && next == '/') {
                masked[index + 1] = ' ';
                ++index;
                state = State::Code;
            }
            continue;
        }
        if (character != '\n' && character != '\r') masked[index] = ' ';
        if (state == State::VerbatimString) {
            if (character == '"' && next == '"') {
                masked[index + 1] = ' ';
                ++index;
            } else if (character == '"') {
                state = State::Code;
            }
        } else if (escaped) {
            escaped = false;
        } else if (character == '\\') {
            escaped = true;
        } else if ((state == State::String && character == '"') ||
                   (state == State::Character && character == '\'')) {
            state = State::Code;
        }
    }
    return masked;
}

bool hasDirectShorthandReference(const std::string& code, const std::string& target) {
    const auto normalized = normalizedNodePath(target);
    const auto needle = strings::startsWith(normalized, "%") ? normalized : "$" + normalized;
    size_t position = 0;
    while ((position = code.find(needle, position)) != std::string::npos) {
        const auto end = position + needle.size();
        const bool left_boundary =
            position == 0 || needle.front() == '$' ||
            (!isUnquotedShorthandCharacter(static_cast<unsigned char>(code[position - 1])) &&
             code[position - 1] != '/');
        const bool right_boundary = end == code.size() ||
                                    (!isUnquotedShorthandCharacter(
                                         static_cast<unsigned char>(code[end])) &&
                                     code[end] != '/' && code[end] != '%');
        if (left_boundary && right_boundary) return true;
        position = end;
    }
    return false;
}

void collectNodePathImpacts(const ProjectTextScan& scan, const std::string& target,
                            std::vector<Impact>& out) {
    static const std::regex connection_endpoint(R"re(\b(?:from|to)="([^"]+)")re");
    static const std::regex node_path_value(R"re(NodePath\("([^"]+)"\))re");
    static const std::regex get_node_call(
        R"re(\bget_node(?:_or_null)?\s*\(\s*(?:\^)?["']([^"']+)["']\s*\))re");
    static const std::regex node_path_literal(R"re(\^["']([^"']+)["'])re");
    const auto normalized_target = normalizedNodePath(target);

    for (const auto& source : scan.sources) {
        const bool gdscript = strings::endsWith(source.path, ".gd");
        const bool csharp = strings::endsWith(source.path, ".cs");
        const bool godot_resource = strings::endsWith(source.path, ".tscn") ||
                                    strings::endsWith(source.path, ".tres");
        std::vector<std::string> syntax_lines;
        if (gdscript || csharp || godot_resource) {
            const auto masked = gdscript ? maskGdscriptNonCode(source.contents)
                              : csharp ? maskCSharpNonCode(source.contents)
                                       : maskGodotResourceComments(source.contents);
            forEachLine(masked,
                        [&](const std::string& line, int) { syntax_lines.push_back(line); });
        }
        size_t line_index = 0;
        forEachLine(source.contents, [&](const std::string& line, int number) {
            const std::string* syntax_line = syntax_lines.empty() ? nullptr
                                                                  : &syntax_lines[line_index];
            ++line_index;
            if (line.find(normalized_target) == std::string::npos) return;
            const auto trimmed = strings::trim(line);
            const auto has_exact_match = [&](const std::string& text, const std::regex& pattern,
                                             bool strip_subnames, size_t capture,
                                             const std::string* code_mask = nullptr,
                                             size_t code_capture = 0) {
                for (auto it = std::sregex_iterator(text.begin(), text.end(), pattern);
                     it != std::sregex_iterator(); ++it) {
                    if (code_mask != nullptr) {
                        const auto position = static_cast<size_t>((*it).position(code_capture));
                        if (position >= code_mask->size() || (*code_mask)[position] == ' ') continue;
                    }
                    if (nodePathMatches((*it)[capture].str(), target, strip_subnames)) return true;
                }
                return false;
            };

            if (godot_resource && strings::startsWith(trimmed, "[connection") &&
                has_exact_match(line, connection_endpoint, false, 1, syntax_line)) {
                out.push_back({source.path, "scene_connection", number, detailFrom(line)});
                return;
            }
            if (godot_resource && strings::startsWith(trimmed, "tracks/") &&
                trimmed.find("/path") != std::string::npos &&
                has_exact_match(line, node_path_value, true, 1, syntax_line)) {
                out.push_back({source.path, "animation_track", number, detailFrom(line)});
                return;
            }
            if (has_exact_match(line, node_path_value, false, 1, syntax_line)) {
                const bool code = strings::endsWith(source.path, ".gd") ||
                                  strings::endsWith(source.path, ".cs");
                out.push_back({source.path, code ? "code_reference" : "node_path_reference",
                               number, detailFrom(line)});
                return;
            }
            if (!gdscript) return;
            if (has_exact_match(line, get_node_call, false, 1, syntax_line) ||
                has_exact_match(line, node_path_literal, false, 1, syntax_line) ||
                hasDirectShorthandReference(*syntax_line, target)) {
                out.push_back({source.path, "code_reference", number, detailFrom(line)});
            }
        });
    }
}

// Where the name is declared, so a caller knows what they are about to rename
// rather than only what would break.
std::vector<Impact> declarationsOf(const ProjectTextScan& scan, const std::string& target) {
    const std::regex declaration(
        R"re(^\s*(?:@\w+(?:\([^)]*\))?\s+)*(signal|func|var|const|class_name|enum|static\s+func)\s+)re" +
        target + R"re(\b)re");
    std::vector<Impact> declarations;
    for (const auto& source : scan.sources) {
        if (source.contents.find(target) == std::string::npos) continue;
        forEachLine(source.contents, [&](const std::string& line, int number) {
            std::smatch match;
            if (!std::regex_search(line, match, declaration)) return;
            auto kind = match[1].str();
            if (strings::startsWith(kind, "static")) kind = "func";
            declarations.push_back({source.path, kind, number, detailFrom(line)});
        });
    }
    return declarations;
}

json impactsToJson(const std::vector<Impact>& impacts, size_t limit, bool& truncated) {
    json array = json::array();
    for (const auto& impact : impacts) {
        if (array.size() >= limit) {
            truncated = true;
            break;
        }
        array.push_back({{"path", impact.path},
                         {"kind", impact.kind},
                         {"line", impact.line},
                         {"detail", impact.detail}});
    }
    return array;
}

} // namespace

Result<json> analyzeImpact(const std::string& root_dir, const ProjectImpactOptions& options) {
    const auto target = strings::trim(options.target);
    if (target.empty()) {
        return Error::invalidArgument("target must name a symbol, a signal, or a res:// path");
    }
    if (target.size() > kMaxTargetBytes) {
        return Error::invalidArgument("target must be at most 256 bytes");
    }
    const bool file_target = looksLikeFileTarget(target);
    if (file_target && !isValidFileTarget(target)) {
        return Error::invalidArgument(
            "resource target must be a canonical res:// path or lowercase-alphanumeric uid:// value");
    }
    const bool node_path_target = !file_target && looksLikeNodePath(target);
    if (!file_target && !node_path_target && !looksLikeIdentifier(target)) {
        return Error::invalidArgument(
            "target must be a res:// or uid:// path, a Godot node path, or a single Godot identifier");
    }

    const auto scan = scanProjectText(root_dir);
    const auto root = paths::projectPathFromUtf8(root_dir);

    std::vector<Impact> impacts;
    if (file_target) {
        collectFileTargetImpacts(scan, target, impacts);
    } else if (node_path_target) {
        collectNodePathImpacts(scan, target, impacts);
    } else {
        collectNameTargetImpacts(scan, target, impacts);
    }
    if (!node_path_target) collectAutoloadImpacts(root, target, file_target, impacts);

    std::sort(impacts.begin(), impacts.end(), [](const Impact& left, const Impact& right) {
        return std::tie(left.path, left.line, left.kind, left.detail) <
               std::tie(right.path, right.line, right.kind, right.detail);
    });

    std::map<std::string, size_t> counts_by_kind;
    for (const auto& impact : impacts) ++counts_by_kind[impact.kind];
    json counts = json::object();
    for (const auto& [kind, count] : counts_by_kind) counts[kind] = count;

    bool truncated = scan.truncated;
    json result = {
        {"target", target},
        {"resolved_kind", file_target ? "file" : (node_path_target ? "node_path" : "name")},
        {"impacts", impactsToJson(impacts, options.max_impacts, truncated)},
        {"impact_count", impacts.size()},
        {"counts_by_kind", std::move(counts)},
        {"scanned_files", scan.sources.size()},
        {"truncated", truncated}
    };

    if (!file_target && !node_path_target) {
        bool declarations_truncated = false;
        result["declared_in"] =
            impactsToJson(declarationsOf(scan, target), options.max_impacts, declarations_truncated);
    }

    // In the payload, not only the docs. A caller who reads the impacts and not
    // this will treat an empty list as permission, which is the one conclusion
    // a static read cannot support.
    result["limitations"] = json::array({
        "A name built at runtime cannot be followed, so an empty impact list is "
        "not proof that nothing depends on the target.",
        "A name match is lexical and whole-word. A local variable that happens "
        "to share the name is reported as a code_reference.",
        "A node path built dynamically or stored in a variable cannot be followed; "
        "node-path results cover only static serialized paths and direct code literals."
    });
    return result;
}

} // namespace didi::offline
