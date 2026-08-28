#pragma once

#include "didi/common/types.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>

namespace didi::paths {

inline std::string normalizedProjectPath(const std::filesystem::path& path) {
    auto value = path.lexically_normal().generic_string();
#if defined(_WIN32)
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
#endif
    return value;
}

inline bool isWithinProject(const std::filesystem::path& root,
                            const std::filesystem::path& candidate) {
    const auto root_value = normalizedProjectPath(root);
    const auto candidate_value = normalizedProjectPath(candidate);
    return candidate_value == root_value ||
           (candidate_value.size() > root_value.size() &&
            candidate_value.compare(0, root_value.size(), root_value) == 0 &&
            candidate_value[root_value.size()] == '/');
}

inline Result<std::filesystem::path> resolveProjectFile(const std::string& file_path) {
    if (file_path.empty()) return Error::invalidArgument("file path is empty");
    std::string relative_value = file_path;
    if (strings::startsWith(relative_value, "res://")) relative_value.erase(0, 6);
    const std::filesystem::path relative(relative_value);
    if (relative.is_absolute() || relative.has_root_name()) {
        return Error::invalidArgument("file path must be relative to the project root");
    }
    for (const auto& component : relative) {
        if (component == "..") {
            return Error::invalidArgument("file path cannot contain parent traversal");
        }
    }

    std::error_code error;
    const auto root = std::filesystem::weakly_canonical(
        std::filesystem::current_path(), error);
    if (error) return Error::internal("project root cannot be resolved");
    const auto target = std::filesystem::weakly_canonical(root / relative, error);
    if (error || !isWithinProject(root, target)) {
        return Error::invalidArgument("file path resolves outside the project root");
    }
    if (!std::filesystem::is_regular_file(target, error) || error) {
        return Error::notFound("file does not exist beneath the project root");
    }
    return target;
}

} // namespace didi::paths
