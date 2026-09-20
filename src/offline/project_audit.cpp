#include "didi/offline/project_audit.hpp"

#include "didi/common/config_file_syntax.hpp"
#include "didi/common/project_path.hpp"
#include "didi/offline/import_health.hpp"
#include "didi/offline/project_text_scan.hpp"
#include "didi/offline/resource_indexer.hpp"

#include <algorithm>
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
    // A plain quoted path, which is how project.godot and an exported string
    // property name a file. It counts as use, so the target is not an orphan,
    // and it is not checked for existence: "res://levels/" + name + ".tscn" is
    // one complete literal to a regex and half a path on disk, and a broken
    // reference that is not broken is worse than one that is not reported.
    bool use_only{false};
};

// Whether a pattern can match at all, asked of the shortest literal every one
// of its matches must contain.
//
// A resource writes a packed array on one line, and running a pattern over one
// of those costs time quadratic in its length. The dead-signal collector below
// spent ten seconds on a single four-hundred-kilobyte line in a .tres that
// mentions no signal at all (#661). A literal the pattern requires is a byte
// comparison, so a file that cannot match is skipped for the price of one scan
// of its bytes, and the answer does not change.
bool mayContain(const std::string& text, std::initializer_list<const char*> literals) {
    for (const auto* literal : literals) {
        if (text.find(literal) != std::string::npos) return true;
    }
    return false;
}

// What is wrong with project.godot itself, as opposed to the files it names.
//
// Every other finding here is about a reference from one file to another. This
// one is about the manifest: a file the engine will not parse, and a setting
// the engine registers under a name nobody can use. Both are states in which
// every other answer about the project describes a project that does not run,
// and neither was reported anywhere until now (#817, #818).
//
// The scan computes all three facts already. `complete` is false exactly when
// the file ends inside a value, which is one of the cases Godot answers with
// ERR_PARSE_ERROR. `valueProblem` is the rest of that set that a reader can
// prove without the engine. `key` against `key_on_line` is the name the engine
// registers against the name the line looks like it declares, and they differ
// when a line with no `=` joined forward into this one.
json projectSettingsIssues(const std::string& text, size_t max_findings) {
    json issues = json::array();
    const auto scanned = config_file::scan(text);
    if (!scanned.complete) {
        json finding = {
            {"kind", "unparseable_project_settings"},
            {"detail",
             "project.godot ends part-way through a value. Godot answers ERR_PARSE_ERROR "
             "for it, the project does not open, and none of the settings in the file are "
             "what it runs on."}};
        // The last key read is the one whose value never closed, which is the
        // line to repair. Naming it is the difference between a verdict and a
        // remedy.
        if (!scanned.entries.empty()) {
            finding["section"] = scanned.entries.back().section;
            finding["key"] = scanned.entries.back().key;
            finding["line"] = scanned.entries.back().line;
        }
        issues.push_back(std::move(finding));
    }
    for (const auto& entry : scanned.entries) {
        if (issues.size() >= max_findings) break;
        // Balanced is not loadable. `config/broken=)` closes every bracket it
        // opens and is still ERR_PARSE_ERROR, so the audit answered
        // project_settings_issue_count: 0 for a project that does not open
        // (#820).
        const auto problem = config_file::valueProblem(entry.value_text);
        if (problem.empty()) continue;
        issues.push_back(
            {{"kind", "unloadable_setting_value"},
             {"section", entry.section},
             {"key", entry.key},
             {"line", entry.line},
             {"detail", "Godot's parser refuses this value, because " + problem +
                            ". The engine answers ERR_PARSE_ERROR for the whole file, so the "
                            "project does not open and none of the settings in it are what it "
                            "runs on."}});
    }
    for (const auto& entry : scanned.entries) {
        if (!entry.joined || entry.key == entry.key_on_line) continue;
        if (issues.size() >= max_findings) break;
        // Both names, because the remedy is to move or delete one line and the
        // user has to be told which. The line the join started on is the line
        // to move.
        issues.push_back({{"kind", "unusable_setting_name"},
                          {"section", entry.section},
                          {"registered_key", entry.key},
                          {"key_on_line", entry.key_on_line},
                          {"line", entry.line},
                          {"joined_from_line", entry.key_line},
                          {"detail",
                           "Godot registers this setting as \"" + entry.section + "/" +
                               entry.key + "\", not \"" + entry.section + "/" +
                               entry.key_on_line + "\". A line with no = does not end a key: "
                               "it joins forward into the next line that has one, so the text "
                               "on line " + std::to_string(entry.key_line) +
                               " became part of this name. Nothing in the project can refer to "
                               "the setting under the name it appears to have."}});
    }
    return issues;
}

