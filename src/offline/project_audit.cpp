#include "didi/offline/project_audit.hpp"

#include "didi/common/project_path.hpp"
#include "didi/offline/resource_indexer.hpp"

#include <algorithm>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace didi::offline {
namespace {

// Every way a project file names another one. Kept in one place so a form that
// is added here is followed by orphan detection and broken-reference detection
// alike, rather than one of them silently lagging.
struct Reference {
    std::string target;   // "res://..." or "uid://..."
    bool is_uid{false};
};

std::string readFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    std::ostringstream contents;
    contents << input.rdbuf();
    return contents.str();
}

void collectMatches(const std::string& text, const std::regex& pattern, bool is_uid,
                    std::vector<Reference>& out) {
    for (auto it = std::sregex_iterator(text.begin(), text.end(), pattern);
         it != std::sregex_iterator(); ++it) {
        out.push_back({(*it)[1].str(), is_uid});
    }
}

std::vector<Reference> referencesIn(const std::string& text) {
    // ext_resource carries a path, a uid, or both, and Godot has been writing
    // more uid-only references since 4.4. Following only path= would call a
    // referenced asset an orphan.
    static const std::regex ext_path(R"re(\[ext_resource[^\]]*path="(res://[^"]+)")re");
    static const std::regex ext_uid(R"re(\[ext_resource[^\]]*uid="(uid://[^"]+)")re");
    // Scripts reference assets too, and skipping them is the difference between
    // a useful orphan list and a wrong one.
    static const std::regex gd_load(R"re((?:preload|load)\s*\(\s*"(res://[^"]+)")re");
    static const std::regex cs_load(R"re(Load\s*(?:<[^>]*>)?\s*\(\s*"(res://[^"]+)")re");
    static const std::regex uid_literal(R"re("(uid://[a-z0-9]+)")re");

    std::vector<Reference> references;
    collectMatches(text, ext_path, false, references);
    collectMatches(text, ext_uid, true, references);
    collectMatches(text, gd_load, false, references);
    collectMatches(text, cs_load, false, references);
    collectMatches(text, uid_literal, true, references);
    return references;
}

bool isAssetType(const std::string& type) {
    return type == "Texture2D" || type == "AudioStream" || type == "MeshResource" ||
           type == "Font" || type == "Shader";
}

// A scene or script is an entry point by nature: nothing has to reference a
// level for it to be the one you open. Only assets are judged.
bool isOrphanCandidate(const ResourceInfo& resource) {
    if (!isAssetType(resource.type)) return false;
    // Godot's own sidecars describe an asset rather than using it.
    return !strings::endsWith(resource.path, ".import") &&
           !strings::endsWith(resource.path, ".uid");
}

struct SignalDeclaration {
    std::string script;
    std::string name;
    int line{0};
};

std::vector<SignalDeclaration> signalsDeclaredIn(const std::string& path,
                                                 const std::string& text) {
    static const std::regex signal_regex(R"re(^\s*signal\s+([A-Za-z_][A-Za-z0-9_]*))re");
    std::vector<SignalDeclaration> declarations;
    std::istringstream lines(text);
    std::string line;
    int number = 0;
    while (std::getline(lines, line)) {
        ++number;
        std::smatch match;
        if (std::regex_search(line, match, signal_regex)) {
            declarations.push_back({path, match[1].str(), number});
        }
    }
    return declarations;
}

// Every signal name that anything emits or connects to, anywhere in the
// project. Editor-made connections live in .tscn as [connection signal="name"],
// and code-made ones as connect("name", ...) or name.connect(...).
//
// This is collected once for the whole project rather than asked per declared
// signal. Asking per signal is six regex passes over every file that contains
// the name, and a name like `changed` is in most of them, so a project with a
// couple of thousand scripts spent tens of seconds here. One pass costs the
// same whether the project declares one signal or a thousand.
std::unordered_set<std::string> usedSignalNames(
    const std::vector<std::pair<std::string, std::string>>& sources) {
    // is_connected is named on its own because `connect` inside it is not
    // followed by an open bracket, so the shorter alternative does not cover it.
    static const std::regex quoted_call(
        R"re((?:emit_signal|is_connected|connect)\s*\(\s*"([A-Za-z_][A-Za-z0-9_]*)")re");
    static const std::regex member_call(
        R"re(\b([A-Za-z_][A-Za-z0-9_]*)\s*\.\s*(?:emit|connect)\s*\()re");
    static const std::regex scene_wired(
        R"re(\[connection[^\]]*signal="([A-Za-z_][A-Za-z0-9_]*)")re");

    std::unordered_set<std::string> used;
    const auto collect = [&used](const std::string& text, const std::regex& pattern) {
        for (auto it = std::sregex_iterator(text.begin(), text.end(), pattern);
             it != std::sregex_iterator(); ++it) {
            used.insert((*it)[1].str());
        }
    };

    for (const auto& [path, text] : sources) {
        (void)path;
        collect(text, quoted_call);
        collect(text, member_call);
        collect(text, scene_wired);
    }
    return used;
}

} // namespace

json auditProject(const std::string& root_dir, const ProjectAuditOptions& options) {
    ResourceIndexer indexer;
    indexer.scan(root_dir);
    const auto resources = indexer.query("res://");

    const auto root = paths::projectPathFromUtf8(root_dir);

    // One read of every text file in the project, reused by all three passes.
    std::vector<std::pair<std::string, std::string>> sources;
    std::unordered_map<std::string, const ResourceInfo*> by_path;
    std::unordered_map<std::string, const ResourceInfo*> by_uid;
    for (const auto& resource : resources) {
        by_path[resource.path] = &resource;
        if (!resource.uid.empty()) by_uid[resource.uid] = &resource;
    }

    for (const auto& resource : resources) {
        const bool textual = resource.type == "PackedScene" || resource.type == "Resource" ||
                             resource.type == "GDScript" || resource.type == "CSharpScript" ||
                             resource.type == "Shader";
        if (!textual) continue;
        auto relative = resource.path;
        if (strings::startsWith(relative, "res://")) relative.erase(0, 6);
        auto text = readFile(root / paths::projectPathFromUtf8(relative));
        if (!text.empty()) sources.emplace_back(resource.path, std::move(text));
    }

    // A uid reference is resolved to the path it names, so "referenced" has one
    // meaning here rather than two that can drift apart.
    std::unordered_set<std::string> referenced_paths;
    json broken = json::array();
    // One ext_resource line carries both a path and a uid, and a uid also
    // matches the bare literal form, so the same missing target can be found
    // more than once. Report it once.
    std::unordered_set<std::string> broken_seen;

    const auto recordBroken = [&](const std::string& source, const std::string& target,
                                  const char* kind) {
        if (!options.include_broken_references) return;
        if (!broken_seen.insert(source + '\n' + target).second) return;
        if (broken.size() >= options.max_findings) return;
        broken.push_back({{"source", source}, {"target", target}, {"kind", kind}});
    };

    for (const auto& [source_path, text] : sources) {
        for (const auto& reference : referencesIn(text)) {
            if (reference.is_uid) {
                const auto found = by_uid.find(reference.target);
                if (found == by_uid.end()) {
                    recordBroken(source_path, reference.target, "unresolved_uid");
                } else {
                    referenced_paths.insert(found->second->path);
                }
                continue;
            }
            referenced_paths.insert(reference.target);
            if (by_path.find(reference.target) == by_path.end()) {
                recordBroken(source_path, reference.target, "missing_file");
            }
        }
    }

    json orphans = json::array();
    uint64_t orphan_bytes = 0;
    if (options.include_orphans) {
        for (const auto& resource : resources) {
            if (!isOrphanCandidate(resource)) continue;
            if (referenced_paths.count(resource.path) != 0) continue;
            orphan_bytes += resource.file_size;
            if (orphans.size() < options.max_findings) {
                orphans.push_back({{"path", resource.path},
                                   {"type", resource.type},
                                   {"file_size", resource.file_size}});
            }
        }
    }

    json dead_signals = json::array();
    if (options.include_dead_signals) {
        const auto used = usedSignalNames(sources);
        for (const auto& [source_path, text] : sources) {
            if (!strings::endsWith(source_path, ".gd")) continue;
            for (const auto& declaration : signalsDeclaredIn(source_path, text)) {
                if (used.count(declaration.name) != 0) continue;
                if (dead_signals.size() >= options.max_findings) break;
                dead_signals.push_back({{"script", declaration.script},
                                        {"signal", declaration.name},
                                        {"line", declaration.line}});
            }
        }
    }

    json result = {
        {"scanned_resources", resources.size()},
        {"scanned_text_files", sources.size()},
        {"orphans", orphans},
        {"orphan_bytes", orphan_bytes},
        {"broken_references", broken},
        {"dead_signals", dead_signals},
        {"max_findings", options.max_findings}
    };
    if (indexer.truncated()) result["truncated"] = true;

    // Said in the payload, not only in the docs, because these are the two ways
    // a caller can act on this and be wrong.
    result["limitations"] = json::array({
        "A path a script builds at runtime cannot be followed, so an asset in "
        "use may still be listed as an orphan.",
        "A signal is reported as dead only when no file emits it, connects to "
        "it, or wires it in a scene. A connection made through a variable name "
        "cannot be seen."
    });
    return result;
}

} // namespace didi::offline
