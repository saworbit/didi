#include "didi/offline/project_impact.hpp"

#include "didi/common/project_path.hpp"
#include "didi/offline/project_text_scan.hpp"

#include <algorithm>
#include <functional>
#include <fstream>
#include <map>
#include <regex>
#include <sstream>

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
    if (!file_target && !looksLikeIdentifier(target)) {
        return Error::invalidArgument(
            "target must be a res:// or uid:// path, or a single Godot identifier");
    }

    const auto scan = scanProjectText(root_dir);
    const auto root = paths::projectPathFromUtf8(root_dir);

    std::vector<Impact> impacts;
    if (file_target) {
        collectFileTargetImpacts(scan, target, impacts);
    } else {
        collectNameTargetImpacts(scan, target, impacts);
    }
    collectAutoloadImpacts(root, target, file_target, impacts);

    std::map<std::string, size_t> counts_by_kind;
    for (const auto& impact : impacts) ++counts_by_kind[impact.kind];
    json counts = json::object();
    for (const auto& [kind, count] : counts_by_kind) counts[kind] = count;

    bool truncated = scan.truncated;
    json result = {
        {"target", target},
        {"resolved_kind", file_target ? "file" : "name"},
        {"impacts", impactsToJson(impacts, options.max_impacts, truncated)},
        {"impact_count", impacts.size()},
        {"counts_by_kind", std::move(counts)},
        {"scanned_files", scan.sources.size()},
        {"truncated", truncated}
    };

    if (!file_target) {
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
        "to share the name is reported as a code_reference."
    });
    return result;
}

} // namespace didi::offline
