#include "didi/offline/resource_indexer.hpp"
#include "didi/common/logger.hpp"
#include "didi/common/project_path.hpp"
#include <fstream>
#include <regex>
#include <algorithm>
#include <array>

namespace didi {
namespace offline {

namespace fs = std::filesystem;

namespace {

bool isValidUid(const std::string& uid) {
    static const std::regex uid_regex(R"re(^uid:\/\/[a-z0-9]+$)re");
    return std::regex_match(uid, uid_regex);
}

std::string extractUidSidecar(const fs::path& file_path) {
    auto sidecar_path = file_path;
    sidecar_path += ".uid";
    std::ifstream sidecar(sidecar_path, std::ios::binary);
    if (!sidecar.is_open()) return "";
    std::array<char, 257> buffer{};
    sidecar.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const auto bytes_read = sidecar.gcount();
    if (bytes_read > 256) return "";
    const std::string uid = strings::trim(
        std::string(buffer.data(), static_cast<size_t>(bytes_read)));
    return isValidUid(uid) ? uid : "";
}

std::string extractUidFromPath(const fs::path& file_path) {
    std::ifstream file(file_path);
    static const std::regex uid_regex(R"re(uid="(uid:\/\/[^"]+)")re");
    if (file.is_open()) {
        std::string line;
        int count = 0;
        while (std::getline(file, line) && count++ < 10) {
            std::smatch match;
            if (std::regex_search(line, match, uid_regex) && match.size() > 1) {
                const auto uid = match[1].str();
                if (isValidUid(uid)) return uid;
            }
        }
    }
    return extractUidSidecar(file_path);
}

std::vector<std::string> extractDependenciesFromPath(const fs::path& file_path) {
    std::vector<std::string> deps;
    std::ifstream file(file_path);
    if (!file.is_open()) return deps;

    std::string line;
    static const std::regex path_regex(R"re(\[ext_resource[^\]]*path="(res:\/\/[^"]+)")re");
    while (std::getline(file, line)) {
        std::smatch match;
        if (std::regex_search(line, match, path_regex) && match.size() > 1) {
            deps.push_back(match[1].str());
        }
    }
    return deps;
}

} // namespace

ResourceIndexer::ResourceIndexer() {}

std::string ResourceIndexer::detectResourceType(const std::string& ext) {
    std::string lower = ext;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

    if (lower == ".tscn" || lower == ".scn") return "PackedScene";
    if (lower == ".tres" || lower == ".res") return "Resource";
    if (lower == ".gd") return "GDScript";
    if (lower == ".cs") return "CSharpScript";
    if (lower == ".png" || lower == ".jpg" || lower == ".jpeg" || lower == ".webp" || lower == ".svg") return "Texture2D";
    if (lower == ".glb" || lower == ".gltf" || lower == ".obj" || lower == ".blend") return "MeshResource";
    if (lower == ".wav" || lower == ".ogg" || lower == ".mp3") return "AudioStream";
    if (lower == ".gdextension") return "GDExtension";
    if (lower == ".gdshader" || lower == ".shader") return "Shader";
    if (lower == ".ttf" || lower == ".otf" || lower == ".woff") return "Font";
    return "GenericResource";
}

std::string ResourceIndexer::extractUidFromFile(const std::string& file_path) {
    return extractUidFromPath(paths::projectPathFromUtf8(file_path));
}

std::vector<std::string> ResourceIndexer::extractDependenciesFromFile(const std::string& file_path) {
    return extractDependenciesFromPath(paths::projectPathFromUtf8(file_path));
}

void ResourceIndexer::scan(const std::string& root_dir) {
    m_resources.clear();
    m_uidMap.clear();

    try {
        fs::path root_path = paths::projectPathFromUtf8(root_dir);
        if (!fs::exists(root_path)) return;

        for (auto it = fs::recursive_directory_iterator(root_path, fs::directory_options::skip_permission_denied);
             it != fs::recursive_directory_iterator(); ++it) {
            const auto& entry = *it;
            if (entry.is_directory()) {
                const auto name = entry.path().filename();
                if (name == ".godot" || name == ".git" || name == "build" || name == ".gemini" || name == ".vs" || name == "out" || name == "bin") {
                    it.disable_recursion_pending();
                }
                continue;
            }

            if (entry.is_regular_file()) {
                // Convert to res:// relative path
                const auto rel = fs::relative(entry.path(), root_path);
                const std::string rel_path = "res://" + paths::projectPathToUtf8(rel);

                const std::string ext = paths::projectPathToUtf8(entry.path().extension());
                const std::string filename = paths::projectPathToUtf8(entry.path().filename());
                std::string type = detectResourceType(ext);
                std::string uid = "";
                std::vector<std::string> deps;

                if (type == "PackedScene" || type == "Resource" || type == "GDScript") {
                    uid = extractUidFromPath(entry.path());
                    if (type == "PackedScene") {
                        deps = extractDependenciesFromPath(entry.path());
                    }
                } else if (ext != ".uid") {
                    uid = extractUidSidecar(entry.path());
                }

                uintmax_t file_size = 0;
                try {
                    file_size = entry.file_size();
                } catch (...) {}

                ResourceInfo info;
                info.path = rel_path;
                info.filename = filename;
                info.type = type;
                info.uid = uid;
                info.file_size = file_size;
                info.dependencies = std::move(deps);

                if (!uid.empty()) {
                    m_uidMap[uid] = rel_path;
                }

                m_resources.push_back(std::move(info));
            }
        }
    } catch (const std::exception& e) {
        DIDI_LOG_WARN("RESOURCE_INDEXER", "Error scanning resources: ", e.what());
    }
}

std::vector<ResourceInfo> ResourceIndexer::query(const std::string& search_path,
                                                const std::string& type_filter,
                                                const std::string& fuzzy_query,
                                                bool include_uid) const {
    std::vector<ResourceInfo> results;
    std::string lower_fuzzy = fuzzy_query;
    std::transform(lower_fuzzy.begin(), lower_fuzzy.end(), lower_fuzzy.begin(), ::tolower);

    std::string lower_type = type_filter;
    std::transform(lower_type.begin(), lower_type.end(), lower_type.begin(), ::tolower);

    for (const auto& res : m_resources) {
        // Path filter
        if (search_path != "res://" && !strings::startsWith(res.path, search_path)) {
            continue;
        }

        // Type filter
        if (!lower_type.empty()) {
            std::string res_type = res.type;
            std::transform(res_type.begin(), res_type.end(), res_type.begin(), ::tolower);
            if (res_type.find(lower_type) == std::string::npos) {
                continue;
            }
        }

        // Fuzzy filter
        if (!lower_fuzzy.empty()) {
            std::string path_lower = res.path;
            std::transform(path_lower.begin(), path_lower.end(), path_lower.begin(), ::tolower);
            if (path_lower.find(lower_fuzzy) == std::string::npos) {
                continue;
            }
        }

        ResourceInfo copy = res;
        if (!include_uid) copy.uid = "";
        results.push_back(std::move(copy));
    }
    return results;
}

json ResourceIndexer::buildProjectTree(const std::string& root_dir) const {
    ResourceIndexer indexer;
    indexer.scan(root_dir);
    auto all_res = indexer.query("res://");

    json root = {
        {"project_root", root_dir},
        {"total_resources", all_res.size()},
        {"resources", json::array()}
    };

    for (const auto& r : all_res) {
        root["resources"].push_back(r.toJson());
    }
    return root;
}

} // namespace offline
} // namespace didi
