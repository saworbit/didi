#include "didi/offline/resource_uid.hpp"
#include "didi/offline/resource_indexer.hpp"
#include "didi/common/secure_random.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <set>
#include <utility>

namespace didi::offline {
namespace {

size_t skipSpaceAndComments(const std::string& text, size_t position, size_t limit);

// Only the opening tag owns this resource's UID. An ext_resource tag owns
// another file's UID, and must never supply the identity on an overwrite.
std::pair<size_t, size_t> resourceHeaderRange(const std::string& text) {
    size_t start = text.rfind("\xef\xbb\xbf", 0) == 0 ? 3 : 0;
    while (start < text.size()) {
        if (std::isspace(static_cast<unsigned char>(text[start]))) { ++start; continue; }
        if (text[start] != ';') break;
        start = text.find('\n', start);
        if (start == std::string::npos) return {start, start};
    }
    if (start == text.size() || text[start] != '[') return {std::string::npos, std::string::npos};
    const size_t name = skipSpaceAndComments(text, start + 1, text.size());
    size_t after_name = name;
    while (after_name < text.size() &&
           (std::isalnum(static_cast<unsigned char>(text[after_name])) || text[after_name] == '_')) ++after_name;
    const auto tag = text.substr(name, after_name - name);
    if (tag != "gd_resource" && tag != "gd_scene") return {std::string::npos, std::string::npos};
    if (after_name < text.size() && !std::isspace(static_cast<unsigned char>(text[after_name])) &&
        text[after_name] != ';' && text[after_name] != ']') return {std::string::npos, std::string::npos};
    bool quoted = false, escaped = false;
    size_t nested_brackets = 0;
    for (size_t end = after_name; end < text.size(); ++end) {
        const char character = text[end];
        if (escaped) { escaped = false; continue; }
        if (quoted && character == '\\') { escaped = true; continue; }
        if (!quoted && character == ';') {
            end = text.find('\n', end);
            if (end == std::string::npos) return {start, end};
            continue;
        }
        if (character == '"') quoted = !quoted;
        if (!quoted && character == '[') ++nested_brackets;
        if (!quoted && character == ']') {
            if (nested_brackets) --nested_brackets;
            else return {start, end + 1};
        }
    }
    return {start, std::string::npos};
}

size_t skipSpaceAndComments(const std::string& text, size_t position, size_t limit) {
    while (position < limit) {
        if (std::isspace(static_cast<unsigned char>(text[position]))) { ++position; continue; }
        if (text[position] != ';') break;
        const auto next = text.find('\n', position);
        position = next == std::string::npos ? limit : std::min(next + 1, limit);
    }
    return position;
}

size_t quotedEnd(const std::string& text, size_t position, size_t limit) {
    for (++position; position < limit; ++position) {
        if (text[position] == '\\') { ++position; continue; }
        if (text[position] == '"') return position;
    }
    return limit;
}

std::string decodeUid(const std::string& text, size_t first, size_t last) {
    std::string value;
    for (size_t position = first + 1; position < last; ++position) {
        unsigned int character = static_cast<unsigned char>(text[position]);
        if (character == '\\') {
            if (++position == last) return {};
            character = static_cast<unsigned char>(text[position]);
            if (character == 'u' || character == 'U') {
                const size_t width = character == 'u' ? 4 : 6;
                if (last - position - 1 < width) return {};
                character = 0;
                for (size_t digit = 0; digit < width; ++digit) {
                    const auto hex = static_cast<unsigned char>(text[++position]);
                    if (!std::isxdigit(hex)) return {};
                    character = character * 16 + (hex <= '9' ? hex - '0' : std::tolower(hex) - 'a' + 10);
                }
            } else if (character == 'b' || character == 't' || character == 'n' ||
                       character == 'f' || character == 'r') return {};
        }
        // A ResourceUID cannot contain non-ASCII text, including a surrogate.
        if (character > 127) return {};
        value.push_back(static_cast<char>(character));
    }
    if (!isResourceUidText(value)) return {};
    return value;
}

} // namespace

bool isResourceUidText(std::string_view text) {
    constexpr std::string_view scheme = "uid://";
    if (text.size() <= scheme.size() || text.substr(0, scheme.size()) != scheme) return false;
    for (const char character : text.substr(scheme.size())) {
        if (!((character >= 'a' && character <= 'z') || (character >= '0' && character <= '9'))) {
            return false;
        }
    }
    return true;
}

ResourceUidHeader inspectTextResourceUidHeader(const std::string& text) {
    const auto [start, end] = resourceHeaderRange(text);
    if (start == std::string::npos) return {};
    if (end == std::string::npos) return {true, false, {}};
    std::string uid;
    for (size_t position = start + 1; position < end;) {
        position = skipSpaceAndComments(text, position, end);
        if (position == end) break;
        if (text[position] == '"') { position = quotedEnd(text, position, end) + 1; continue; }
        if (!(std::isalpha(static_cast<unsigned char>(text[position])) || text[position] == '_')) {
            ++position;
            continue;
        }
        const size_t word = position++;
        while (position < end && (std::isalnum(static_cast<unsigned char>(text[position])) || text[position] == '_')) ++position;
        if (text.compare(word, position - word, "uid") != 0) continue;
        position = skipSpaceAndComments(text, position, end);
        if (position == end || text[position] != '=') continue;
        position = skipSpaceAndComments(text, position + 1, end);
        if (position < end && (text[position] == '&' || text[position] == '@')) ++position;
        uid.clear();
        if (position == end || text[position] != '"') continue;
        const size_t closing = quotedEnd(text, position, end);
        if (closing == end) break;
        uid = decodeUid(text, position, closing);
        position = closing + 1;
    }
    return {true, true, std::move(uid)};
}

Result<std::string> resourceUidForWrite(const std::filesystem::path& target) {
    std::error_code error;
    const bool exists = std::filesystem::exists(target, error);
    if (error) return Error(403, "Cannot inspect the existing resource's UID: " + error.message());
    if (exists) {
        std::ifstream previous(target, std::ios::binary);
        if (!previous) return Error(403, "Cannot read the existing resource's UID; the file is unchanged");
        std::array<char, 65536> buffer{};
        previous.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        if (previous.bad()) return Error(500, "Reading the existing resource's UID failed; the file is unchanged");
        const std::string header(buffer.data(), static_cast<size_t>(previous.gcount()));
        const auto facts = inspectTextResourceUidHeader(header);
        if (!facts.complete && header.size() == buffer.size()) {
            return Error(422, "The existing resource header was not found within the first 64 KiB, so its UID cannot be preserved; the file is unchanged");
        }
        if (!facts.uid.empty()) return facts.uid;
    }
    std::set<std::string> known;
    for (const auto& resource : offline::ResourceIndexer::sharedIndex(".")->query()) {
        if (!resource.uid.empty()) known.insert(resource.uid);
    }
    // Godot 4.5 through 4.7 use random nonnegative 63-bit ids and this base-34
    // alphabet, not base 36. See core/io/resource_uid.cpp in the pinned engine.
    constexpr char alphabet[] = "abcdefghijklmnopqrstuvwxy012345678";
    for (int attempt = 0; attempt < 16; ++attempt) {
        auto bytes = security::secureRandomBytes(8);
        if (bytes.isErr()) return bytes.error();
        uint64_t id = 0;
        for (const auto byte : bytes.value()) id = (id << 8) | byte;
        id &= UINT64_C(0x7fffffffffffffff);
        std::string digits;
        do {
            digits.push_back(alphabet[id % (sizeof(alphabet) - 1)]);
            id /= sizeof(alphabet) - 1;
        } while (id);
        std::reverse(digits.begin(), digits.end());
        const auto uid = "uid://" + digits;
        if (!known.count(uid)) return uid;
    }
    return Error::internal("Could not allocate a resource UID distinct from the indexed project UIDs");
}

} // namespace didi::offline
