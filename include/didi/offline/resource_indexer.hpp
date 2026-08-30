#pragma once

#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <unordered_map>
#include <filesystem>
#include "didi/common/types.hpp"
#include "didi/common/json.hpp"

namespace didi {
namespace offline {

struct ResourceInfo {
    std::string path;        // e.g. "res://scenes/player.tscn"
    std::string filename;    // "player.tscn"
    std::string type;        // "PackedScene", "GDScript", "Texture2D", "Mesh", etc.
    std::string uid;         // "uid://..." if found
    uintmax_t file_size{0};
    std::vector<std::string> dependencies;

    json toJson() const {
        return {
            {"path", path},
            {"filename", filename},
            {"type", type},
            {"uid", uid},
            {"file_size", file_size},
            {"dependencies", dependencies}
        };
    }
};

class ResourceIndexer {
public:
    // An index this large already exceeds anything an MCP client can usefully
    // read in one call, and a scan past it just stalls the stdio transport.
    static constexpr size_t kMaxIndexedResources = 20000;
    static constexpr std::chrono::milliseconds kSharedIndexLifetime{2000};

    ResourceIndexer();

    void scan(const std::string& root_dir = ".");

    // search_path is a directory, not a prefix: "res://scenes" matches
    // res://scenes/player.tscn and res://scenes itself, never res://scenes_v2.
    std::vector<ResourceInfo> query(const std::string& search_path = "res://",
                                    const std::string& type_filter = "",
                                    const std::string& fuzzy_query = "",
                                    bool include_uid = true) const;

    // Exact path lookup. query() cannot answer this, because a directory filter
    // for "res://player.gd" also matches nothing while a prefix filter would
    // have matched res://player.gd.uid and res://player.gdextension.
    const ResourceInfo* findExact(const std::string& resource_path) const;

    bool truncated() const { return m_truncated; }

    json buildProjectTree(const std::string& root_dir = ".") const;

    // One scan per project root, reused for kSharedIndexLifetime. Mutating
    // tools must call invalidateSharedIndex after they change the tree.
    static std::shared_ptr<const ResourceIndexer> sharedIndex(const std::string& root_dir = ".");
    static void invalidateSharedIndex();

    static std::string detectResourceType(const std::string& extension);
    static std::string extractUidFromFile(const std::string& file_path);
    static std::vector<std::string> extractDependenciesFromFile(const std::string& file_path);

private:
    std::vector<ResourceInfo> m_resources;
    std::unordered_map<std::string, std::string> m_uidMap;
    bool m_truncated{false};
};

} // namespace offline
} // namespace didi
