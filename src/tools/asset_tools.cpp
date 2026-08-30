#include "didi/mcp/mcp_protocol.hpp"
#include "didi/common/ipc_channel.hpp"
#include "didi/common/logger.hpp"
#include "didi/offline/resource_indexer.hpp"
#include "didi/common/project_path.hpp"
#include "didi/common/atomic_write.hpp"
#include <fstream>
#include <initializer_list>
#include <sstream>
#include <set>

namespace didi {
namespace mcp {

CallToolResult handleQueryProjectResources(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    (void)ipc;
    std::string search_path = args.value("search_path", "res://");
    std::string type_filter = args.value("type_filter", "");
    std::string fuzzy_query = args.value("fuzzy_query", "");
    bool include_uid = args.value("include_uid", true);

    offline::ResourceIndexer indexer;
    indexer.scan(".");
    auto results = indexer.query(search_path, type_filter, fuzzy_query);

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

    offline::ResourceIndexer indexer;
    indexer.scan(".");
    auto results = indexer.query(resource_path);
    if (!results.empty()) {
        return CallToolResult::successJson(results[0].toJson());
    }

    return CallToolResult::error("Resource not found: " + resource_path);
}

CallToolResult handleProjectGetUidMap(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    offline::ResourceIndexer indexer;
    indexer.scan(".");
    auto all_res = indexer.query("res://");

    json uid_map = json::object();
    for (const auto& r : all_res) {
        if (!r.uid.empty()) {
            uid_map[r.uid] = r.path;
        }
    }

    return CallToolResult::successJson({
        {"total_uids", uid_map.size()},
        {"uid_map", uid_map}
    });
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
    if (response.isErr()) {
        return CallToolResult::error("Failed to reimport assets: " + response.error().message);
    }
    return CallToolResult::successJson(response.value());
}

} // namespace mcp
} // namespace didi
