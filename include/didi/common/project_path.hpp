#pragma once

#include "didi/common/types.hpp"

#include <algorithm>
#include <cctype>
#include <cwctype>
#include <filesystem>
#include <iomanip>
#include <sstream>
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

inline std::string projectPathToUtf8(const std::filesystem::path& path) {
    const auto value = path.generic_u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

// UTF-8 encoding of a path in its native separator form. Use this where the
// separator style is part of an identity that another process also produced,
// such as a runtime session descriptor. Use projectPathToUtf8 for res:// paths.
inline std::string nativePathToUtf8(const std::filesystem::path& path) {
    const auto value = path.u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
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

inline Result<std::filesystem::path> resolveExplicitProjectRoot(const std::string& value) {
    if (value.empty()) {
        return Error::invalidArgument("An explicit Godot project root is required (--project or DIDI_PROJECT_ROOT)");
    }
    std::filesystem::path supplied;
    try {
        supplied = projectPathFromUtf8(value);
    } catch (const std::filesystem::filesystem_error&) {
        return Error::invalidArgument("The project root must be valid UTF-8");
    }
    std::error_code error;
    const auto root = std::filesystem::weakly_canonical(
        std::filesystem::absolute(supplied, error), error);
    if (error || !std::filesystem::is_directory(root, error) || error) {
        return Error::notFound("The explicit project root is not an accessible directory");
    }
    if (!std::filesystem::is_regular_file(root / "project.godot", error) || error) {
        return Error::invalidArgument("The explicit project root must contain project.godot");
    }
    return root;
}

inline std::string projectEndpointKey(const std::filesystem::path& project_root) {
    std::error_code error;
    const auto canonical = std::filesystem::weakly_canonical(project_root, error);
    const auto normalized = normalizedProjectPath(error ? project_root : canonical);
    uint64_t hash = 1469598103934665603ull;
    for (const auto character : normalized) {
        hash ^= static_cast<uint64_t>(character);
        hash *= 1099511628211ull;
    }
    std::ostringstream output;
    output << std::hex << std::setfill('0') << std::setw(16) << hash;
    return output.str();
}

// The same path rules as resolveProjectFile without requiring that something is
// already there. A tool that creates a file has to check the path before it
// exists, and repeating the traversal, UTF-8 and containment rules per writer
// is how two of them end up disagreeing.
//
// Containment is decided by resolving the path and comparing it against the
// project root, not by looking for ".." in the string. A substring test refused
// res://nested/../ok2.gd, which lands inside the root, while accepting
// res://./ok.gd through the same root, and the resolve-and-compare check behind
// it never ran (#534). Composing a path from a directory and a relative name is
// the ordinary way to build one.
inline Result<std::filesystem::path> resolveProjectFileForWrite(const std::string& file_path) {
    if (file_path.empty()) return Error::invalidArgument("file path is empty");
    // A NUL truncates the path at the filesystem boundary, so a name whose
    // string ends in .gd writes a file with no extension at a name the caller
    // never asked for, and every check upstream ran against the longer string.
    // The other control characters are refused with it: blackboard_write
    // already refuses them by name, and there is no reason for two writers in
    // the same server to disagree about what a path may hold.
    for (const unsigned char character : file_path) {
        if (character < 0x20 || character == 0x7F) {
            return Error::invalidArgument("file path cannot contain control characters");
        }
    }
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

    std::error_code error;
    const auto root = std::filesystem::weakly_canonical(
        std::filesystem::current_path(), error);
    if (error) return Error::internal("project root cannot be resolved");
    const auto target = std::filesystem::weakly_canonical(root / relative, error);
    if (error || !isWithinProject(root, target)) {
        return Error::invalidArgument("file path resolves outside the project root");
    }
    return target;
}

inline Result<std::filesystem::path> resolveProjectFile(const std::string& file_path) {
    auto target = resolveProjectFileForWrite(file_path);
    if (target.isErr()) return target;
    std::error_code error;
    if (!std::filesystem::is_regular_file(target.value(), error) || error) {
        return Error::notFound("file does not exist beneath the project root");
    }
    return target;
}

} // namespace didi::paths
