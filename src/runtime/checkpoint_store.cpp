#include "didi/runtime/checkpoint_store.hpp"
#include "didi/common/atomic_write.hpp"
#include "didi/common/project_path.hpp"
#include "didi/common/sha256.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <map>
#include <set>
#include <atomic>
#include <sstream>
#include <stdexcept>
#ifdef _WIN32
#include <windows.h>
#endif

namespace didi::runtime {
namespace {
namespace fs = std::filesystem;
constexpr uint64_t maxBytes = 256ull * 1024 * 1024;
std::atomic<size_t> g_max_files{kDefaultCheckpointMaxFiles};
const json exclusions = {".git",
                         ".godot",
                         ".didi",
                         ".worktrees",
                         "unsaved_editor_state",
                         "undo_history",
                         "user_data",
                         "external_side_effects"};
void require(bool condition, const std::string& message) {
    if (!condition)
        throw std::runtime_error(message);
}
std::string lower(std::string s) {
    for (auto& c : s)
        if (c >= 'A' && c <= 'Z')
            c += 32;
    return s;
}
bool excluded(const fs::path& path) {
    auto n = lower(path.filename().string());
    return n == ".git" || n == ".godot" || n == ".didi" || n == ".worktrees";
}
bool validId(const std::string& id) {
    return id.size() > 3 && id.size() < 100 && id.starts_with("cp-") &&
           std::all_of(id.begin() + 3, id.end(),
                       [](char c) { return (c >= '0' && c <= '9') || c == '-'; });
}
void safeNode(const fs::path& p) {
    auto st = fs::symlink_status(p);
    require(!fs::is_symlink(st), "Links are not supported: " + p.string());
    require(fs::is_directory(st) || fs::is_regular_file(st),
            "Unsupported file type: " + p.string());
#ifdef _WIN32
    auto attr = GetFileAttributesW(p.c_str());
    require(attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_REPARSE_POINT),
            "Reparse paths are not supported: " + p.string());
#endif
    if (fs::is_regular_file(st))
        require(fs::hard_link_count(p) == 1, "Hardlinks are not supported: " + p.string());
}
void safeAncestors(const fs::path& path) {
    auto p = fs::absolute(path).lexically_normal();
    for (;;) {
        if (fs::exists(fs::symlink_status(p)))
            safeNode(p);
        auto parent = p.parent_path();
        if (parent == p || parent.empty())
            break;
        p = parent;
    }
}
bool beneath(const fs::path& a, const fs::path& b) {
    auto x = lower(fs::absolute(a).lexically_normal().generic_string());
    auto y = lower(fs::absolute(b).lexically_normal().generic_string());
    return x == y ||
           (x.size() > y.size() && x.starts_with(y) && (y.back() == '/' || x[y.size()] == '/'));
}
void validRelative(const std::string& name) {
    require(!name.empty() && name.size() <= 4096 && name.find('\\') == std::string::npos &&
                name.find(':') == std::string::npos && name.find('\0') == std::string::npos,
            "Invalid checkpoint path");
    fs::path p = paths::projectPathFromUtf8(name);
    require(!p.is_absolute() && !p.has_root_name() && p.generic_string() == name,
            "Invalid checkpoint path");
    for (const auto& component : p) {
        auto s = component.string();
        auto base = lower(s.substr(0, s.find('.')));
        require(!s.empty() && s != "." && s != ".." && s.back() != '.' && s.back() != ' ' &&
                    !excluded(component),
                "Unsafe checkpoint path");
        require(s.find_first_of("<>\"|?*") == std::string::npos &&
                    std::none_of(s.begin(), s.end(), [](unsigned char c) { return c < 32; }),
                "Unsafe checkpoint path");
        require(base != "con" && base != "prn" && base != "aux" && base != "nul" &&
                    !(base.size() == 4 && (base.starts_with("com") || base.starts_with("lpt")) &&
                      base[3] >= '0' && base[3] <= '9'),
                "Reserved checkpoint path");
    }
}
std::string digest(const std::string& input) { return sha256Hex(input); }
std::string read(const fs::path& p, uint64_t limit) {
    safeAncestors(p);
    safeNode(p);
    auto size = fs::file_size(p);
    require(size <= limit, "Checkpoint size limit exceeded");
    std::ifstream in(p, std::ios::binary);
    require(bool(in), "Cannot read checkpoint file");
    std::string data(static_cast<size_t>(size), '\0');
    in.read(data.data(), static_cast<std::streamsize>(size));
    require(static_cast<uint64_t>(in.gcount()) == size &&
                in.peek() == std::char_traits<char>::eof(),
            "File changed during checkpoint read");
    return data;
}
void write(const fs::path& p, const std::string& data) {
    safeAncestors(p.parent_path());
    fs::create_directories(p.parent_path());
    require(!fs::exists(fs::symlink_status(p)), "Refusing to overwrite checkpoint file");
    std::ofstream out(p, std::ios::binary);
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    out.flush();
    require(bool(out), "Cannot write checkpoint file");
    out.close();
    require(!out.fail(), "Cannot close checkpoint file");
}
struct Tree {
    std::vector<fs::path> files, dirs;
    std::map<fs::path, std::pair<uint64_t, fs::file_time_type>> metadata;
    uint64_t bytes = 0;
    bool filter = true;
};
Tree scan(const fs::path& root, bool filter = true) {
    safeAncestors(root);
    safeNode(root);
    require(fs::is_directory(root), "Project must be a directory");
    Tree tree;
    tree.filter = filter;
    std::set<std::string> names;
    size_t nodes = 0;
    for (fs::recursive_directory_iterator it(root), end; it != end; ++it) {
        require(++nodes <= 30000, "Checkpoint path count limit exceeded");
        safeNode(it->path());
        if (filter && excluded(it->path())) {
            if (it->is_directory())
                it.disable_recursion_pending();
            continue;
        }
        auto rel = it->path().lexically_relative(root);
        auto name = rel.generic_string();
        if (filter)
            validRelative(name);
        require(names.insert(lower(name)).second, "Duplicate checkpoint path");
        if (it->is_directory())
            tree.dirs.push_back(rel);
        else {
            require(tree.files.size() < checkpointMaxFiles(),
                    "Checkpoint file count limit exceeded");
            auto size = it->file_size();
            require(size <= maxBytes - tree.bytes, "Checkpoint byte limit exceeded");
            tree.bytes += size;
            tree.files.push_back(rel);
            tree.metadata.emplace(rel, std::make_pair(size, it->last_write_time()));
        }
    }
    std::sort(tree.files.begin(), tree.files.end());
    std::sort(tree.dirs.begin(), tree.dirs.end());
    return tree;
}
json copyTree(const fs::path& source, const fs::path& destination, const Tree& tree) {
    require(!fs::exists(fs::symlink_status(destination)), "Destination already exists");
    safeAncestors(destination.parent_path());
    fs::create_directory(destination);
    for (const auto& dir : tree.dirs)
        fs::create_directories(destination / dir);
    json entries = json::array();
    uint64_t total = 0;
    for (const auto& rel : tree.files) {
        auto data = read(source / rel, maxBytes - total);
        total += data.size();
        write(destination / rel, data);
        entries.push_back(
            {{"path", rel.generic_string()}, {"size", data.size()}, {"sha256", digest(data)}});
    }
    require(total == tree.bytes, "Project changed during checkpoint copy");
    return entries;
}
void validateSourceStability(const fs::path& source, const Tree& initial, const json& entries) {
    auto unchangedInventory = [&](const Tree& current) {
        require(current.files == initial.files && current.dirs == initial.dirs &&
                    current.metadata == initial.metadata && current.bytes == initial.bytes,
                "Project inventory or metadata changed during checkpoint copy");
    };
    unchangedInventory(scan(source, initial.filter));
    uint64_t total = 0;
    for (const auto& entry : entries) {
        auto data = read(source / paths::projectPathFromUtf8(entry.at("path").get<std::string>()),
                         maxBytes - total);
        total += data.size();
        require(data.size() == entry.at("size").get<uint64_t>() &&
                    digest(data) == entry.at("sha256").get<std::string>(),
                "Project content changed during checkpoint copy");
    }
    // Catch ordinary saves/additions during the digest pass too. These bounded
    // checks are not an atomic filesystem transaction: a malicious concurrent
    // writer can still race the last check and publication or conceal a rewrite.
    unchangedInventory(scan(source, initial.filter));
}
json manifest(const fs::path& folder, const std::string& id) {
    safeAncestors(folder);
    auto j = json::parse(read(folder / "manifest.json", 8 * 1024 * 1024));
    require(j.is_object() && j.at("id") == id && j.at("coverage") == "saved_project_files" &&
                j.at("created_at_ms").is_number_unsigned() && j.at("entries").is_array(),
            "Invalid checkpoint manifest");
    require(j.at("files").is_number_unsigned() && j.at("bytes").is_number_unsigned() &&
                j.at("files").get<uint64_t>() <= checkpointMaxFiles() &&
                j.at("bytes").get<uint64_t>() <= maxBytes &&
                j.at("files") == j.at("entries").size(),
            "Invalid checkpoint bounds");
    require(j.at("directories").is_array() && j.at("directories").size() <= 30000,
            "Invalid checkpoint directories");
    std::set<std::string> paths;
    uint64_t total = 0;
    for (const auto& entry : j.at("entries")) {
        auto name = entry.at("path").get<std::string>();
        validRelative(name);
        require(paths.insert(lower(name)).second, "Duplicate checkpoint path");
        require(entry.at("size").is_number_unsigned(), "Invalid file size");
        auto size = entry.at("size").get<uint64_t>();
        require(size <= maxBytes - total, "Invalid checkpoint size");
        total += size;
        auto hash = entry.at("sha256").get<std::string>();
        require(hash.size() == 64 && std::all_of(hash.begin(), hash.end(),
                                                 [](char c) {
                                                     return (c >= '0' && c <= '9') ||
                                                            (c >= 'a' && c <= 'f');
                                                 }),
                "Invalid content digest");
    }
    for (const auto& dir : j.at("directories")) {
        auto name = dir.get<std::string>();
        validRelative(name);
        require(paths.insert(lower(name)).second, "Duplicate checkpoint directory");
    }
    require(total == j.at("bytes").get<uint64_t>(), "Invalid checkpoint total size");
    return j;
}
std::string newId() {
    static std::atomic<uint64_t> sequence{0};
    auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    return "cp-" + std::to_string(now) + "-" + std::to_string(sequence++);
}
Error failure(const std::exception& e) {
    return Error::internal(std::string("Checkpoint: ") + e.what());
}
json summary(json manifest) {
    manifest.erase("entries");
    manifest.erase("directories");
    return manifest;
}
} // namespace
size_t checkpointMaxFiles() { return g_max_files.load(std::memory_order_relaxed); }

