#pragma once

#include "didi/common/types.hpp"

#include <algorithm>
#include <cctype>
#include <cwctype>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>

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

// Whether a byte string is well-formed UTF-8, and a lossy rendering of one
// that is not.
//
// A POSIX filename is a byte string: the only bytes it may not hold are '/'
// and NUL, so a .gd copied off an old drive or unpacked from a Latin-1 zip is
// a legal file with a name that is not UTF-8. On Windows a name is UTF-16 and
// always converts, which is why this only bites on Unix; the default macOS
// volume refuses the name outright, so it is Linux and the other Unix
// filesystems that can hold one.
//
// JSON is defined over Unicode, so such a path cannot be put in a response at
// all: serialising it threw, four project walkers died on the throw, and each
// reported that an argument the caller never sent had the wrong type (#650).
// Naming the file is the one thing a user needs here, so the lossy form exists
// to say which file it was.
inline bool isDecodableUtf8(std::string_view value) {
    size_t index = 0;
    while (index < value.size()) {
        const auto lead = static_cast<unsigned char>(value[index]);
        size_t length = 0;
        unsigned int code_point = 0;
        if (lead < 0x80) {
            ++index;
            continue;
        }
        if ((lead & 0xE0) == 0xC0) { length = 2; code_point = lead & 0x1Fu; }
        else if ((lead & 0xF0) == 0xE0) { length = 3; code_point = lead & 0x0Fu; }
        else if ((lead & 0xF8) == 0xF0) { length = 4; code_point = lead & 0x07u; }
        else return false;
        if (index + length > value.size()) return false;
        for (size_t offset = 1; offset < length; ++offset) {
            const auto continuation = static_cast<unsigned char>(value[index + offset]);
            if ((continuation & 0xC0) != 0x80) return false;
            code_point = (code_point << 6) | (continuation & 0x3Fu);
        }
        // Overlong forms, surrogates and anything past U+10FFFF are all
        // rejected, because a decoder that accepts them lets the same character
        // have two spellings and JSON does not.
        if ((length == 2 && code_point < 0x80) || (length == 3 && code_point < 0x800) ||
            (length == 4 && code_point < 0x10000) || code_point > 0x10FFFF ||
            (code_point >= 0xD800 && code_point <= 0xDFFF)) {
            return false;
        }
        index += length;
    }
    return true;
}

// How many characters a UTF-8 byte string holds, which is what JSON Schema
// means by the length of a string: "the number of its characters as defined by
// RFC 8259". Counting bytes instead enforced a bound a third as generous as the
// one published for anything outside ASCII (#663).
inline size_t codePointCount(std::string_view value) {
    size_t count = 0;
    for (const unsigned char byte : value) {
        if ((byte & 0xC0) != 0x80) ++count;
    }
    return count;
}

// The same string with every byte that is not part of a well-formed sequence
// replaced by U+FFFD, so it can be named in a response.
inline std::string lossyUtf8(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    size_t index = 0;
    while (index < value.size()) {
        size_t length = 1;
        const auto lead = static_cast<unsigned char>(value[index]);
        if (lead >= 0x80) {
            if ((lead & 0xE0) == 0xC0) length = 2;
            else if ((lead & 0xF0) == 0xE0) length = 3;
            else if ((lead & 0xF8) == 0xF0) length = 4;
            else length = 0;
        }
        if (length != 0 && index + length <= value.size() &&
            isDecodableUtf8(value.substr(index, length))) {
            out.append(value.substr(index, length));
            index += length;
            continue;
        }
        out += "\xEF\xBF\xBD";  // U+FFFD, written as bytes so the source stays ASCII
        ++index;
    }
    return out;
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
    } catch (const std::exception&) {
        // Wider than filesystem_error on purpose. On Windows the conversion
        // from UTF-8 to the native wide path throws std::system_error ("No
        // mapping for the Unicode character exists in the target multi-byte
        // code page"), which is not a filesystem_error, so the narrower catch
        // let it escape main and the process fast-failed with 0xC0000409 and
        // no output at all (#611). The message below was written for this case
        // and could never be reached.
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

// The res:// spelling of a resolved path, which is the spelling every reader
// reports. A writer that echoed its argument named res://d1/../x.gd for a file
// that landed at res://x.gd, through a directory that was never created, and
// on Windows named res://PLAYER.gd for a file that only ever existed as
// res://player.gd; nothing else on the surface produces either string, so a
// caller storing the answer stored a name no later comparison would match
// (#546, #551). When the file is already there its on-disk spelling is used,
// which on a case-insensitive filesystem is the one fact the argument does not
// carry: the file the write is about to replace.
inline std::string resourcePathOf(const std::filesystem::path& resolved) {
    std::error_code error;
    const auto root = std::filesystem::weakly_canonical(std::filesystem::current_path(error), error);
    if (error) return projectPathToUtf8(resolved);
    auto target = resolved;
    if (std::filesystem::exists(target, error) && !error) {
        const auto on_disk = std::filesystem::canonical(target, error);
        if (!error) target = on_disk;
    }
    const auto relative = target.lexically_relative(root);
    if (relative.empty() || *relative.begin() == "..") return projectPathToUtf8(target);
    return "res://" + projectPathToUtf8(relative);
}

} // namespace didi::paths
