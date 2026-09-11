#include "didi/offline/resource_indexer.hpp"
#include "didi/common/logger.hpp"
#include "didi/common/project_path.hpp"
#include <fstream>
#include <regex>
#include <algorithm>
#include <array>
#include <chrono>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace didi {
namespace offline {

namespace fs = std::filesystem;

namespace {

bool isValidUid(const std::string& uid) {
    static const std::regex uid_regex(R"re(^uid:\/\/[a-z0-9]+$)re");
    return std::regex_match(uid, uid_regex);
}

void foldAsciiLower(std::string& value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        if (character >= 'A' && character <= 'Z') {
            return static_cast<char>(character + ('a' - 'A'));
        }
        return static_cast<char>(character);
    });
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

// Godot writes no .uid sidecar for an imported asset. Its UID lives in the
// .import file the import pipeline already generates beside it, so a texture,
// audio clip or mesh never appeared in the map and project_audit_assets called
// every uid:// reference to one a broken reference.
std::string extractUidFromImportMetadata(const fs::path& file_path) {
    auto import_path = file_path;
    import_path += ".import";
    std::ifstream metadata(import_path);
    if (!metadata.is_open()) return "";
    static const std::regex uid_regex(R"re(^\s*uid\s*=\s*"(uid:\/\/[^"]+)")re");
    std::string line;
    int count = 0;
    while (std::getline(metadata, line) && count++ < 32) {
        std::smatch match;
        if (std::regex_search(line, match, uid_regex) && match.size() > 1) {
            const auto uid = match[1].str();
            if (isValidUid(uid)) return uid;
        }
    }
    return "";
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

// --- Per file memo ----------------------------------------------------------
//
// Rebuilding the index re-opens every scene, resource and script to pull its
// uid and dependencies. On a project of a few thousand files that is most of
// the cost of a scan, and it is repeated whether or not anything changed.
//
// A file whose size and modification time both match what was read last time
// cannot have different contents in any way this cares about, so the extracted
// facts are kept and the read is skipped. That is the same assumption a build
// system makes about a timestamp.
//
// Didi's own writes do not rely on it: invalidateSharedIndex drops this too, so
// a mutation is never followed by a memo hit on the file it just wrote.
namespace {

// The type a .tres or .res actually declares, read from the header Godot reads
// it from. The extension only ever says "Resource", so a CanvasItemMaterial and
// a file whose type is not a Godot class at all were reported identically, and
// the tool named inspect could not answer what the resource is (#467).
std::string extractDeclaredTypeFromPath(const fs::path& file_path) {
    std::ifstream file(file_path, std::ios::binary);
    if (!file.is_open()) return {};
    // A bounded read rather than getline. A .res is the binary spelling of the
    // same extension and may carry no newline at all, and reading one into a
    // string to look at its first line is a file-sized allocation for a header
    // that is never longer than this.
    char buffer[512];
    file.read(buffer, sizeof(buffer));
    std::string line(buffer, static_cast<size_t>(file.gcount()));
    const auto newline = line.find('\n');
    if (newline != std::string::npos) line.resize(newline);
    static const std::regex type_regex(R"re(^\[gd_resource[^\]]*type="([^"]+)")re");
    std::smatch match;
    if (std::regex_search(line, match, type_regex) && match.size() > 1) {
        const auto declared = match[1].str();
        // A header long enough to be a payload rather than a class name is not
        // one, and this value is echoed back to a caller.
        if (!declared.empty() && declared.size() <= 128) return declared;
    }
    return {};
}

struct FileFacts {
    std::string uid;
    std::string declared_type;
    std::vector<std::string> dependencies;
};

struct FileMemoEntry {
    std::filesystem::file_time_type modified;
    uintmax_t size{0};
    FileFacts facts;
};

std::mutex& fileMemoMutex() {
    static std::mutex mutex;
    return mutex;
}

std::unordered_map<std::filesystem::path::string_type, FileMemoEntry>& fileMemo() {
    static std::unordered_map<std::filesystem::path::string_type, FileMemoEntry> memo;
    return memo;
}

bool lookupFileFacts(const std::filesystem::path::string_type& key,
                     std::filesystem::file_time_type modified, uintmax_t size, FileFacts& out) {
    std::lock_guard<std::mutex> lock(fileMemoMutex());
    const auto found = fileMemo().find(key);
    if (found == fileMemo().end()) return false;
    if (found->second.modified != modified || found->second.size != size) return false;
    out = found->second.facts;
    return true;
}

void rememberFileFacts(const std::filesystem::path::string_type& key,
                       std::filesystem::file_time_type modified, uintmax_t size,
                       const FileFacts& facts) {
    std::lock_guard<std::mutex> lock(fileMemoMutex());
    auto& memo = fileMemo();
    // One entry per indexable file is the natural ceiling. Past it the memo is
    // holding paths that no longer exist, so it is cheaper to start again than
    // to track deletions.
    if (memo.size() > ResourceIndexer::kMaxIndexedResources) memo.clear();
    memo[key] = FileMemoEntry{modified, size, facts};
}

void clearFileMemo() {
    std::lock_guard<std::mutex> lock(fileMemoMutex());
    fileMemo().clear();
}

} // namespace

ResourceIndexer::ResourceIndexer() {}

std::string ResourceIndexer::detectResourceType(const std::string& ext) {
    std::string lower = ext;
    foldAsciiLower(lower);

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
    m_truncated = false;

    try {
        std::error_code root_error;
        const fs::path root_path = fs::weakly_canonical(
            paths::projectPathFromUtf8(root_dir), root_error);
        if (root_error || !fs::exists(root_path)) return;

        for (auto it = fs::recursive_directory_iterator(root_path, fs::directory_options::skip_permission_denied);
             it != fs::recursive_directory_iterator(); ++it) {
            const auto& entry = *it;
            // A res:// symlink pointing outside the project is not a project
            // resource. project_search already refuses to follow them; the index
            // has to agree, or a listing can name a file outside the root.
            std::error_code symlink_error;
            if (fs::is_symlink(entry.symlink_status(symlink_error)) || symlink_error) {
                if (entry.is_directory(symlink_error)) it.disable_recursion_pending();
                continue;
            }
            if (entry.is_directory()) {
                const auto name = entry.path().filename();
                // build-clean and build-vs used to be named one at a time, so
                // build-ninja was indexed in full. Every artifact in it counts
                // against kMaxIndexedResources, and a large enough build tree
                // pushed real project files out of the index entirely. Prefix,
                // for the same reason project_search uses one.
                const auto name_text = paths::projectPathToUtf8(name);
                // .didi is this server's own blackboard and crash state. An agent
                // auditing a project saw two files it did not create and the user
                // did not either (#468).
                if (name == ".godot" || name == ".git" || name == "build" || name == ".gemini" ||
                    name == ".vs" || name == "out" || name == "bin" || name == ".worktrees" ||
                    name == ".didi" || strings::startsWith(name_text, "build-")) {
                    it.disable_recursion_pending();
                }
                continue;
            }

            if (entry.is_regular_file()) {
                if (m_resources.size() >= kMaxIndexedResources) {
                    m_truncated = true;
                    break;
                }
                // Convert to res:// relative path
                const auto rel = fs::relative(entry.path(), root_path);
                const std::string rel_path = "res://" + paths::projectPathToUtf8(rel);

                const std::string ext = paths::projectPathToUtf8(entry.path().extension());
                const std::string filename = paths::projectPathToUtf8(entry.path().filename());
                std::string type = detectResourceType(ext);

                uintmax_t file_size = 0;
                std::error_code size_error;
                file_size = entry.file_size(size_error);
                if (size_error) file_size = 0;
                std::error_code time_error;
                const auto modified = entry.last_write_time(time_error);

                // The walk is cheap; opening every scene, resource and script
                // to pull its uid and its dependencies is not, and that work is
                // repeated in full every time the index is rebuilt. A file that
                // has not been written since the last read cannot have new
                // contents, so the answer is kept and the read is skipped.
                FileFacts facts;
                const auto native = entry.path().native();
                const bool stampable = !size_error && !time_error;
                if (!stampable || !lookupFileFacts(native, modified, file_size, facts)) {
                    if (type == "PackedScene" || type == "Resource" || type == "GDScript") {
                        facts.uid = extractUidFromPath(entry.path());
                        // .tres and .res carry [ext_resource] references to
                        // textures, scripts and shaders exactly as scenes do, so
                        // a material or theme has a dependency graph too.
                        if (type == "PackedScene" || type == "Resource") {
                            facts.dependencies = extractDependenciesFromPath(entry.path());
                        }
                        // The file is already being read for its dependencies,
                        // so the type on its first line costs nothing more and
                        // is memoized with the rest.
                        if (type == "Resource") {
                            facts.declared_type = extractDeclaredTypeFromPath(entry.path());
                        }
                    } else if (ext != ".uid" && ext != ".import") {
                        facts.uid = extractUidSidecar(entry.path());
                        if (facts.uid.empty()) {
                            facts.uid = extractUidFromImportMetadata(entry.path());
                        }
                    }
                    if (stampable) rememberFileFacts(native, modified, file_size, facts);
                }
                std::string uid = facts.uid;
                std::vector<std::string> deps = std::move(facts.dependencies);

                ResourceInfo info;
                info.path = rel_path;
                info.filename = filename;
                info.type = type;
                info.uid = uid;
                info.declared_type = facts.declared_type;
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
    foldAsciiLower(lower_fuzzy);

    std::string lower_type = type_filter;
    foldAsciiLower(lower_type);

    // A directory filter, not a prefix filter. "res://scenes" must match
    // res://scenes/player.tscn and res://scenes itself, and must not match
    // res://scenes_v2/level.tscn.
    std::string directory = search_path;
    if (!directory.empty() && directory.back() != '/') directory.push_back('/');

    for (const auto& res : m_resources) {
        // Path filter
        if (search_path != "res://" && res.path != search_path &&
            !strings::startsWith(res.path, directory)) {
            continue;
        }

        // Type filter
        if (!lower_type.empty()) {
            std::string res_type = res.type;
            foldAsciiLower(res_type);
            if (res_type.find(lower_type) == std::string::npos) {
                continue;
            }
        }

        // Fuzzy filter
        if (!lower_fuzzy.empty()) {
            std::string path_lower = res.path;
            foldAsciiLower(path_lower);
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

const ResourceInfo* ResourceIndexer::findExact(const std::string& resource_path) const {
    for (const auto& res : m_resources) {
        if (res.path == resource_path) return &res;
    }
    return nullptr;
}

json ResourceIndexer::buildProjectTree(const std::string& root_dir) const {
    const auto indexer = sharedIndex(root_dir);
    auto all_res = indexer->query("res://");

    json root = {
        {"project_root", root_dir},
        {"total_resources", all_res.size()},
        {"resources", json::array()}
    };
    if (indexer->truncated()) root["truncated"] = true;

    for (const auto& r : all_res) {
        root["resources"].push_back(r.toJson());
    }
    return root;
}

// --- Shared scan ------------------------------------------------------------
//
// Every read tool used to construct its own indexer and walk the whole project
// again, so a run of resource_inspect, project_list_resources and
// project_get_uid_map crawled the tree three times. One scan is kept per root
// and reused for a short window, and the mutating tools drop it explicitly so
// our own writes are never served stale.
//
// The window is deliberately short. It exists to collapse a burst of sequential
// MCP calls, not to hold an index across a working session; #112 tracks a real
// file system watcher.
namespace {

struct SharedIndexEntry {
    std::shared_ptr<const ResourceIndexer> index;
    std::chrono::steady_clock::time_point scanned_at;
};

std::mutex& sharedIndexMutex() {
    static std::mutex mutex;
    return mutex;
}

std::unordered_map<std::string, SharedIndexEntry>& sharedIndexCache() {
    static std::unordered_map<std::string, SharedIndexEntry> cache;
    return cache;
}

// The cache key has to name the directory, not the spelling of it. Most tools
// ask for ".", so keying on the string handed in files two different projects
// under one key and serves each the other's index. Resolving first is also what
// lets two tools that spell the same project differently share one crawl, which
// is the entire point of the cache.
//
// A path that cannot be resolved keys on the normalised absolute form, and if
// even that fails, on what was passed. Failing to share a crawl costs a scan.
std::string sharedIndexKey(const std::string& root_dir) {
    std::error_code error;
    const auto resolved = fs::weakly_canonical(fs::path(root_dir), error);
    if (!error && !resolved.empty()) return resolved.lexically_normal().string();
    const auto absolute = fs::absolute(fs::path(root_dir), error);
    if (!error && !absolute.empty()) return absolute.lexically_normal().string();
    return root_dir;
}

} // namespace

std::shared_ptr<const ResourceIndexer> ResourceIndexer::sharedIndex(const std::string& root_dir) {
    const auto key = sharedIndexKey(root_dir);
    const auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(sharedIndexMutex());
        auto& cache = sharedIndexCache();
        const auto found = cache.find(key);
        if (found != cache.end() && now - found->second.scanned_at < kSharedIndexLifetime) {
            return found->second.index;
        }
    }

    auto fresh = std::make_shared<ResourceIndexer>();
    fresh->scan(root_dir);

    std::lock_guard<std::mutex> lock(sharedIndexMutex());
    sharedIndexCache()[key] = SharedIndexEntry{fresh, std::chrono::steady_clock::now()};
    return fresh;
}

void ResourceIndexer::invalidateSharedIndex() {
    // The per file memo goes with it. A mutation that rewrites a file to the
    // same size inside the filesystem's timestamp resolution would otherwise be
    // served the contents it just replaced.
    clearFileMemo();
    std::lock_guard<std::mutex> lock(sharedIndexMutex());
    sharedIndexCache().clear();
}

} // namespace offline
} // namespace didi