size_t setCheckpointMaxFilesForTesting(size_t files) {
    return g_max_files.exchange(files, std::memory_order_relaxed);
}

CheckpointStore::CheckpointStore(fs::path project, fs::path store)
    : m_project(std::move(project)), m_store(std::move(store)) {}
Result<void> CheckpointStore::initialize(const fs::path& source, const fs::path& container) {
    try {
        safeAncestors(source);
        safeAncestors(container);
        require(!beneath(container, source) && !beneath(source, container),
                "Managed container and source must be disjoint");
        require(!fs::exists(fs::symlink_status(container)), "Managed container must be new");
        auto tree = scan(source);
        require(fs::is_directory(container.parent_path()), "Container parent must exist");
        require(fs::create_directory(container), "Cannot create managed container");
        auto entries = copyTree(source, container / "project", tree);
        validateSourceStability(source, tree, entries);
        fs::create_directory(container / "checkpoints");
        return {};
    } catch (const std::exception& e) {
        return failure(e);
    }
}
Result<json> CheckpointStore::create(const std::string& label) {
    try {
        require(label.size() <= 1024, "Checkpoint label too long");
        require(!beneath(m_store, m_project) && !beneath(m_project, m_store),
                "Checkpoint store and project must be disjoint");
        auto tree = scan(m_project);
        safeAncestors(m_store);
        if (fs::exists(m_store)) {
            size_t count = 0;
            for (const auto& entry : fs::directory_iterator(m_store)) {
                (void)entry;
                require(++count < 1000, "Checkpoint store is full, including incomplete snapshots");
            }
        }
        // Fail before creating another partial when the existing store is corrupt
        // or full. Retained incomplete copies count against this bounded capacity.
        auto existing = list();
        require(existing.isOk(), "Cannot validate checkpoint store before creation");
        fs::create_directories(m_store);
        auto id = newId();
        auto partial = m_store / (".partial-" + id);
        require(fs::create_directory(partial), "Checkpoint already exists");
        // A snapshot that did not publish is removed rather than left behind,
        // where it counted against the store's limit until someone deleted it
        // by hand (#937). One left by a process that died mid-copy still
        // counts, which is what the limit is for.
        struct DiscardUnpublished {
            fs::path path;
            bool published = false;
            ~DiscardUnpublished() {
                if (published) return;
                std::error_code ignored;
                fs::remove_all(path, ignored);
            }
        } unpublished{partial};
        auto entries = copyTree(m_project, partial / "files", tree);
        auto ms = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                            std::chrono::system_clock::now().time_since_epoch())
                                            .count());
        json dirs = json::array();
        for (const auto& d : tree.dirs)
            dirs.push_back(d.generic_string());
        json j = {{"id", id},
                  {"created_at_ms", ms},
                  {"label", label},
                  {"coverage", "saved_project_files"},
                  {"files", entries.size()},
                  {"bytes", tree.bytes},
                  {"exclusions", exclusions},
                  {"entries", entries},
                  {"directories", dirs}};
        write(partial / "manifest.json", j.dump(2));
        validateSourceStability(m_project, tree, entries);
        // With the retry managed restore has. On Windows a scanner or indexer
        // briefly holding a file just copied failed this rename with "Access
        // is denied" about one time in twenty in a test that did nothing else
        // (#937).
        if (const auto error = files::renameWithRetry(partial, m_store / id, std::chrono::seconds(10))) {
            throw fs::filesystem_error("rename", partial, m_store / id, error);
        }
        unpublished.published = true;
        auto completed = list();
        require(completed.isOk(), "Cannot enumerate checkpoint retention");
        for (size_t i = 5; i < completed.value().size(); ++i) {
            auto obsolete = m_store / completed.value()[i].at("id").get<std::string>();
            scan(obsolete / "files", false);
            size_t rootEntries = 0;
            for (const auto& item : fs::directory_iterator(obsolete)) {
                safeNode(item.path());
                require(++rootEntries <= 2 && (item.path().filename() == "files" ||
                                               item.path().filename() == "manifest.json"),
                        "Unexpected snapshot content during retention");
            }
            fs::remove_all(obsolete);
        }
        return summary(std::move(j));
    } catch (const std::exception& e) {
        return failure(e);
    }
}
Result<json> CheckpointStore::list() const {
    try {
        json result = json::array();
        safeAncestors(m_store);
        if (!fs::exists(m_store))
            return result;
        require(fs::is_directory(m_store), "Checkpoint store must be a directory");
        size_t count = 0;
        for (const auto& entry : fs::directory_iterator(m_store)) {
            require(++count <= 1000, "Checkpoint store entry limit exceeded");
            auto id = entry.path().filename().string();
            if (!validId(id))
                continue;
            safeNode(entry.path());
            result.push_back(summary(manifest(entry.path(), id)));
        }
        std::sort(result.begin(), result.end(), [](const json& a, const json& b) {
            if (a.at("created_at_ms") != b.at("created_at_ms"))
                return a.at("created_at_ms") > b.at("created_at_ms");
            return a.at("id") > b.at("id");
        });
        return result;
    } catch (const std::exception& e) {
        return failure(e);
    }
}
Result<void> CheckpointStore::stageRestore(const std::string& id,
                                           const fs::path& destination) const {
    try {
        require(validId(id), "Invalid checkpoint ID");
        require(!beneath(destination, m_store) && !beneath(destination, m_project) &&
                    !beneath(m_store, destination) && !beneath(m_project, destination),
                "Restore destination must be disjoint");
        safeAncestors(destination);
        require(!fs::exists(fs::symlink_status(destination)), "Restore destination must be new");
        auto folder = m_store / id;
        auto j = manifest(folder, id);
        auto tree = scan(folder / "files", false);
        require(tree.files.size() == j.at("files").get<size_t>() &&
                    tree.bytes == j.at("bytes").get<uint64_t>() &&
                    tree.dirs.size() == j.at("directories").size(),
                "Checkpoint content does not match manifest");
        std::set<std::string> actualFiles, actualDirs;
        for (const auto& p : tree.files)
            actualFiles.insert(p.generic_string());
        for (const auto& p : tree.dirs)
            actualDirs.insert(p.generic_string());
        for (const auto& entry : j.at("entries")) {
            auto name = entry.at("path").get<std::string>();
            require(actualFiles.erase(name) == 1, "Missing checkpoint path");
            auto data = read(folder / "files" / paths::projectPathFromUtf8(name), maxBytes);
            require(data.size() == entry.at("size").get<uint64_t>() &&
                        digest(data) == entry.at("sha256").get<std::string>(),
                    "Checkpoint content digest mismatch");
        }
        for (const auto& dir : j.at("directories"))
            require(actualDirs.erase(dir.get<std::string>()) == 1, "Missing checkpoint directory");
        // Copy only after complete validation, and verify the bytes actually staged.
        auto copied = copyTree(folder / "files", destination, tree);
        require(copied == j.at("entries"), "Checkpoint changed while staging restore");
        validateSourceStability(folder / "files", tree, copied);
        return {};
    } catch (const std::exception& e) {
        return failure(e);
    }
}
} // namespace didi::runtime
