#include "didi/offline/speculative_verify.hpp"
#include "didi/mcp/mcp_protocol.hpp"
#include "didi/common/ipc_channel.hpp"
#include "didi/common/logger.hpp"
#include "didi/offline/resource_indexer.hpp"
#include "didi/offline/project_audit.hpp"
#include "didi/offline/project_impact.hpp"
#include "didi/offline/audio_bus_layout.hpp"
#include "didi/common/project_path.hpp"
#include "didi/common/atomic_write.hpp"
#include <cctype>
#include <cstdint>
#include <fstream>
#include <initializer_list>
#include <map>
#include <sstream>
#include <set>
#include <string>
#include <vector>

namespace didi {
namespace mcp {

CallToolResult handleQueryProjectResources(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    (void)ipc;
    std::string search_path = args.value("search_path", "res://");
    std::string type_filter = args.value("type_filter", "");
    std::string fuzzy_query = args.value("fuzzy_query", "");
    bool include_uid = args.value("include_uid", true);

    const auto indexer = offline::ResourceIndexer::sharedIndex(".");
    auto results = indexer->query(search_path, type_filter, fuzzy_query);

    json res_arr = json::array();
    for (const auto& item : results) {
        json j = item.toJson();
        if (!include_uid) j.erase("uid");
        res_arr.push_back(j);
    }

    json out = {
        {"total_found", results.size()},
        {"resources", res_arr}
    };
    if (indexer->truncated()) out["truncated"] = true;
    return CallToolResult::successJson(out);
}

namespace {

bool hasNumericKeys(const json& value, std::initializer_list<const char*> keys) {
    for (const auto* key : keys) {
        if (!value.contains(key) || !value[key].is_number()) return false;
    }
    return true;
}

// Renders a JSON object as the Godot text-resource literal it actually stands
// for. Anything that is not a recognised built-in becomes a dictionary, which
// is what a .tres file expects, rather than a fabricated vector constructor.
template <typename Escape>
std::string tresObjectLiteral(const json& value, const Escape& escape) {
    std::ostringstream out;
    const auto number = [&](const char* key) { return value[key].dump(); };

    if (value.size() == 4 && hasNumericKeys(value, {"r", "g", "b", "a"})) {
        out << "Color(" << number("r") << ", " << number("g") << ", "
            << number("b") << ", " << number("a") << ")";
        return out.str();
    }
    if (value.size() == 3 && hasNumericKeys(value, {"r", "g", "b"})) {
        out << "Color(" << number("r") << ", " << number("g") << ", "
            << number("b") << ", 1)";
        return out.str();
    }
    if (hasNumericKeys(value, {"x", "y", "z", "w"})) {
        // Godot writes rotations as Quaternion and 4D vectors as Vector4. The
        // caller says which with an explicit type, otherwise Vector4 is the
        // safer read of four plain components.
        const auto hint = value.value("type", std::string{});
        out << (hint == "Quaternion" ? "Quaternion(" : "Vector4(")
            << number("x") << ", " << number("y") << ", " << number("z")
            << ", " << number("w") << ")";
        return out.str();
    }
    if (hasNumericKeys(value, {"x", "y", "z"})) {
        out << "Vector3(" << number("x") << ", " << number("y") << ", " << number("z") << ")";
        return out.str();
    }
    if (hasNumericKeys(value, {"x", "y"})) {
        out << "Vector2(" << number("x") << ", " << number("y") << ")";
        return out.str();
    }

    out << "{";
    bool first = true;
    for (auto entry = value.begin(); entry != value.end(); ++entry) {
        if (!first) out << ", ";
        first = false;
        out << "\"" << escape(entry.key()) << "\": ";
        if (entry.value().is_string()) {
            out << "\"" << escape(entry.value().template get<std::string>()) << "\"";
        } else if (entry.value().is_boolean()) {
            out << (entry.value().template get<bool>() ? "true" : "false");
        } else if (entry.value().is_object()) {
            out << tresObjectLiteral(entry.value(), escape);
        } else {
            out << entry.value().dump();
        }
    }
    out << "}";
    return out.str();
}

} // namespace

CallToolResult handleResourceCreate(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    std::string resource_type = args.value("resource_type", "StandardMaterial3D");
    std::string save_path = args.value("save_path", "");
    json properties = args.value("properties", json::object());
    if (args.contains("overwrite") && !args["overwrite"].is_boolean()) {
        return CallToolResult::error("Parameter 'overwrite' must be a boolean.");
    }
    const bool overwrite = args.value("overwrite", false);

    if (save_path.empty()) {
        return CallToolResult::error("Parameter 'save_path' is required (e.g. res://materials/wood.tres).");
    }

    // The body written below is Godot text-resource markup and nothing else.
    // Writing it to any path the caller names produced a .gd file full of
    // [gd_resource] markup, reported as created, that script_check_syntax in
    // the same session immediately called unparseable. Refuse the target
    // instead of writing a file the engine cannot load.
    {
        const auto dot = save_path.find_last_of('.');
        const auto slash = save_path.find_last_of('/');
        std::string extension =
            (dot == std::string::npos || (slash != std::string::npos && dot < slash))
                ? std::string()
                : save_path.substr(dot);
        for (auto& character : extension) {
            character = static_cast<char>(
                std::tolower(static_cast<unsigned char>(character)));
        }
        if (extension != ".tres" && extension != ".res") {
            return CallToolResult::error(
                "resource_create writes Godot text-resource markup, so save_path must end in "
                ".tres or .res; received \"" + save_path +
                "\". Use script_create for a GDScript file.");
        }
    }

    // Offline generator for common .tres resources
    namespace fs = std::filesystem;
    std::string disk_path = save_path;
    if (strings::startsWith(disk_path, "res://")) disk_path = disk_path.substr(6);

    fs::path target_p = paths::projectPathFromUtf8(disk_path);
    fs::path current_root = fs::current_path();
    try {
        auto canon_root = fs::weakly_canonical(current_root);
        auto canon_target = fs::weakly_canonical(current_root / target_p);
        auto [root_it, target_it] = std::mismatch(
            canon_root.begin(), canon_root.end(),
            canon_target.begin(), canon_target.end()
        );
        if (root_it != canon_root.end()) {
            return CallToolResult::error("Access denied: save_path is outside the project root directory.");
        }
        if (fs::exists(canon_target) && !overwrite) {
            return CallToolResult::error(
                "Resource already exists; pass overwrite: true to replace it: " + save_path);
        }
        if (target_p.has_parent_path()) {
            fs::create_directories(target_p.parent_path());
        }
    } catch (const std::exception& e) {
        return CallToolResult::error(std::string("Path resolution error: ") + e.what());
    }

    auto escape_tres_str = [](const std::string& s) -> std::string {
        std::string out;
        for (char c : s) {
            if (c == '\\') out += "\\\\";
            else if (c == '"') out += "\\\"";
            else if (c == '\n') out += "\\n";
            else if (c == '\r') out += "\\r";
            else if (c == '\t') out += "\\t";
            else out += c;
        }
        return out;
    };

    std::ostringstream out;
    out << "[gd_resource type=\"" << resource_type << "\" format=3]\n\n"
        << "[resource]\n";
    for (auto it = properties.begin(); it != properties.end(); ++it) {
        out << it.key() << " = ";
        if (it.value().is_string()) {
            out << "\"" << escape_tres_str(it.value().get<std::string>()) << "\"\n";
        } else if (it.value().is_boolean()) {
            out << (it.value().get<bool>() ? "true" : "false") << "\n";
        } else if (it.value().is_number()) {
            out << it.value().dump() << "\n";
        } else if (it.value().is_array()) {
            out << "[";
            bool first = true;
            for (const auto& elem : it.value()) {
                if (!first) out << ", ";
                first = false;
                if (elem.is_string()) {
                    out << "\"" << escape_tres_str(elem.get<std::string>()) << "\"";
                } else if (elem.is_boolean()) {
                    out << (elem.get<bool>() ? "true" : "false");
                } else {
                    out << elem.dump();
                }
            }
            out << "]\n";
        } else if (it.value().is_object()) {
            out << tresObjectLiteral(it.value(), escape_tres_str) << "\n";
        } else {
            out << it.value().dump() << "\n";
        }
    }
    auto written = files::writeFileAtomically(target_p, out.str());
    offline::ResourceIndexer::invalidateSharedIndex();
    if (written.isErr()) {
        return CallToolResult::error("Failed to write resource file to disk: " +
                                     written.error().message);
    }
    return CallToolResult::successJson({
        {"status", "created_offline"},
        {"save_path", save_path},
        {"resource_type", resource_type}
    });
}

CallToolResult handleResourceInspect(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    std::string resource_path = args.value("resource_path", "");
    if (resource_path.empty()) {
        return CallToolResult::error("Parameter 'resource_path' is required.");
    }

    // Exact match. A prefix match reported res://player.gd.uid or
    // res://player.gdextension for res://player.gd, and the scan order is
    // unsorted, so which sibling came back was down to directory order.
    const auto indexer = offline::ResourceIndexer::sharedIndex(".");
    if (const auto* found = indexer->findExact(resource_path)) {
        return CallToolResult::successJson(found->toJson());
    }

    return CallToolResult::error("Resource not found: " + resource_path);
}

CallToolResult handleAudioConfigureBus(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    // Live only, and that is the point rather than a gap. Writing the layout
    // file would change what the project loads next time and not what anyone
    // is listening to now, which is the opposite of what someone chasing a
    // silent bus wants.
    if (!ipc || !ipc->isConnected()) {
        return CallToolResult::error(
            "Godot Editor is offline. Audio bus state lives in the running engine, so launch "
            "Godot to change it. audio_list_buses still reads the project layout offline.");
    }
    auto response = ipc->sendRequest("audio.configureBus", args, ipc::kWaitForDefinitiveResponse);
    if (response.isErr()) {
        return CallToolResult::error("Failed to configure the audio bus: " + response.error().message);
    }
    return CallToolResult::successJson(response.value());
}

CallToolResult handleAudioListBuses(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    if (!args.is_object() || !args.empty()) {
        return CallToolResult::error("Invalid audio request: this tool takes no arguments");
    }
    // Live first, because a bus a script muted at runtime is exactly the case
    // someone is looking for and the layout file cannot show it. The offline
    // read is a fallback, and it says so in the result rather than letting the
    // caller assume they are looking at live state.
    if (ipc && ipc->isConnected()) {
        auto response = ipc->sendRequest("audio.listBuses", args, ipc::kWaitForDefinitiveResponse);
        if (response.isOk()) return CallToolResult::successJson(response.value());
    }

    auto layout = offline::readAudioBusLayout(".");
    if (layout.isErr()) return CallToolResult::error(layout.error().message);
    auto payload = layout.value();
    payload["execution_mode"] = "offline_fallback";
    payload["is_live_engine"] = false;
    return CallToolResult::successJson(std::move(payload));
}

CallToolResult handleProjectRenameReferences(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    (void)ipc;
    if (!args.is_object()) {
        return CallToolResult::error("Invalid rename request: arguments must be an object");
    }
    offline::ProjectRenameOptions options;
    if (!args.contains("target") || !args["target"].is_string()) {
        return CallToolResult::error("Invalid rename request: target must be a string");
    }
    if (!args.contains("new_name") || !args["new_name"].is_string()) {
        return CallToolResult::error("Invalid rename request: new_name must be a string");
    }
    options.target = args["target"].get<std::string>();
    options.new_name = args["new_name"].get<std::string>();
    if (args.contains("max_impacts")) {
        const auto& value = args["max_impacts"];
        if (!value.is_number_integer() || value.get<int64_t>() < 1 || value.get<int64_t>() > 5000) {
            return CallToolResult::error(
                "Invalid rename request: max_impacts must be an integer from 1 to 5000");
        }
        options.max_impacts = static_cast<size_t>(value.get<int64_t>());
    }

    auto report = offline::renameReferences(".", options);
    if (report.isErr()) {
        // The conflict and truncation refusals carry the evidence a caller needs
        // to act, so they travel with the message rather than being flattened
        // into it.
        const auto& failure = report.error();
        if (failure.data.is_object() && !failure.data.empty()) {
            return CallToolResult::error(
                json{{"error", {{"code", failure.code}, {"message", failure.message},
                                {"data", failure.data}}}}.dump());
        }
        return CallToolResult::error(failure.message);
    }
    auto payload = report.value();
    payload["execution_mode"] = "offline_fallback";
    return CallToolResult::successJson(std::move(payload));
}

CallToolResult handleProjectVerifyChanges(const json& args) {
    auto parsed = offline::parseSpeculativeVerifyRequest(args);
    if (parsed.isErr()) {
        return CallToolResult::error("Invalid verification request: " + parsed.error().message);
    }
    auto verified = offline::verifyChangesInSandbox(parsed.value());
    if (verified.isErr()) {
        return CallToolResult::error(verified.error().message);
    }
    return CallToolResult::successJson(verified.value().toJson());
}

CallToolResult handleProjectApplyChanges(const json& args) {
    auto parsed = offline::parseSpeculativeVerifyRequest(args);
    if (parsed.isErr()) {
        return CallToolResult::error("Invalid apply request: " + parsed.error().message);
    }
    auto applied = offline::applyVerifiedChanges(parsed.value());
    if (applied.isErr()) {
        const auto& failure = applied.error();
        if (failure.data.is_object() && !failure.data.empty()) {
            return CallToolResult::error(
                json{{"error", {{"code", failure.code}, {"message", failure.message},
                                {"data", failure.data}}}}.dump());
        }
        return CallToolResult::error(failure.message);
    }
    auto payload = applied.value().toJson();
    payload["execution_mode"] = "offline_fallback";
    // A proposal the check rejected is not an error in this tool. The tool did
    // what it promises, which is to write nothing when the proposal does not
    // hold up, and the report says why.
    auto result = CallToolResult::successJson(std::move(payload));
    result.isError = !applied.value().applied;
    return result;
}

CallToolResult handleProjectAnalyzeImpact(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    (void)ipc;
    if (!args.is_object()) {
        return CallToolResult::error("Invalid impact request: arguments must be an object");
    }
    offline::ProjectImpactOptions options;
    if (!args.contains("target") || !args["target"].is_string()) {
        return CallToolResult::error("Invalid impact request: target must be a string");
    }
    options.target = args["target"].get<std::string>();
    if (args.contains("max_impacts")) {
        const auto& value = args["max_impacts"];
        if (!value.is_number_integer() || value.get<int64_t>() < 1 || value.get<int64_t>() > 5000) {
            return CallToolResult::error(
                "Invalid impact request: max_impacts must be an integer from 1 to 5000");
        }
        options.max_impacts = static_cast<size_t>(value.get<int64_t>());
    }

    auto report = offline::analyzeImpact(".", options);
    if (report.isErr()) return CallToolResult::error(report.error().message);
    auto payload = report.value();
    payload["execution_mode"] = "offline_fallback";
    return CallToolResult::successJson(std::move(payload));
}

CallToolResult handleProjectAuditAssets(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    if (!args.is_object()) {
        return CallToolResult::error("Invalid audit request: arguments must be an object");
    }
    offline::ProjectAuditOptions options;
    for (const auto& [key, target] : {std::pair<const char*, bool*>{"include_orphans", &options.include_orphans},
                                      {"include_broken_references", &options.include_broken_references},
                                      {"include_dead_signals", &options.include_dead_signals},
                                      {"include_import_health", &options.include_import_health}}) {
        if (!args.contains(key)) continue;
        if (!args[key].is_boolean()) {
            return CallToolResult::error(std::string("Invalid audit request: ") + key +
                                         " must be a boolean");
        }
        *target = args[key].get<bool>();
    }
    if (args.contains("max_findings")) {
        const auto& value = args["max_findings"];
        if (!value.is_number_integer() || value.get<int64_t>() < 1 || value.get<int64_t>() > 5000) {
            return CallToolResult::error(
                "Invalid audit request: max_findings must be an integer from 1 to 5000");
        }
        options.max_findings = static_cast<size_t>(value.get<int64_t>());
    }
    if (!options.include_orphans && !options.include_broken_references &&
        !options.include_dead_signals && !options.include_import_health) {
        return CallToolResult::error(
            "Invalid audit request: at least one of include_orphans, "
            "include_broken_references, include_dead_signals or include_import_health "
            "must stay enabled");
    }

    auto report = offline::auditProject(".", options);
    // The scan is a file scan in every case and says so on every call. The
    // execution mode below describes whether an engine contributed to the
    // findings, which is the question a caller weighing them is actually
    // asking; uid_verification carries the detail.
    report["scan_source"] = "project_files";
    report["execution_mode"] = "offline_fallback";

    // A uid no project file records is reported as a broken reference, because
    // offline that is the only reading available. With an editor attached the
    // engine's own table can say whether it is true, and a reference the engine
    // resolves is not broken -- nor is the file it points at an orphan. Both
    // findings are corrected rather than annotated, because leaving a finding
    // in place with a footnote saying it is wrong is how a tool trains people
    // to skim past it.
    constexpr size_t kMaxUidVerifications = 256; // the bridge's own batch cap
    std::vector<std::string> candidates;
    std::set<std::string> distinct_unresolved;
    if (report.contains("broken_references") && report["broken_references"].is_array()) {
        for (const auto& entry : report["broken_references"]) {
            if (entry.value("kind", std::string{}) != "unresolved_uid") continue;
            const auto target = entry.value("target", std::string{});
            if (target.empty() || !distinct_unresolved.insert(target).second) continue;
            if (candidates.size() < kMaxUidVerifications) candidates.push_back(target);
        }
    }

    json verification{{"mode", candidates.empty() ? "not_needed" : "unavailable"},
                      {"checked", 0},
                      {"truncated", distinct_unresolved.size() > candidates.size()}};

    if (!candidates.empty() && ipc && ipc->isConnected()) {
        auto response = ipc->sendRequest("project.resolveUids", json{{"queries", candidates}},
                                         ipc::kWaitForDefinitiveResponse);
        if (response.isOk() && response.value().is_object() &&
            response.value().contains("entries") && response.value()["entries"].is_array()) {
            std::map<std::string, std::string> engine_paths;
            std::set<std::string> engine_unknown;
            for (const auto& entry : response.value()["entries"]) {
                const auto query = entry.value("query", std::string{});
                if (query.empty()) continue;
                if (entry.value("found", false)) {
                    engine_paths[query] = entry.value("path", std::string{});
                } else {
                    engine_unknown.insert(query);
                }
            }

            json kept = json::array();
            json engine_only = json::array();
            for (auto entry : report["broken_references"]) {
                if (entry.value("kind", std::string{}) == "unresolved_uid") {
                    const auto target = entry.value("target", std::string{});
                    const auto resolved = engine_paths.find(target);
                    if (resolved != engine_paths.end()) {
                        engine_only.push_back({{"source", entry.value("source", std::string{})},
                                               {"target", target},
                                               {"engine_path", resolved->second}});
                        continue;
                    }
                    if (engine_unknown.count(target) != 0) entry["confirmed_by_engine"] = true;
                }
                kept.push_back(std::move(entry));
            }
            report["broken_references"] = std::move(kept);
            report["engine_only_references"] = engine_only;

            // A file the engine proved is referenced cannot also be unreferenced.
            size_t orphans_cleared = 0;
            if (!engine_paths.empty() && report.contains("orphans") && report["orphans"].is_array()) {
                std::set<std::string> resolved_paths;
                for (const auto& [uid, resolved_path] : engine_paths) {
                    if (!resolved_path.empty()) resolved_paths.insert(resolved_path);
                }
                json kept_orphans = json::array();
                uint64_t reclaimed = 0;
                for (const auto& orphan : report["orphans"]) {
                    if (resolved_paths.count(orphan.value("path", std::string{})) != 0) {
                        reclaimed += orphan.value("file_size", static_cast<uint64_t>(0));
                        ++orphans_cleared;
                        continue;
                    }
                    kept_orphans.push_back(orphan);
                }
                if (orphans_cleared > 0) {
                    report["orphans"] = std::move(kept_orphans);
                    const auto counted = report.value("orphan_bytes", static_cast<uint64_t>(0));
                    report["orphan_bytes"] = counted > reclaimed ? counted - reclaimed : 0;
                }
            }

            report["execution_mode"] = "live";
            verification["mode"] = "live";
            verification["checked"] = candidates.size();
            verification["resolved_by_engine"] = engine_paths.size();
            verification["confirmed_broken"] = engine_unknown.size();
            verification["orphans_cleared"] = orphans_cleared;

            if (!engine_only.empty() && report.contains("limitations") &&
                report["limitations"].is_array()) {
                report["limitations"].push_back(
                    "A uid under engine_only_references resolves in this editor but no project "
                    "file records it. The editor's table is not in the repository, so a fresh "
                    "checkout or another machine would report it broken.");
            }
        }
    }
    report["uid_verification"] = std::move(verification);
    return CallToolResult::successJson(std::move(report));
}

namespace {

// What the project files say about a query, next to what the engine said. The
// two disagree exactly when a sidecar is stale, missing, or newer than the
// editor's table, which is the drift a caller is looking for.
std::string uidIndexAgreement(const json& uid_map,
                              const std::map<std::string, std::string>& path_to_uid,
                              const json& engine_entry) {
    const std::string query = engine_entry.value("query", "");
    const bool engine_found = engine_entry.value("found", false);
    std::string index_answer;
    bool index_has = false;
    if (query.rfind("uid://", 0) == 0) {
        if (uid_map.contains(query)) {
            index_has = true;
            index_answer = uid_map.at(query).get<std::string>();
        }
        return index_has ? (engine_found && index_answer == engine_entry.value("path", "")
                                ? "agrees" : "differs")
                         : "absent";
    }
    if (query.rfind("res://", 0) == 0) {
        const auto found = path_to_uid.find(query);
        if (found != path_to_uid.end()) {
            index_has = true;
            index_answer = found->second;
        }
        return index_has ? (engine_found && index_answer == engine_entry.value("uid", "")
                                ? "agrees" : "differs")
                         : "absent";
    }
    return "absent";
}

} // namespace

CallToolResult handleProjectGetUidMap(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    if (!args.is_object()) {
        return CallToolResult::error("Invalid uid map request: arguments must be an object");
    }
    for (const auto& entry : args.items()) {
        if (entry.key() != "resolve") {
            return CallToolResult::error("Invalid uid map request: unknown parameter " + entry.key());
        }
    }
    std::vector<std::string> queries;
    if (args.contains("resolve")) {
        const auto& value = args["resolve"];
        if (!value.is_array() || value.empty() || value.size() > 256) {
            return CallToolResult::error(
                "Invalid uid map request: resolve must be an array of 1 to 256 strings");
        }
        for (const auto& item : value) {
            if (!item.is_string() || item.get<std::string>().empty()) {
                return CallToolResult::error(
                    "Invalid uid map request: resolve must contain only non-empty strings");
            }
            queries.push_back(item.get<std::string>());
        }
    }

    const auto indexer = offline::ResourceIndexer::sharedIndex(".");
    auto all_res = indexer->query("res://");

    json uid_map = json::object();
    std::map<std::string, std::string> path_to_uid;
    for (const auto& r : all_res) {
        if (!r.uid.empty()) {
            uid_map[r.uid] = r.path;
            path_to_uid[r.path] = r.uid;
        }
    }

    json payload{
        {"total_uids", uid_map.size()},
        {"uid_map", uid_map},
        // The map is always the file scan. ResourceUID resolves an id or a path
        // it is given but exposes no way to enumerate its table through
        // GDExtension, so a live enumeration would be an invented claim.
        {"uid_map_source", "project_files"}
    };

    if (queries.empty()) {
        payload["execution_mode"] = "offline_fallback";
        payload["is_live_engine"] = false;
        return CallToolResult::successJson(std::move(payload));
    }

    if (ipc && ipc->isConnected()) {
        auto response = ipc->sendRequest("project.resolveUids", json{{"queries", queries}},
                                         ipc::kWaitForDefinitiveResponse);
        if (response.isOk() && response.value().is_object() &&
            response.value().contains("entries") && response.value()["entries"].is_array()) {
            json resolved = json::array();
            for (auto entry : response.value()["entries"]) {
                entry["source"] = "engine";
                entry["index_state"] = uidIndexAgreement(uid_map, path_to_uid, entry);
                resolved.push_back(std::move(entry));
            }
            payload["execution_mode"] = "live";
            payload["is_live_engine"] = true;
            payload["resolved"] = std::move(resolved);
            return CallToolResult::successJson(std::move(payload));
        }
    }

    json resolved = json::array();
    for (const auto& query : queries) {
        json entry{{"query", query}, {"found", false}, {"uid", ""}, {"path", ""},
                   {"source", "index"}};
        if (query.rfind("uid://", 0) == 0) {
            if (uid_map.contains(query)) {
                entry["found"] = true;
                entry["uid"] = query;
                entry["path"] = uid_map.at(query);
            } else {
                // Not the same claim as "does not exist". An imported asset
                // whose .import has not been written yet is unknown here and
                // known to a running editor.
                entry["reason"] = "not_in_project_files";
            }
        } else if (query.rfind("res://", 0) == 0) {
            const auto found = path_to_uid.find(query);
            if (found != path_to_uid.end()) {
                entry["found"] = true;
                entry["uid"] = found->second;
                entry["path"] = query;
            } else {
                entry["reason"] = "not_in_project_files";
            }
        } else {
            entry["reason"] = "unsupported_query";
        }
        resolved.push_back(std::move(entry));
    }
    payload["execution_mode"] = "offline_fallback";
    payload["is_live_engine"] = false;
    payload["resolved"] = std::move(resolved);
    return CallToolResult::successJson(std::move(payload));
}

CallToolResult handleInstantiateAsset(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    std::string asset_path = args.value("asset_path", "");
    std::string parent_path = args.value("parent_path", "/root");

    if (asset_path.empty()) {
        return CallToolResult::error("Parameter 'asset_path' is required.");
    }

    if (ipc && ipc->isConnected()) {
        auto res = ipc->sendRequest("asset.instantiate", args);
        if (res.isOk()) {
            return CallToolResult::successJson(res.value());
        }
        return CallToolResult::error("Failed to instantiate asset in Godot: " + res.error().message);
    }

    return CallToolResult::error("Godot Editor is offline. Launch Godot Editor to instantiate assets directly into the scene tree.");
}

CallToolResult handleAssetReimport(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    if (!args.is_object() || !args.contains("paths") || !args["paths"].is_array() ||
        args["paths"].empty() || args["paths"].size() > 256) {
        return CallToolResult::error("Invalid asset reimport request: paths must be an array of 1 to 256 strings");
    }
    std::set<std::string> unique;
    for (const auto& value : args["paths"]) {
        if (!value.is_string()) {
            return CallToolResult::error("Invalid asset reimport request: paths must contain only strings");
        }
        const auto path = value.get<std::string>();
        const auto remainder = strings::startsWith(path, "res://") ? path.substr(6) : std::string();
        if (path.size() < 7 || path.size() > 1024 || !strings::startsWith(path, "res://") ||
            path.find('\0') != std::string::npos || path.find("..") != std::string::npos ||
            path.find('\\') != std::string::npos || strings::startsWith(path, "res://.godot/") ||
            strings::endsWith(path, ".import") || remainder.empty() || remainder.front() == '/' ||
            remainder.find("//") != std::string::npos || strings::startsWith(remainder, "./") ||
            remainder.find("/./") != std::string::npos || strings::endsWith(remainder, "/.") ||
            remainder.find(':') != std::string::npos) {
            return CallToolResult::error("Invalid asset reimport request: every path must be a normalized project-owned res:// source asset");
        }
        if (!unique.insert(path).second) {
            return CallToolResult::error("Invalid asset reimport request: paths must be unique");
        }
    }
    if (args.contains("timeout_ms") &&
        (!args["timeout_ms"].is_number_integer() || args["timeout_ms"].get<int64_t>() < 1 ||
         args["timeout_ms"].get<int64_t>() > 10000)) {
        return CallToolResult::error("Invalid asset reimport request: timeout_ms must be an integer from 1 to 10000");
    }
    if (!ipc || !ipc->isConnected()) {
        return CallToolResult::error("Godot Editor is offline. Launch Godot to reimport assets.");
    }
    auto response = ipc->sendRequest("asset.reimport", args, ipc::kWaitForDefinitiveResponse);
    // A reimport rewrites .import sidecars and can change uids, so the shared
    // scan is no longer trustworthy whether the call succeeded or not.
    offline::ResourceIndexer::invalidateSharedIndex();
    if (response.isErr()) {
        return CallToolResult::error("Failed to reimport assets: " + response.error().message);
    }
    return CallToolResult::successJson(response.value());
}

} // namespace mcp
} // namespace didi