void collectMatches(const std::string& text, const std::regex& pattern, bool is_uid,
                    std::vector<Reference>& out, bool use_only = false) {
    for (auto it = std::sregex_iterator(text.begin(), text.end(), pattern);
         it != std::sregex_iterator(); ++it) {
        out.push_back({(*it)[1].str(), is_uid, use_only});
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
    // A quoted res:// value, which is the only form project.godot has:
    // config/icon, boot_splash/image and run/main_scene write it bare, the
    // [autoload] section prefixes it with the enabled marker, and
    // locale/translations writes it inside PackedStringArray(...). Asked of
    // every file for the same reason uid_literal is: a path in an exported
    // string property is a use, and calling its target an orphan is the one
    // mistake this list must not make. Bounded because an unbounded run over a
    // packed metadata line is what overflowed the stack in #661, and a res://
    // path is never a kilobyte.
    static const std::regex res_literal(R"re("\*?(res://[^"]{1,1024})")re");

    std::vector<Reference> references;
    if (mayContain(text, {"[ext_resource"})) {
        collectMatches(text, ext_path, false, references);
        collectMatches(text, ext_uid, true, references);
    }
    if (mayContain(text, {"res://"})) {
        collectMatches(text, gd_load, false, references);
        collectMatches(text, cs_load, false, references);
        collectMatches(text, res_literal, false, references, true);
    }
    if (mayContain(text, {"uid://"})) collectMatches(text, uid_literal, true, references);
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

// GDScript identifiers may hold Unicode letters (UAX#31), and source is read as
// UTF-8, so a name class that stops at ASCII captures a truncated name and the
// audit then reports a signal that does not exist. Every byte outside ASCII is
// part of the name.
//
// Three things about the shape of this pattern, each of which cost a CI run:
//
// One character class, not an alternation. std::regex backtracks through
// (?:a|b)* at every byte, and MSVC gave up on a long .tscn line rather than
// finish.
//
// A bounded repeat, not a star. libstdc++ recurses once per repetition, so an
// unbounded run over a packed metadata string of 300k characters overflowed
// the stack. No signal is named in a kilobyte, and a run longer than that is
// not an identifier, so refusing to consider it costs nothing real.
constexpr int kMaxIdentifierBytes = 1024;
const std::string kIdentifierPattern =
    R"re([A-Za-z_\x80-\xFF][A-Za-z0-9_\x80-\xFF]{0,)re" +
    std::to_string(kMaxIdentifierBytes - 1) + "}";
// And a left boundary, because \b is ASCII-only and fails in front of a name
// that starts with a Unicode letter. It has to be replaced rather than
// dropped: without one the engine starts a fresh name run at every byte of a
// line, which is quadratic, and the same packed line spun for minutes. This
// consumes the byte in front of the name, which is fine for a scan that only
// collects the names it sees.
const std::string kNameLeftBoundary = R"re((?:^|[^A-Za-z0-9_\x80-\xFF]))re";

std::vector<SignalDeclaration> signalsDeclaredIn(const std::string& path,
                                                 const std::string& text) {
    static const std::regex signal_regex(R"re(^\s*signal\s+()re" + kIdentifierPattern + R"re())re");
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
std::unordered_set<std::string> usedSignalNames(const std::vector<ProjectTextSource>& sources) {
    // is_connected is named on its own because `connect` inside it is not
    // followed by an open bracket, so the shorter alternative does not cover it.
    static const std::regex quoted_call(
        R"re((?:emit_signal|is_connected|connect)\s*\(\s*"()re" + kIdentifierPattern + R"re()")re");
    static const std::regex member_call(
        kNameLeftBoundary + "(" + kIdentifierPattern +
        R"re()\s*\.\s*(?:emit|connect)\s*\()re");
    static const std::regex scene_wired(
        R"re(\[connection[^\]]*signal="()re" + kIdentifierPattern + R"re()")re");

    std::unordered_set<std::string> used;
    const auto collect = [&used](const std::string& text, const std::regex& pattern) {
        for (auto it = std::sregex_iterator(text.begin(), text.end(), pattern);
             it != std::sregex_iterator(); ++it) {
            used.insert((*it)[1].str());
        }
    };
    // member_call is the one that costs, and the cost is quadratic in the
    // length of the run it is scanning. A resource writes a packed array on one
    // line, and this took ten seconds on a single four-hundred-kilobyte line
    // (#661), so it is asked a line at a time and only of lines that carry the
    // literal every one of its matches ends with. The other two are asked of
    // the whole text as before: both need a run of letters the engine fails on
    // immediately inside a numeric array, and quoted_call's `connect(` is
    // regularly written across a line break.
    //
    // A member call is not, because GDScript continues a line only inside
    // brackets, so `name` and `.connect(` are on one line in anything the
    // parser accepts. That is stated in the payload's limitations.
    const auto collectPerLine = [&used, &collect](const std::string& text,
                                                  const std::regex& pattern,
                                                  std::initializer_list<const char*> literals) {
        size_t begin = 0;
        while (begin <= text.size()) {
            auto end = text.find('\n', begin);
            if (end == std::string::npos) end = text.size();
            const auto line = text.substr(begin, end - begin);
            if (mayContain(line, literals)) collect(line, pattern);
            begin = end + 1;
        }
    };

    for (const auto& source : sources) {
        const auto& text = source.contents;
        if (mayContain(text, {"connect", "emit_signal"})) collect(text, quoted_call);
        if (mayContain(text, {"emit", "connect"})) collectPerLine(text, member_call, {"emit", "connect"});
        if (mayContain(text, {"[connection"})) collect(text, scene_wired);
    }
    return used;
}

} // namespace

json auditProject(const std::string& root_dir, const ProjectAuditOptions& options) {
    // One read of every text file in the project, reused by all three passes,
    // through the shared scan rather than a second copy of the same loop. The
    // copy this replaces had no bounds and its own list of which types carry
    // text, so a file type added to one was invisible to the other (#664). The
    // index underneath is still shared with the other read tools, so a run of
    // resource_inspect, project_list_resources and project_analyze_bloat
    // crawls the tree once.
    const auto scan = scanProjectText(root_dir);
    const auto& resources = scan.resources;
    const auto& sources = scan.sources;

    std::unordered_map<std::string, const ResourceInfo*> by_path;
    std::unordered_map<std::string, const ResourceInfo*> by_uid;
    for (const auto& resource : resources) {
        by_path[resource.path] = &resource;
        if (!resource.uid.empty()) by_uid[resource.uid] = &resource;
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

    const auto collectReferences = [&](const std::string& source_path, const std::string& text) {
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
            if (reference.use_only) continue;
            if (by_path.find(reference.target) == by_path.end()) {
                recordBroken(source_path, reference.target, "missing_file");
            }
        }
    };

    for (const auto& [source_path, text] : sources) collectReferences(source_path, text);
    // project.godot names resources and is not one, so it was never in sources
    // and its targets were reported as orphans in every project -- the icon
    // every Godot project ships among them (#774). Read through the same scan
    // as everything else, so there is still one read of the project.
    if (scan.project_settings.has_value()) {
        collectReferences(scan.project_settings->path, scan.project_settings->contents);
    }

    json orphans = json::array();
    uint64_t orphan_bytes = 0;
    size_t excluded_addon_orphans = 0;
    if (options.include_orphans) {
        for (const auto& resource : resources) {
            if (!isOrphanCandidate(resource)) continue;
            if (referenced_paths.count(resource.path) != 0) continue;
            if (!options.include_addon_orphans &&
                strings::startsWith(resource.path, "res://addons/")) {
                // Counted rather than dropped, so the number is explainable and
                // a caller who does want them knows there is something to ask
                // for.
                ++excluded_addon_orphans;
                continue;
            }
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

    json import_health = {
        {"scanned_import_metadata", 0},
        {"import_issues", json::array()},
        {"import_issue_count", 0}
    };
    if (options.include_import_health) {
        import_health = inspectImportHealth(root_dir, options.max_findings);
    }

    json project_settings_issues = json::array();
    if (scan.project_settings.has_value()) {
        project_settings_issues =
            projectSettingsIssues(scan.project_settings->contents, options.max_findings);
    }

    json result = {
        {"scanned_resources", resources.size()},
        {"scanned_text_files", sources.size() + (scan.project_settings.has_value() ? 1u : 0u)},
        {"skipped_text_files", scan.skipped_files},
        {"orphans", orphans},
        {"orphan_bytes", orphan_bytes},
        {"excluded_addon_orphans", excluded_addon_orphans},
        {"addon_orphans_included", options.include_addon_orphans},
        {"broken_references", broken},
        {"dead_signals", dead_signals},
        {"max_findings", options.max_findings},
        {"scanned_import_metadata", import_health["scanned_import_metadata"]},
        {"import_issues", import_health["import_issues"]},
        {"import_issue_count", import_health["import_issue_count"]},
        {"project_settings_issues", project_settings_issues},
        {"project_settings_issue_count", project_settings_issues.size()}
    };
    if (scan.truncated) result["truncated"] = true;
    // Same reason as the scan bounds above: a file this cannot name is a file
    // the answer is missing, and saying so beats omitting it (#650).
    {
        const auto indexer = ResourceIndexer::sharedIndex(root_dir);
        if (indexer->undecodablePathCount() > 0) {
            result["undecodable_paths"] = indexer->undecodablePaths();
            result["undecodable_path_count"] = indexer->undecodablePathCount();
        }
    }
    if (import_health.contains("import_scan_truncated")) {
        result["import_scan_truncated"] = true;
    }

    // Said in the payload, not only in the docs, because these are the two ways
    // a caller can act on this and be wrong.
    result["limitations"] = json::array({
        "A path a script builds at runtime cannot be followed, so an asset in "
        "use may still be listed as an orphan.",
        "Files under res://addons/ are third-party and are not counted as "
        "orphans by default. excluded_addon_orphans says how many were left "
        "out; pass include_addon_orphans to see them.",
        "A signal is reported as dead only when no file emits it, connects to "
        "it, or wires it in a scene. A connection made through a variable name "
        "cannot be seen, and neither can a member call written with the name "
        "and the .connect on different lines.",
        "source_changed_since_import and output_changed_since_import compare the "
        "source and the outputs against the source_md5 and dest_md5 Godot "
        "recorded in the .md5 it wrote beside the output. Those are two of the "
        "three questions the engine asks; it also checks the importer's version, "
        "which is not checked here, so no finding is not a promise that Godot "
        "will leave the asset alone. import_freshness_unchecked says a record is "
        "there and a half of it was not compared, and its detail says which half "
        "and why. source_newer_than_output is the fallback where there is no "
        "record at all: it compares modification times, which a clone, a checkout "
        "or a worktree does not preserve, so it is evidence rather than a "
        "verdict, and the remedy is to open the project in the editor once so a "
        "record exists.",
        "project_settings_issues reports the three states of project.godot that a "
        "reader can see without the engine: a file that ends part-way through a "
        "value, a value the parser cannot start, and a setting registered under a "
        "name the join built. It is not a full parse, so a constructor with the "
        "wrong arity, one the engine does not know, and a Resource() whose file is "
        "missing are all ERR_PARSE_ERROR and none of them is reported. An empty "
        "list is not a promise that Godot will load the file; a finding is a "
        "promise that it will not."
    });
    return result;
}

} // namespace didi::offline
