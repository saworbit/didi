#pragma once

#include "didi/common/types.hpp"

#include <algorithm>
#include <cctype>
#include <cwctype>
#include <filesystem>
#include <string>

namespace didi::paths {

inline auto normalizedProjectPath(const std::filesystem::path& path) {
#if defined(_WIN32)
    auto value = path.lexically_normal().generic_wstring();
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t character) {
        return static_cast<wchar_t>(std::towlower(character));
    });
#else
    auto value = path.lexically_normal().generic_string();
#endif
    return value;
}

inline std::filesystem::path projectPathFromUtf8(const std::string& value) {
#if defined(_WIN32)
    const auto* begin = reinterpret_cast<const char8_t*>(value.data());
    return std::filesystem::path(std::u8string(begin, begin + value.size()));
#else
    return std::filesystem::path(value);
#endif
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
    std::filesystem::path relative;
    try {
        relative = projectPathFromUtf8(relative_value);
    } catch (const std::filesystem::filesystem_error&) {
        return Error::invalidArgument("file path must be valid UTF-8");
    }
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
